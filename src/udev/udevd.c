/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright © 2004 Chris Friesen <chris_friesen@sympatico.ca>
 * Copyright © 2009 Canonical Ltd.
 * Copyright © 2009 Scott James Remnant <scott@netsplit.com>
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "sd-daemon.h"
#include "sd-event.h"

#include "alloc-util.h"
#include "cgroup-util.h"
#include "cpu-set-util.h"
#include "dev-setup.h"
#include "device-monitor-private.h"
#include "device-private.h"
#include "device-util.h"
#include "event-util.h"
#include "fd-util.h"
#include "fileio.h"
#include "format-util.h"
#include "fs-util.h"
#include "hashmap.h"
#include "io-util.h"
#include "limits-util.h"
#include "list.h"
#include "main-func.h"
#include "mkdir.h"
#include "netlink-util.h"
#include "parse-util.h"
#include "pretty-print.h"
#include "proc-cmdline.h"
#include "process-util.h"
#include "selinux-util.h"
#include "signal-util.h"
#include "socket-util.h"
#include "string-util.h"
#include "strv.h"
#include "strxcpyx.h"
#include "syslog-util.h"
#include "udevd.h"
#include "udev-builtin.h"
#include "udev-ctrl.h"
#include "udev-event.h"
#include "udev-util.h"
#include "udev-watch.h"
#include "user-util.h"
#include "version.h"

#define WORKER_NUM_MAX 2048U

static bool arg_debug = false;
static int arg_daemonize = false;
static ResolveNameTiming arg_resolve_name_timing = RESOLVE_NAME_EARLY;
static unsigned arg_children_max = 0;
static usec_t arg_exec_delay_usec = 0;
static usec_t arg_event_timeout_usec = 180 * USEC_PER_SEC;
static int arg_timeout_signal = SIGKILL;
static bool arg_blockdev_read_only = false;

typedef struct Manager {
        sd_event *event;
        Hashmap *workers;
        LIST_HEAD(struct event, events);
        const char *cgroup;
        pid_t pid; /* the process that originally allocated the manager object */
        int log_level;

        UdevRules *rules;
        Hashmap *properties;

        sd_netlink *rtnl;

        sd_device_monitor *monitor;
        struct udev_ctrl *ctrl;
        int worker_watch[2];

        /* used by udev-watch */
        int inotify_fd;
        sd_event_source *inotify_event;

        sd_event_source *kill_workers_event;

        usec_t last_usec;

        bool stop_exec_queue;
        bool exit;
} Manager;

enum event_state {
        EVENT_UNDEF,
        EVENT_QUEUED,
        EVENT_RUNNING,
};

struct event {
        Manager *manager;
        struct worker *worker;
        enum event_state state;

        sd_device *dev;
        sd_device *dev_kernel; /* clone of originally received device */

        uint64_t seqnum;
        uint64_t delaying_seqnum;

        sd_event_source *timeout_warning_event;
        sd_event_source *timeout_event;

        LIST_FIELDS(struct event, event);
};

static void event_queue_cleanup(Manager *manager, enum event_state type);

enum worker_state {
        WORKER_UNDEF,
        WORKER_RUNNING,
        WORKER_IDLE,
        WORKER_KILLED,
        WORKER_KILLING,
};

struct worker {
        Manager *manager;
        pid_t pid;
        sd_device_monitor *monitor;
        enum worker_state state;
        struct event *event;
};

/* passed from worker to main process */
struct worker_message {
};

static void event_free(struct event *event) {
        if (!event)
                return;

        assert(event->manager);

        LIST_REMOVE(event, event->manager->events, event);
        sd_device_unref(event->dev);
        sd_device_unref(event->dev_kernel);

        sd_event_source_unref(event->timeout_warning_event);
        sd_event_source_unref(event->timeout_event);

        if (event->worker)
                event->worker->event = NULL;

        /* only clean up the queue from the process that created it */
        if (LIST_IS_EMPTY(event->manager->events) &&
            event->manager->pid == getpid_cached())
                if (unlink("/run/udev/queue") < 0)
                        log_warning_errno(errno, "Failed to unlink /run/udev/queue: %m");

        free(event);
}

static struct worker* worker_free(struct worker *worker) {
        if (!worker)
                return NULL;

        assert(worker->manager);

        hashmap_remove(worker->manager->workers, PID_TO_PTR(worker->pid));
        sd_device_monitor_unref(worker->monitor);
        event_free(worker->event);

        return mfree(worker);
}

DEFINE_TRIVIAL_CLEANUP_FUNC(struct worker *, worker_free);
DEFINE_PRIVATE_HASH_OPS_WITH_VALUE_DESTRUCTOR(worker_hash_op, void, trivial_hash_func, trivial_compare_func, struct worker, worker_free);

static int worker_new(struct worker **ret, Manager *manager, sd_device_monitor *worker_monitor, pid_t pid) {
        _cleanup_(worker_freep) struct worker *worker = NULL;
        int r;

        assert(ret);
        assert(manager);
        assert(worker_monitor);
        assert(pid > 1);

        /* close monitor, but keep address around */
        device_monitor_disconnect(worker_monitor);

        worker = new(struct worker, 1);
        if (!worker)
                return -ENOMEM;

        *worker = (struct worker) {
                .manager = manager,
                .monitor = sd_device_monitor_ref(worker_monitor),
                .pid = pid,
        };

        r = hashmap_ensure_put(&manager->workers, &worker_hash_op, PID_TO_PTR(pid), worker);
        if (r < 0)
                return r;

        *ret = TAKE_PTR(worker);

        return 0;
}

static int on_event_timeout(sd_event_source *s, uint64_t usec, void *userdata) {
        struct event *event = userdata;

        assert(event);
        assert(event->worker);

        kill_and_sigcont(event->worker->pid, arg_timeout_signal);
        event->worker->state = WORKER_KILLED;

        log_device_error(event->dev, "Worker ["PID_FMT"] processing SEQNUM=%"PRIu64" killed", event->worker->pid, event->seqnum);

        return 1;
}

static int on_event_timeout_warning(sd_event_source *s, uint64_t usec, void *userdata) {
        struct event *event = userdata;

        assert(event);
        assert(event->worker);

        log_device_warning(event->dev, "Worker ["PID_FMT"] processing SEQNUM=%"PRIu64" is taking a long time", event->worker->pid, event->seqnum);

        return 1;
}

static void worker_attach_event(struct worker *worker, struct event *event) {
        sd_event *e;

        assert(worker);
        assert(worker->manager);
        assert(event);
        assert(!event->worker);
        assert(!worker->event);

        worker->state = WORKER_RUNNING;
        worker->event = event;
        event->state = EVENT_RUNNING;
        event->worker = worker;

        e = worker->manager->event;

        (void) sd_event_add_time_relative(e, &event->timeout_warning_event, CLOCK_MONOTONIC,
                                          udev_warn_timeout(arg_event_timeout_usec), USEC_PER_SEC,
                                          on_event_timeout_warning, event);

        (void) sd_event_add_time_relative(e, &event->timeout_event, CLOCK_MONOTONIC,
                                          arg_event_timeout_usec, USEC_PER_SEC,
                                          on_event_timeout, event);
}

static void manager_clear_for_worker(Manager *manager) {
        assert(manager);

        manager->inotify_event = sd_event_source_unref(manager->inotify_event);
        manager->kill_workers_event = sd_event_source_unref(manager->kill_workers_event);

        manager->event = sd_event_unref(manager->event);

        manager->workers = hashmap_free(manager->workers);
        event_queue_cleanup(manager, EVENT_UNDEF);

        manager->monitor = sd_device_monitor_unref(manager->monitor);
        manager->ctrl = udev_ctrl_unref(manager->ctrl);

        manager->worker_watch[READ_END] = safe_close(manager->worker_watch[READ_END]);
}

static Manager* manager_free(Manager *manager) {
        if (!manager)
                return NULL;

        udev_builtin_exit();

        if (manager->pid == getpid_cached())
                udev_ctrl_cleanup(manager->ctrl);

        manager_clear_for_worker(manager);

        sd_netlink_unref(manager->rtnl);

        hashmap_free_free_free(manager->properties);
        udev_rules_free(manager->rules);

        safe_close(manager->inotify_fd);
        safe_close_pair(manager->worker_watch);

        return mfree(manager);
}

DEFINE_TRIVIAL_CLEANUP_FUNC(Manager*, manager_free);

static int worker_send_message(int fd) {
        struct worker_message message = {};

        return loop_write(fd, &message, sizeof(message), false);
}

static int worker_lock_block_device(sd_device *dev, int *ret_fd) {
        _cleanup_close_ int fd = -1;
        const char *val;
        int r;

        assert(dev);
        assert(ret_fd);

        /* Take a shared lock on the device node; this establishes a concept of device "ownership" to
         * serialize device access. External processes holding an exclusive lock will cause udev to skip the
         * event handling; in the case udev acquired the lock, the external process can block until udev has
         * finished its event handling. */

        if (device_for_action(dev, SD_DEVICE_REMOVE))
                return 0;

        r = sd_device_get_subsystem(dev, &val);
        if (r < 0)
                return log_device_debug_errno(dev, r, "Failed to get subsystem: %m");

        if (!streq(val, "block"))
                return 0;

        r = sd_device_get_sysname(dev, &val);
        if (r < 0)
                return log_device_debug_errno(dev, r, "Failed to get sysname: %m");

        if (STARTSWITH_SET(val, "dm-", "md", "drbd"))
                return 0;

        r = sd_device_get_devtype(dev, &val);
        if (r < 0 && r != -ENOENT)
                return log_device_debug_errno(dev, r, "Failed to get devtype: %m");
        if (r >= 0 && streq(val, "partition")) {
                r = sd_device_get_parent(dev, &dev);
                if (r < 0)
                        return log_device_debug_errno(dev, r, "Failed to get parent device: %m");
        }

        r = sd_device_get_devname(dev, &val);
        if (r == -ENOENT)
                return 0;
        if (r < 0)
                return log_device_debug_errno(dev, r, "Failed to get devname: %m");

        fd = open(val, O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
        if (fd < 0) {
                log_device_debug_errno(dev, errno, "Failed to open '%s', ignoring: %m", val);
                return 0;
        }

        if (flock(fd, LOCK_SH|LOCK_NB) < 0)
                return log_device_debug_errno(dev, errno, "Failed to flock(%s): %m", val);

        *ret_fd = TAKE_FD(fd);
        return 1;
}

static int worker_mark_block_device_read_only(sd_device *dev) {
        _cleanup_close_ int fd = -1;
        const char *val;
        int state = 1, r;

        assert(dev);

        if (!arg_blockdev_read_only)
                return 0;

        /* Do this only once, when the block device is new. If the device is later retriggered let's not
         * toggle the bit again, so that people can boot up with full read-only mode and then unset the bit
         * for specific devices only. */
        if (!device_for_action(dev, SD_DEVICE_ADD))
                return 0;

        r = sd_device_get_subsystem(dev, &val);
        if (r < 0)
                return log_device_debug_errno(dev, r, "Failed to get subsystem: %m");

        if (!streq(val, "block"))
                return 0;

        r = sd_device_get_sysname(dev, &val);
        if (r < 0)
                return log_device_debug_errno(dev, r, "Failed to get sysname: %m");

        /* Exclude synthetic devices for now, this is supposed to be a safety feature to avoid modification
         * of physical devices, and what sits on top of those doesn't really matter if we don't allow the
         * underlying block devices to receive changes. */
        if (STARTSWITH_SET(val, "dm-", "md", "drbd", "loop", "nbd", "zram"))
                return 0;

        r = sd_device_get_devname(dev, &val);
        if (r == -ENOENT)
                return 0;
        if (r < 0)
                return log_device_debug_errno(dev, r, "Failed to get devname: %m");

        fd = open(val, O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
        if (fd < 0)
                return log_device_debug_errno(dev, errno, "Failed to open '%s', ignoring: %m", val);

        if (ioctl(fd, BLKROSET, &state) < 0)
                return log_device_warning_errno(dev, errno, "Failed to mark block device '%s' read-only: %m", val);

        log_device_info(dev, "Successfully marked block device '%s' read-only.", val);
        return 0;
}

static int worker_process_device(Manager *manager, sd_device *dev) {
        _cleanup_(udev_event_freep) UdevEvent *udev_event = NULL;
        _cleanup_close_ int fd_lock = -1;
        int r;

        assert(manager);
        assert(dev);

        log_device_uevent(dev, "Processing device");

        udev_event = udev_event_new(dev, arg_exec_delay_usec, manager->rtnl, manager->log_level);
        if (!udev_event)
                return -ENOMEM;

        r = worker_lock_block_device(dev, &fd_lock);
        if (r == -EAGAIN) {
                /* So this is a block device and the device is locked currently via the BSD advisory locks —
                 * someone else is exclusively using it. This means we don't run our udev rules now, to not
                 * interfere. However we want to know when the device is unlocked again, and retrigger the
                 * device again then, so that the rules are run eventually. For that we use IN_CLOSE_WRITE
                 * inotify watches (which isn't exactly the same as waiting for the BSD locks to release, but
                 * not totally off, as long as unlock+close() is done together, as it usually is).
                 *
                 * (The user-facing side of this: https://systemd.io/BLOCK_DEVICE_LOCKING)
                 *
                 * There's a bit of a chicken and egg problem here for this however: inotify watching is
                 * supposed to be enabled via an option set via udev rules (OPTIONS+="watch"). If we skip the
                 * udev rules here however (as we just said we do), we would thus never see that specific
                 * udev rule, and thus never turn on inotify watching. But in order to catch up eventually
                 * and run them we we need the inotify watching: hence a classic chicken and egg problem.
                 *
                 * Our way out here: if we see the block device locked, unconditionally watch the device via
                 * inotify, regardless of any explicit request via OPTIONS+="watch". Thus, a device that is
                 * currently locked via the BSD file locks will be treated as if we ran a single udev rule
                 * only for it: the one that turns on inotify watching for it. If we eventually see the
                 * inotify IN_CLOSE_WRITE event, and then run the rules after all and we then realize that
                 * this wasn't actually requested (i.e. no OPTIONS+="watch" set) we'll simply turn off the
                 * watching again (see below). Effectively this means: inotify watching is now enabled either
                 * a) when the udev rules say so, or b) while the device is locked.
                 *
                 * Worst case scenario hence: in the (unlikely) case someone locked the device and we clash
                 * with that we might do inotify watching for a brief moment for a device where we actually
                 * weren't supposed to. But that shouldn't be too bad, in particular as BSD locks being taken
                 * on a block device is kinda an indication that the inotify logic is desired too, to some
                 * degree — they go hand-in-hand after all. */

                log_device_debug(dev, "Block device is currently locked, installing watch to wait until the lock is released.");
                (void) udev_watch_begin(manager->inotify_fd, dev);

                /* Now the watch is installed, let's lock the device again, maybe in the meantime things changed */
                r = worker_lock_block_device(dev, &fd_lock);
        }
        if (r < 0)
                return r;

        (void) worker_mark_block_device_read_only(dev);

        /* apply rules, create node, symlinks */
        r = udev_event_execute_rules(
                          udev_event,
                          manager->inotify_fd,
                          arg_event_timeout_usec,
                          arg_timeout_signal,
                          manager->properties,
                          manager->rules);
        if (r < 0)
                return r;

        udev_event_execute_run(udev_event, arg_event_timeout_usec, arg_timeout_signal);

        if (!manager->rtnl)
                /* in case rtnl was initialized */
                manager->rtnl = sd_netlink_ref(udev_event->rtnl);

        r = udev_event_process_inotify_watch(udev_event, manager->inotify_fd);
        if (r < 0)
                return r;

        log_device_uevent(dev, "Device processed");
        return 0;
}

static int worker_device_monitor_handler(sd_device_monitor *monitor, sd_device *dev, void *userdata) {
        Manager *manager = userdata;
        int r;

        assert(dev);
        assert(manager);

        r = worker_process_device(manager, dev);
        if (r == -EAGAIN)
                /* if we couldn't acquire the flock(), then proceed quietly */
                log_device_debug_errno(dev, r, "Device currently locked, not processing.");
        else {
                if (r < 0)
                        log_device_warning_errno(dev, r, "Failed to process device, ignoring: %m");

                /* send processed event back to libudev listeners */
                r = device_monitor_send_device(monitor, NULL, dev);
                if (r < 0)
                        log_device_warning_errno(dev, r, "Failed to send device, ignoring: %m");
        }

        /* send udevd the result of the event execution */
        r = worker_send_message(manager->worker_watch[WRITE_END]);
        if (r < 0)
                log_device_warning_errno(dev, r, "Failed to send signal to main daemon, ignoring: %m");

        /* Reset the log level, as it might be changed by "OPTIONS=log_level=". */
        log_set_max_level(manager->log_level);

        return 1;
}

static int worker_main(Manager *_manager, sd_device_monitor *monitor, sd_device *first_device) {
        _cleanup_(sd_device_unrefp) sd_device *dev = first_device;
        _cleanup_(manager_freep) Manager *manager = _manager;
        int r;

        assert(manager);
        assert(monitor);
        assert(dev);

        assert_se(unsetenv("NOTIFY_SOCKET") == 0);

        assert_se(sigprocmask_many(SIG_BLOCK, NULL, SIGTERM, -1) >= 0);

        /* Reset OOM score, we only protect the main daemon. */
        r = set_oom_score_adjust(0);
        if (r < 0)
                log_debug_errno(r, "Failed to reset OOM score, ignoring: %m");

        /* Clear unnecessary data in Manager object. */
        manager_clear_for_worker(manager);

        r = sd_event_new(&manager->event);
        if (r < 0)
                return log_error_errno(r, "Failed to allocate event loop: %m");

        r = sd_event_add_signal(manager->event, NULL, SIGTERM, NULL, NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to set SIGTERM event: %m");

        r = sd_device_monitor_attach_event(monitor, manager->event);
        if (r < 0)
                return log_error_errno(r, "Failed to attach event loop to device monitor: %m");

        r = sd_device_monitor_start(monitor, worker_device_monitor_handler, manager);
        if (r < 0)
                return log_error_errno(r, "Failed to start device monitor: %m");

        (void) sd_event_source_set_description(sd_device_monitor_get_event_source(monitor), "worker-device-monitor");

        /* Process first device */
        (void) worker_device_monitor_handler(monitor, dev, manager);

        r = sd_event_loop(manager->event);
        if (r < 0)
                return log_error_errno(r, "Event loop failed: %m");

        return 0;
}

static int worker_spawn(Manager *manager, struct event *event) {
        _cleanup_(sd_device_monitor_unrefp) sd_device_monitor *worker_monitor = NULL;
        struct worker *worker;
        pid_t pid;
        int r;

        /* listen for new events */
        r = device_monitor_new_full(&worker_monitor, MONITOR_GROUP_NONE, -1);
        if (r < 0)
                return r;

        /* allow the main daemon netlink address to send devices to the worker */
        r = device_monitor_allow_unicast_sender(worker_monitor, manager->monitor);
        if (r < 0)
                return log_error_errno(r, "Worker: Failed to set unicast sender: %m");

        r = device_monitor_enable_receiving(worker_monitor);
        if (r < 0)
                return log_error_errno(r, "Worker: Failed to enable receiving of device: %m");

        r = safe_fork("(udev-worker)", FORK_DEATHSIG, &pid);
        if (r < 0) {
                event->state = EVENT_QUEUED;
                return log_error_errno(r, "Failed to fork() worker: %m");
        }
        if (r == 0) {
                DEVICE_TRACE_POINT(worker_spawned, event->dev, getpid());

                /* Worker process */
                r = worker_main(manager, worker_monitor, sd_device_ref(event->dev));
                log_close();
                _exit(r < 0 ? EXIT_FAILURE : EXIT_SUCCESS);
        }

        r = worker_new(&worker, manager, worker_monitor, pid);
        if (r < 0)
                return log_error_errno(r, "Failed to create worker object: %m");

        worker_attach_event(worker, event);

        log_device_debug(event->dev, "Worker ["PID_FMT"] is forked for processing SEQNUM=%"PRIu64".", pid, event->seqnum);
        return 0;
}

static void event_run(Manager *manager, struct event *event) {
        static bool log_children_max_reached = true;
        struct worker *worker;
        int r;

        assert(manager);
        assert(event);

        log_device_uevent(event->dev, "Device ready for processing");

        HASHMAP_FOREACH(worker, manager->workers) {
                if (worker->state != WORKER_IDLE)
                        continue;

                r = device_monitor_send_device(manager->monitor, worker->monitor, event->dev);
                if (r < 0) {
                        log_device_error_errno(event->dev, r, "Worker ["PID_FMT"] did not accept message, killing the worker: %m",
                                               worker->pid);
                        (void) kill(worker->pid, SIGKILL);
                        worker->state = WORKER_KILLED;
                        continue;
                }
                worker_attach_event(worker, event);
                return;
        }

        if (hashmap_size(manager->workers) >= arg_children_max) {

                /* Avoid spamming the debug logs if the limit is already reached and
                 * many events still need to be processed */
                if (log_children_max_reached && arg_children_max > 1) {
                        log_debug("Maximum number (%u) of children reached.", hashmap_size(manager->workers));
                        log_children_max_reached = false;
                }
                return;
        }

        /* Re-enable the debug message for the next batch of events */
        log_children_max_reached = true;

        /* fork with up-to-date SELinux label database, so the child inherits the up-to-date db
           and, until the next SELinux policy changes, we safe further reloads in future children */
        mac_selinux_maybe_reload();

        /* start new worker and pass initial device */
        worker_spawn(manager, event);
}

static int event_queue_insert(Manager *manager, sd_device *dev) {
        _cleanup_(sd_device_unrefp) sd_device *clone = NULL;
        struct event *event;
        uint64_t seqnum;
        int r;

        assert(manager);
        assert(dev);

        /* only one process can add events to the queue */
        assert(manager->pid == getpid_cached());

        /* We only accepts devices received by device monitor. */
        r = sd_device_get_seqnum(dev, &seqnum);
        if (r < 0)
                return r;

        /* Save original device to restore the state on failures. */
        r = device_shallow_clone(dev, &clone);
        if (r < 0)
                return r;

        r = device_copy_properties(clone, dev);
        if (r < 0)
                return r;

        event = new(struct event, 1);
        if (!event)
                return -ENOMEM;

        *event = (struct event) {
                .manager = manager,
                .dev = sd_device_ref(dev),
                .dev_kernel = TAKE_PTR(clone),
                .seqnum = seqnum,
                .state = EVENT_QUEUED,
        };

        if (LIST_IS_EMPTY(manager->events)) {
                r = touch("/run/udev/queue");
                if (r < 0)
                        log_warning_errno(r, "Failed to touch /run/udev/queue: %m");
        }

        LIST_APPEND(event, manager->events, event);

        log_device_uevent(dev, "Device is queued");

        return 0;
}

static void manager_kill_workers(Manager *manager, bool force) {
        struct worker *worker;

        assert(manager);

        HASHMAP_FOREACH(worker, manager->workers) {
                if (worker->state == WORKER_KILLED)
                        continue;

                if (worker->state == WORKER_RUNNING && !force) {
                        worker->state = WORKER_KILLING;
                        continue;
                }

                worker->state = WORKER_KILLED;
                (void) kill(worker->pid, SIGTERM);
        }
}

/* lookup event for identical, parent, child device */
static int is_device_busy(Manager *manager, struct event *event) {
        const char *subsystem, *devpath, *devpath_old = NULL;
        dev_t devnum = makedev(0, 0);
        struct event *loop_event;
        size_t devpath_len;
        int r, ifindex = 0;
        bool is_block;

        r = sd_device_get_subsystem(event->dev, &subsystem);
        if (r < 0)
                return r;

        is_block = streq(subsystem, "block");

        r = sd_device_get_devpath(event->dev, &devpath);
        if (r < 0)
                return r;

        devpath_len = strlen(devpath);

        r = sd_device_get_property_value(event->dev, "DEVPATH_OLD", &devpath_old);
        if (r < 0 && r != -ENOENT)
                return r;

        r = sd_device_get_devnum(event->dev, &devnum);
        if (r < 0 && r != -ENOENT)
                return r;

        r = sd_device_get_ifindex(event->dev, &ifindex);
        if (r < 0 && r != -ENOENT)
                return r;

        /* check if queue contains events we depend on */
        LIST_FOREACH(event, loop_event, manager->events) {
                size_t loop_devpath_len, common;
                const char *loop_devpath;

                /* we already found a later event, earlier cannot block us, no need to check again */
                if (loop_event->seqnum < event->delaying_seqnum)
                        continue;

                /* event we checked earlier still exists, no need to check again */
                if (loop_event->seqnum == event->delaying_seqnum)
                        return true;

                /* found ourself, no later event can block us */
                if (loop_event->seqnum >= event->seqnum)
                        break;

                /* check major/minor */
                if (major(devnum) != 0) {
                        const char *s;
                        dev_t d;

                        if (sd_device_get_subsystem(loop_event->dev, &s) < 0)
                                continue;

                        if (sd_device_get_devnum(loop_event->dev, &d) >= 0 &&
                            devnum == d && is_block == streq(s, "block"))
                                goto set_delaying_seqnum;
                }

                /* check network device ifindex */
                if (ifindex > 0) {
                        int i;

                        if (sd_device_get_ifindex(loop_event->dev, &i) >= 0 &&
                            ifindex == i)
                                goto set_delaying_seqnum;
                }

                if (sd_device_get_devpath(loop_event->dev, &loop_devpath) < 0)
                        continue;

                /* check our old name */
                if (devpath_old && streq(devpath_old, loop_devpath))
                        goto set_delaying_seqnum;

                loop_devpath_len = strlen(loop_devpath);

                /* compare devpath */
                common = MIN(devpath_len, loop_devpath_len);

                /* one devpath is contained in the other? */
                if (!strneq(devpath, loop_devpath, common))
                        continue;

                /* identical device event found */
                if (devpath_len == loop_devpath_len)
                        goto set_delaying_seqnum;

                /* parent device event found */
                if (devpath[common] == '/')
                        goto set_delaying_seqnum;

                /* child device event found */
                if (loop_devpath[common] == '/')
                        goto set_delaying_seqnum;
        }

        return false;

set_delaying_seqnum:
        log_device_debug(event->dev, "SEQNUM=%" PRIu64 " blocked by SEQNUM=%" PRIu64,
                         event->seqnum, loop_event->seqnum);

        event->delaying_seqnum = loop_event->seqnum;
        return true;
}

static void manager_exit(Manager *manager) {
        assert(manager);

        manager->exit = true;

        sd_notify(false,
                  "STOPPING=1\n"
                  "STATUS=Starting shutdown...");

        /* close sources of new events and discard buffered events */
        manager->ctrl = udev_ctrl_unref(manager->ctrl);

        manager->inotify_event = sd_event_source_unref(manager->inotify_event);
        manager->inotify_fd = safe_close(manager->inotify_fd);

        manager->monitor = sd_device_monitor_unref(manager->monitor);

        /* discard queued events and kill workers */
        event_queue_cleanup(manager, EVENT_QUEUED);
        manager_kill_workers(manager, true);
}

/* reload requested, HUP signal received, rules changed, builtin changed */
static void manager_reload(Manager *manager) {

        assert(manager);

        sd_notify(false,
                  "RELOADING=1\n"
                  "STATUS=Flushing configuration...");

        manager_kill_workers(manager, false);
        manager->rules = udev_rules_free(manager->rules);
        udev_builtin_exit();

        sd_notifyf(false,
                   "READY=1\n"
                   "STATUS=Processing with %u children at max", arg_children_max);
}

static int on_kill_workers_event(sd_event_source *s, uint64_t usec, void *userdata) {
        Manager *manager = userdata;

        assert(manager);

        log_debug("Cleanup idle workers");
        manager_kill_workers(manager, false);

        return 1;
}

static void event_queue_start(Manager *manager) {
        struct event *event;
        usec_t usec;
        int r;

        assert(manager);

        if (LIST_IS_EMPTY(manager->events) ||
            manager->exit || manager->stop_exec_queue)
                return;

        assert_se(sd_event_now(manager->event, CLOCK_MONOTONIC, &usec) >= 0);
        /* check for changed config, every 3 seconds at most */
        if (manager->last_usec == 0 ||
            usec - manager->last_usec > 3 * USEC_PER_SEC) {
                if (udev_rules_check_timestamp(manager->rules) ||
                    udev_builtin_validate())
                        manager_reload(manager);

                manager->last_usec = usec;
        }

        r = event_source_disable(manager->kill_workers_event);
        if (r < 0)
                log_warning_errno(r, "Failed to disable event source for cleaning up idle workers, ignoring: %m");

        udev_builtin_init();

        if (!manager->rules) {
                r = udev_rules_load(&manager->rules, arg_resolve_name_timing);
                if (r < 0) {
                        log_warning_errno(r, "Failed to read udev rules: %m");
                        return;
                }
        }

        LIST_FOREACH(event, event, manager->events) {
                if (event->state != EVENT_QUEUED)
                        continue;

                /* do not start event if parent or child event is still running */
                if (is_device_busy(manager, event) != 0)
                        continue;

                event_run(manager, event);
        }
}

static void event_queue_cleanup(Manager *manager, enum event_state match_type) {
        struct event *event, *tmp;

        LIST_FOREACH_SAFE(event, event, tmp, manager->events) {
                if (match_type != EVENT_UNDEF && match_type != event->state)
                        continue;

                event_free(event);
        }
}

static int on_worker(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
        Manager *manager = userdata;

        assert(manager);

        for (;;) {
                struct worker_message msg;
                struct iovec iovec = {
                        .iov_base = &msg,
                        .iov_len = sizeof(msg),
                };
                CMSG_BUFFER_TYPE(CMSG_SPACE(sizeof(struct ucred))) control;
                struct msghdr msghdr = {
                        .msg_iov = &iovec,
                        .msg_iovlen = 1,
                        .msg_control = &control,
                        .msg_controllen = sizeof(control),
                };
                ssize_t size;
                struct ucred *ucred;
                struct worker *worker;

                size = recvmsg_safe(fd, &msghdr, MSG_DONTWAIT);
                if (size == -EINTR)
                        continue;
                if (size == -EAGAIN)
                        /* nothing more to read */
                        break;
                if (size < 0)
                        return log_error_errno(size, "Failed to receive message: %m");

                cmsg_close_all(&msghdr);

                if (size != sizeof(struct worker_message)) {
                        log_warning("Ignoring worker message with invalid size %zi bytes", size);
                        continue;
                }

                ucred = CMSG_FIND_DATA(&msghdr, SOL_SOCKET, SCM_CREDENTIALS, struct ucred);
                if (!ucred || ucred->pid <= 0) {
                        log_warning("Ignoring worker message without valid PID");
                        continue;
                }

                /* lookup worker who sent the signal */
                worker = hashmap_get(manager->workers, PID_TO_PTR(ucred->pid));
                if (!worker) {
                        log_debug("Worker ["PID_FMT"] returned, but is no longer tracked", ucred->pid);
                        continue;
                }

                if (worker->state == WORKER_KILLING) {
                        worker->state = WORKER_KILLED;
                        (void) kill(worker->pid, SIGTERM);
                } else if (worker->state != WORKER_KILLED)
                        worker->state = WORKER_IDLE;

                /* worker returned */
                event_free(worker->event);
        }

        /* we have free workers, try to schedule events */
        event_queue_start(manager);

        return 1;
}

static int on_uevent(sd_device_monitor *monitor, sd_device *dev, void *userdata) {
        Manager *manager = userdata;
        int r;

        assert(manager);

        DEVICE_TRACE_POINT(kernel_uevent_received, dev);

        device_ensure_usec_initialized(dev, NULL);

        r = event_queue_insert(manager, dev);
        if (r < 0) {
                log_device_error_errno(dev, r, "Failed to insert device into event queue: %m");
                return 1;
        }

        /* we have fresh events, try to schedule them */
        event_queue_start(manager);

        return 1;
}

/* receive the udevd message from userspace */
static int on_ctrl_msg(struct udev_ctrl *uctrl, enum udev_ctrl_msg_type type, const union udev_ctrl_msg_value *value, void *userdata) {
        Manager *manager = userdata;
        int r;

        assert(value);
        assert(manager);

        switch (type) {
        case UDEV_CTRL_SET_LOG_LEVEL:
                log_debug("Received udev control message (SET_LOG_LEVEL), setting log_level=%i", value->intval);
                log_set_max_level(value->intval);
                manager->log_level = value->intval;
                manager_kill_workers(manager, false);
                break;
        case UDEV_CTRL_STOP_EXEC_QUEUE:
                log_debug("Received udev control message (STOP_EXEC_QUEUE)");
                manager->stop_exec_queue = true;
                break;
        case UDEV_CTRL_START_EXEC_QUEUE:
                log_debug("Received udev control message (START_EXEC_QUEUE)");
                manager->stop_exec_queue = false;
                event_queue_start(manager);
                break;
        case UDEV_CTRL_RELOAD:
                log_debug("Received udev control message (RELOAD)");
                manager_reload(manager);
                break;
        case UDEV_CTRL_SET_ENV: {
                _unused_ _cleanup_free_ char *old_val = NULL;
                _cleanup_free_ char *key = NULL, *val = NULL, *old_key = NULL;
                const char *eq;

                eq = strchr(value->buf, '=');
                if (!eq) {
                        log_error("Invalid key format '%s'", value->buf);
                        return 1;
                }

                key = strndup(value->buf, eq - value->buf);
                if (!key) {
                        log_oom();
                        return 1;
                }

                old_val = hashmap_remove2(manager->properties, key, (void **) &old_key);

                r = hashmap_ensure_allocated(&manager->properties, &string_hash_ops);
                if (r < 0) {
                        log_oom();
                        return 1;
                }

                eq++;
                if (isempty(eq)) {
                        log_debug("Received udev control message (ENV), unsetting '%s'", key);

                        r = hashmap_put(manager->properties, key, NULL);
                        if (r < 0) {
                                log_oom();
                                return 1;
                        }
                } else {
                        val = strdup(eq);
                        if (!val) {
                                log_oom();
                                return 1;
                        }

                        log_debug("Received udev control message (ENV), setting '%s=%s'", key, val);

                        r = hashmap_put(manager->properties, key, val);
                        if (r < 0) {
                                log_oom();
                                return 1;
                        }
                }

                key = val = NULL;
                manager_kill_workers(manager, false);
                break;
        }
        case UDEV_CTRL_SET_CHILDREN_MAX:
                if (value->intval <= 0) {
                        log_debug("Received invalid udev control message (SET_MAX_CHILDREN, %i), ignoring.", value->intval);
                        return 0;
                }

                log_debug("Received udev control message (SET_MAX_CHILDREN), setting children_max=%i", value->intval);
                arg_children_max = value->intval;

                (void) sd_notifyf(false,
                                  "READY=1\n"
                                  "STATUS=Processing with %u children at max", arg_children_max);
                break;
        case UDEV_CTRL_PING:
                log_debug("Received udev control message (PING)");
                break;
        case UDEV_CTRL_EXIT:
                log_debug("Received udev control message (EXIT)");
                manager_exit(manager);
                break;
        default:
                log_debug("Received unknown udev control message, ignoring");
        }

        return 1;
}

static int synthesize_change_one(sd_device *dev, sd_device *target) {
        int r;

        if (DEBUG_LOGGING) {
                const char *syspath = NULL;
                (void) sd_device_get_syspath(target, &syspath);
                log_device_debug(dev, "device is closed, synthesising 'change' on %s", strna(syspath));
        }

        r = sd_device_trigger(target, SD_DEVICE_CHANGE);
        if (r < 0)
                return log_device_debug_errno(target, r, "Failed to trigger 'change' uevent: %m");

        DEVICE_TRACE_POINT(synthetic_change_event, dev);

        return 0;
}

static int synthesize_change(sd_device *dev) {
        const char *subsystem, *sysname, *devtype;
        int r;

        r = sd_device_get_subsystem(dev, &subsystem);
        if (r < 0)
                return r;

        r = sd_device_get_devtype(dev, &devtype);
        if (r < 0)
                return r;

        r = sd_device_get_sysname(dev, &sysname);
        if (r < 0)
                return r;

        if (streq_ptr(subsystem, "block") &&
            streq_ptr(devtype, "disk") &&
            !startswith(sysname, "dm-")) {
                _cleanup_(sd_device_enumerator_unrefp) sd_device_enumerator *e = NULL;
                bool part_table_read = false, has_partitions = false;
                const char *devname;
                sd_device *d;
                int fd;

                r = sd_device_get_devname(dev, &devname);
                if (r < 0)
                        return r;

                /* Try to re-read the partition table. This only succeeds if none of the devices is
                 * busy. The kernel returns 0 if no partition table is found, and we will not get an
                 * event for the disk. */
                fd = open(devname, O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
                if (fd >= 0) {
                        r = flock(fd, LOCK_EX|LOCK_NB);
                        if (r >= 0)
                                r = ioctl(fd, BLKRRPART, 0);

                        close(fd);
                        if (r >= 0)
                                part_table_read = true;
                }

                /* search for partitions */
                r = sd_device_enumerator_new(&e);
                if (r < 0)
                        return r;

                r = sd_device_enumerator_allow_uninitialized(e);
                if (r < 0)
                        return r;

                r = sd_device_enumerator_add_match_parent(e, dev);
                if (r < 0)
                        return r;

                r = sd_device_enumerator_add_match_subsystem(e, "block", true);
                if (r < 0)
                        return r;

                FOREACH_DEVICE(e, d) {
                        const char *t;

                        if (sd_device_get_devtype(d, &t) < 0 || !streq(t, "partition"))
                                continue;

                        has_partitions = true;
                        break;
                }

                /* We have partitions and re-read the table, the kernel already sent out a "change"
                 * event for the disk, and "remove/add" for all partitions. */
                if (part_table_read && has_partitions)
                        return 0;

                /* We have partitions but re-reading the partition table did not work, synthesize
                 * "change" for the disk and all partitions. */
                (void) synthesize_change_one(dev, dev);

                FOREACH_DEVICE(e, d) {
                        const char *t;

                        if (sd_device_get_devtype(d, &t) < 0 || !streq(t, "partition"))
                                continue;

                        (void) synthesize_change_one(dev, d);
                }

        } else
                (void) synthesize_change_one(dev, dev);

        return 0;
}

static int on_inotify(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
        Manager *manager = userdata;
        union inotify_event_buffer buffer;
        struct inotify_event *e;
        ssize_t l;
        int r;

        assert(manager);

        r = event_source_disable(manager->kill_workers_event);
        if (r < 0)
                log_warning_errno(r, "Failed to disable event source for cleaning up idle workers, ignoring: %m");

        l = read(fd, &buffer, sizeof(buffer));
        if (l < 0) {
                if (IN_SET(errno, EAGAIN, EINTR))
                        return 1;

                return log_error_errno(errno, "Failed to read inotify fd: %m");
        }

        FOREACH_INOTIFY_EVENT(e, buffer, l) {
                _cleanup_(sd_device_unrefp) sd_device *dev = NULL;
                const char *devnode;

                r = device_new_from_watch_handle(&dev, e->wd);
                if (r < 0) {
                        log_debug_errno(r, "Failed to create sd_device object from watch handle, ignoring: %m");
                        continue;
                }

                if (sd_device_get_devname(dev, &devnode) < 0)
                        continue;

                log_device_debug(dev, "Inotify event: %x for %s", e->mask, devnode);
                if (e->mask & IN_CLOSE_WRITE)
                        (void) synthesize_change(dev);

                /* Do not handle IN_IGNORED here. It should be handled by worker in 'remove' uevent;
                 * udev_event_execute_rules() -> event_execute_rules_on_remove() -> udev_watch_end(). */
        }

        return 1;
}

static int on_sigterm(sd_event_source *s, const struct signalfd_siginfo *si, void *userdata) {
        Manager *manager = userdata;

        assert(manager);

        manager_exit(manager);

        return 1;
}

static int on_sighup(sd_event_source *s, const struct signalfd_siginfo *si, void *userdata) {
        Manager *manager = userdata;

        assert(manager);

        manager_reload(manager);

        return 1;
}

static int on_sigchld(sd_event_source *s, const struct signalfd_siginfo *si, void *userdata) {
        Manager *manager = userdata;
        int r;

        assert(manager);

        for (;;) {
                pid_t pid;
                int status;
                struct worker *worker;

                pid = waitpid(-1, &status, WNOHANG);
                if (pid <= 0)
                        break;

                worker = hashmap_get(manager->workers, PID_TO_PTR(pid));
                if (!worker) {
                        log_warning("Worker ["PID_FMT"] is unknown, ignoring", pid);
                        continue;
                }

                if (WIFEXITED(status)) {
                        if (WEXITSTATUS(status) == 0)
                                log_debug("Worker ["PID_FMT"] exited", pid);
                        else
                                log_warning("Worker ["PID_FMT"] exited with return code %i", pid, WEXITSTATUS(status));
                } else if (WIFSIGNALED(status))
                        log_warning("Worker ["PID_FMT"] terminated by signal %i (%s)", pid, WTERMSIG(status), signal_to_string(WTERMSIG(status)));
                else if (WIFSTOPPED(status)) {
                        log_info("Worker ["PID_FMT"] stopped", pid);
                        continue;
                } else if (WIFCONTINUED(status)) {
                        log_info("Worker ["PID_FMT"] continued", pid);
                        continue;
                } else
                        log_warning("Worker ["PID_FMT"] exit with status 0x%04x", pid, status);

                if ((!WIFEXITED(status) || WEXITSTATUS(status) != 0) && worker->event) {
                        log_device_error(worker->event->dev, "Worker ["PID_FMT"] failed", pid);

                        /* delete state from disk */
                        device_delete_db(worker->event->dev);
                        device_tag_index(worker->event->dev, NULL, false);

                        if (manager->monitor) {
                                /* forward kernel event without amending it */
                                r = device_monitor_send_device(manager->monitor, NULL, worker->event->dev_kernel);
                                if (r < 0)
                                        log_device_error_errno(worker->event->dev_kernel, r, "Failed to send back device to kernel: %m");
                        }
                }

                worker_free(worker);
        }

        /* we can start new workers, try to schedule events */
        event_queue_start(manager);

        /* Disable unnecessary cleanup event */
        if (hashmap_isempty(manager->workers)) {
                r = event_source_disable(manager->kill_workers_event);
                if (r < 0)
                        log_warning_errno(r, "Failed to disable event source for cleaning up idle workers, ignoring: %m");
        }

        return 1;
}

static int on_post(sd_event_source *s, void *userdata) {
        Manager *manager = userdata;

        assert(manager);

        if (!LIST_IS_EMPTY(manager->events))
                return 1;

        /* There are no pending events. Let's cleanup idle process. */

        if (!hashmap_isempty(manager->workers)) {
                /* There are idle workers */
                (void) event_reset_time(manager->event, &manager->kill_workers_event, CLOCK_MONOTONIC,
                                        now(CLOCK_MONOTONIC) + 3 * USEC_PER_SEC, USEC_PER_SEC,
                                        on_kill_workers_event, manager, 0, "kill-workers-event", false);
                return 1;
        }

        /* There are no idle workers. */

        if (manager->exit)
                return sd_event_exit(manager->event, 0);

        if (manager->cgroup)
                /* cleanup possible left-over processes in our cgroup */
                (void) cg_kill(SYSTEMD_CGROUP_CONTROLLER, manager->cgroup, SIGKILL, CGROUP_IGNORE_SELF, NULL, NULL, NULL);

        return 1;
}

static int listen_fds(int *ret_ctrl, int *ret_netlink) {
        int ctrl_fd = -1, netlink_fd = -1;
        int fd, n;

        assert(ret_ctrl);
        assert(ret_netlink);

        n = sd_listen_fds(true);
        if (n < 0)
                return n;

        for (fd = SD_LISTEN_FDS_START; fd < n + SD_LISTEN_FDS_START; fd++) {
                if (sd_is_socket(fd, AF_LOCAL, SOCK_SEQPACKET, -1) > 0) {
                        if (ctrl_fd >= 0)
                                return -EINVAL;
                        ctrl_fd = fd;
                        continue;
                }

                if (sd_is_socket(fd, AF_NETLINK, SOCK_RAW, -1) > 0) {
                        if (netlink_fd >= 0)
                                return -EINVAL;
                        netlink_fd = fd;
                        continue;
                }

                return -EINVAL;
        }

        *ret_ctrl = ctrl_fd;
        *ret_netlink = netlink_fd;

        return 0;
}

/*
 * read the kernel command line, in case we need to get into debug mode
 *   udev.log_level=<level>                    syslog priority
 *   udev.children_max=<number of workers>     events are fully serialized if set to 1
 *   udev.exec_delay=<number of seconds>       delay execution of every executed program
 *   udev.event_timeout=<number of seconds>    seconds to wait before terminating an event
 *   udev.blockdev_read_only<=bool>            mark all block devices read-only when they appear
 */
static int parse_proc_cmdline_item(const char *key, const char *value, void *data) {
        int r;

        assert(key);

        if (proc_cmdline_key_streq(key, "udev.log_level") ||
            proc_cmdline_key_streq(key, "udev.log_priority")) { /* kept for backward compatibility */

                if (proc_cmdline_value_missing(key, value))
                        return 0;

                r = log_level_from_string(value);
                if (r >= 0)
                        log_set_max_level(r);

        } else if (proc_cmdline_key_streq(key, "udev.event_timeout")) {

                if (proc_cmdline_value_missing(key, value))
                        return 0;

                r = parse_sec(value, &arg_event_timeout_usec);

        } else if (proc_cmdline_key_streq(key, "udev.children_max")) {

                if (proc_cmdline_value_missing(key, value))
                        return 0;

                r = safe_atou(value, &arg_children_max);

        } else if (proc_cmdline_key_streq(key, "udev.exec_delay")) {

                if (proc_cmdline_value_missing(key, value))
                        return 0;

                r = parse_sec(value, &arg_exec_delay_usec);

        } else if (proc_cmdline_key_streq(key, "udev.timeout_signal")) {

                if (proc_cmdline_value_missing(key, value))
                        return 0;

                r = signal_from_string(value);
                if (r > 0)
                        arg_timeout_signal = r;

        } else if (proc_cmdline_key_streq(key, "udev.blockdev_read_only")) {

                if (!value)
                        arg_blockdev_read_only = true;
                else {
                        r = parse_boolean(value);
                        if (r < 0)
                                log_warning_errno(r, "Failed to parse udev.blockdev-read-only argument, ignoring: %s", value);
                        else
                                arg_blockdev_read_only = r;
                }

                if (arg_blockdev_read_only)
                        log_notice("All physical block devices will be marked read-only.");

                return 0;

        } else {
                if (startswith(key, "udev."))
                        log_warning("Unknown udev kernel command line option \"%s\", ignoring.", key);

                return 0;
        }

        if (r < 0)
                log_warning_errno(r, "Failed to parse \"%s=%s\", ignoring: %m", key, value);

        return 0;
}

static int help(void) {
        _cleanup_free_ char *link = NULL;
        int r;

        r = terminal_urlify_man("systemd-udevd.service", "8", &link);
        if (r < 0)
                return log_oom();

        printf("%s [OPTIONS...]\n\n"
               "Rule-based manager for device events and files.\n\n"
               "  -h --help                   Print this message\n"
               "  -V --version                Print version of the program\n"
               "  -d --daemon                 Detach and run in the background\n"
               "  -D --debug                  Enable debug output\n"
               "  -c --children-max=INT       Set maximum number of workers\n"
               "  -e --exec-delay=SECONDS     Seconds to wait before executing RUN=\n"
               "  -t --event-timeout=SECONDS  Seconds to wait before terminating an event\n"
               "  -N --resolve-names=early|late|never\n"
               "                              When to resolve users and groups\n"
               "\nSee the %s for details.\n",
               program_invocation_short_name,
               link);

        return 0;
}

static int parse_argv(int argc, char *argv[]) {
        enum {
                ARG_TIMEOUT_SIGNAL,
        };

        static const struct option options[] = {
                { "daemon",             no_argument,            NULL, 'd'                 },
                { "debug",              no_argument,            NULL, 'D'                 },
                { "children-max",       required_argument,      NULL, 'c'                 },
                { "exec-delay",         required_argument,      NULL, 'e'                 },
                { "event-timeout",      required_argument,      NULL, 't'                 },
                { "resolve-names",      required_argument,      NULL, 'N'                 },
                { "help",               no_argument,            NULL, 'h'                 },
                { "version",            no_argument,            NULL, 'V'                 },
                { "timeout-signal",     required_argument,      NULL,  ARG_TIMEOUT_SIGNAL },
                {}
        };

        int c, r;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "c:de:Dt:N:hV", options, NULL)) >= 0) {
                switch (c) {

                case 'd':
                        arg_daemonize = true;
                        break;
                case 'c':
                        r = safe_atou(optarg, &arg_children_max);
                        if (r < 0)
                                log_warning_errno(r, "Failed to parse --children-max= value '%s', ignoring: %m", optarg);
                        break;
                case 'e':
                        r = parse_sec(optarg, &arg_exec_delay_usec);
                        if (r < 0)
                                log_warning_errno(r, "Failed to parse --exec-delay= value '%s', ignoring: %m", optarg);
                        break;
                case ARG_TIMEOUT_SIGNAL:
                        r = signal_from_string(optarg);
                        if (r <= 0)
                                log_warning_errno(r, "Failed to parse --timeout-signal= value '%s', ignoring: %m", optarg);
                        else
                                arg_timeout_signal = r;

                        break;
                case 't':
                        r = parse_sec(optarg, &arg_event_timeout_usec);
                        if (r < 0)
                                log_warning_errno(r, "Failed to parse --event-timeout= value '%s', ignoring: %m", optarg);
                        break;
                case 'D':
                        arg_debug = true;
                        break;
                case 'N': {
                        ResolveNameTiming t;

                        t = resolve_name_timing_from_string(optarg);
                        if (t < 0)
                                log_warning("Invalid --resolve-names= value '%s', ignoring.", optarg);
                        else
                                arg_resolve_name_timing = t;
                        break;
                }
                case 'h':
                        return help();
                case 'V':
                        printf("%s\n", GIT_VERSION);
                        return 0;
                case '?':
                        return -EINVAL;
                default:
                        assert_not_reached("Unhandled option");

                }
        }

        return 1;
}

static int manager_new(Manager **ret, int fd_ctrl, int fd_uevent, const char *cgroup) {
        _cleanup_(manager_freep) Manager *manager = NULL;
        int r;

        assert(ret);

        manager = new(Manager, 1);
        if (!manager)
                return log_oom();

        *manager = (Manager) {
                .inotify_fd = -1,
                .worker_watch = { -1, -1 },
                .cgroup = cgroup,
        };

        r = udev_ctrl_new_from_fd(&manager->ctrl, fd_ctrl);
        if (r < 0)
                return log_error_errno(r, "Failed to initialize udev control socket: %m");

        r = udev_ctrl_enable_receiving(manager->ctrl);
        if (r < 0)
                return log_error_errno(r, "Failed to bind udev control socket: %m");

        r = device_monitor_new_full(&manager->monitor, MONITOR_GROUP_KERNEL, fd_uevent);
        if (r < 0)
                return log_error_errno(r, "Failed to initialize device monitor: %m");

        /* Bump receiver buffer, but only if we are not called via socket activation, as in that
         * case systemd sets the receive buffer size for us, and the value in the .socket unit
         * should take full effect. */
        if (fd_uevent < 0) {
                r = sd_device_monitor_set_receive_buffer_size(manager->monitor, 128 * 1024 * 1024);
                if (r < 0)
                        log_warning_errno(r, "Failed to set receive buffer size for device monitor, ignoring: %m");
        }

        r = device_monitor_enable_receiving(manager->monitor);
        if (r < 0)
                return log_error_errno(r, "Failed to bind netlink socket: %m");

        manager->log_level = log_get_max_level();

        *ret = TAKE_PTR(manager);

        return 0;
}

static int main_loop(Manager *manager) {
        int fd_worker, r;

        manager->pid = getpid_cached();

        /* unnamed socket from workers to the main daemon */
        r = socketpair(AF_LOCAL, SOCK_DGRAM|SOCK_CLOEXEC, 0, manager->worker_watch);
        if (r < 0)
                return log_error_errno(errno, "Failed to create socketpair for communicating with workers: %m");

        fd_worker = manager->worker_watch[READ_END];

        r = setsockopt_int(fd_worker, SOL_SOCKET, SO_PASSCRED, true);
        if (r < 0)
                return log_error_errno(r, "Failed to enable SO_PASSCRED: %m");

        manager->inotify_fd = inotify_init1(IN_CLOEXEC);
        if (manager->inotify_fd < 0)
                return log_error_errno(errno, "Failed to create inotify descriptor: %m");

        udev_watch_restore(manager->inotify_fd);

        /* block and listen to all signals on signalfd */
        assert_se(sigprocmask_many(SIG_BLOCK, NULL, SIGTERM, SIGINT, SIGHUP, SIGCHLD, -1) >= 0);

        r = sd_event_default(&manager->event);
        if (r < 0)
                return log_error_errno(r, "Failed to allocate event loop: %m");

        r = sd_event_add_signal(manager->event, NULL, SIGINT, on_sigterm, manager);
        if (r < 0)
                return log_error_errno(r, "Failed to create SIGINT event source: %m");

        r = sd_event_add_signal(manager->event, NULL, SIGTERM, on_sigterm, manager);
        if (r < 0)
                return log_error_errno(r, "Failed to create SIGTERM event source: %m");

        r = sd_event_add_signal(manager->event, NULL, SIGHUP, on_sighup, manager);
        if (r < 0)
                return log_error_errno(r, "Failed to create SIGHUP event source: %m");

        r = sd_event_add_signal(manager->event, NULL, SIGCHLD, on_sigchld, manager);
        if (r < 0)
                return log_error_errno(r, "Failed to create SIGCHLD event source: %m");

        r = sd_event_set_watchdog(manager->event, true);
        if (r < 0)
                return log_error_errno(r, "Failed to create watchdog event source: %m");

        r = udev_ctrl_attach_event(manager->ctrl, manager->event);
        if (r < 0)
                return log_error_errno(r, "Failed to attach event to udev control: %m");

        r = udev_ctrl_start(manager->ctrl, on_ctrl_msg, manager);
        if (r < 0)
                return log_error_errno(r, "Failed to start device monitor: %m");

        /* This needs to be after the inotify and uevent handling, to make sure
         * that the ping is send back after fully processing the pending uevents
         * (including the synthetic ones we may create due to inotify events).
         */
        r = sd_event_source_set_priority(udev_ctrl_get_event_source(manager->ctrl), SD_EVENT_PRIORITY_IDLE);
        if (r < 0)
                return log_error_errno(r, "Failed to set IDLE event priority for udev control event source: %m");

        r = sd_event_add_io(manager->event, &manager->inotify_event, manager->inotify_fd, EPOLLIN, on_inotify, manager);
        if (r < 0)
                return log_error_errno(r, "Failed to create inotify event source: %m");

        r = sd_device_monitor_attach_event(manager->monitor, manager->event);
        if (r < 0)
                return log_error_errno(r, "Failed to attach event to device monitor: %m");

        r = sd_device_monitor_start(manager->monitor, on_uevent, manager);
        if (r < 0)
                return log_error_errno(r, "Failed to start device monitor: %m");

        (void) sd_event_source_set_description(sd_device_monitor_get_event_source(manager->monitor), "device-monitor");

        r = sd_event_add_io(manager->event, NULL, fd_worker, EPOLLIN, on_worker, manager);
        if (r < 0)
                return log_error_errno(r, "Failed to create worker event source: %m");

        r = sd_event_add_post(manager->event, NULL, on_post, manager);
        if (r < 0)
                return log_error_errno(r, "Failed to create post event source: %m");

        udev_builtin_init();

        r = udev_rules_load(&manager->rules, arg_resolve_name_timing);
        if (!manager->rules)
                return log_error_errno(r, "Failed to read udev rules: %m");

        r = udev_rules_apply_static_dev_perms(manager->rules);
        if (r < 0)
                log_warning_errno(r, "Failed to apply permissions on static device nodes, ignoring: %m");

        (void) sd_notifyf(false,
                          "READY=1\n"
                          "STATUS=Processing with %u children at max", arg_children_max);

        r = sd_event_loop(manager->event);
        if (r < 0)
                log_error_errno(r, "Event loop failed: %m");

        sd_notify(false,
                  "STOPPING=1\n"
                  "STATUS=Shutting down...");
        return r;
}

int run_udevd(int argc, char *argv[]) {
        _cleanup_free_ char *cgroup = NULL;
        _cleanup_(manager_freep) Manager *manager = NULL;
        int fd_ctrl = -1, fd_uevent = -1;
        int r;

        log_set_target(LOG_TARGET_AUTO);
        log_open();
        udev_parse_config_full(&arg_children_max, &arg_exec_delay_usec, &arg_event_timeout_usec, &arg_resolve_name_timing, &arg_timeout_signal);
        log_parse_environment();
        log_open(); /* Done again to update after reading configuration. */

        r = parse_argv(argc, argv);
        if (r <= 0)
                return r;

        r = proc_cmdline_parse(parse_proc_cmdline_item, NULL, PROC_CMDLINE_STRIP_RD_PREFIX);
        if (r < 0)
                log_warning_errno(r, "Failed to parse kernel command line, ignoring: %m");

        if (arg_debug) {
                log_set_target(LOG_TARGET_CONSOLE);
                log_set_max_level(LOG_DEBUG);
        }

        r = must_be_root();
        if (r < 0)
                return r;

        if (arg_children_max == 0) {
                unsigned long cpu_limit, mem_limit, cpu_count = 1;

                r = cpus_in_affinity_mask();
                if (r < 0)
                        log_warning_errno(r, "Failed to determine number of local CPUs, ignoring: %m");
                else
                        cpu_count = r;

                cpu_limit = cpu_count * 2 + 16;
                mem_limit = MAX(physical_memory() / (128UL*1024*1024), 10U);

                arg_children_max = MIN(cpu_limit, mem_limit);
                arg_children_max = MIN(WORKER_NUM_MAX, arg_children_max);

                log_debug("Set children_max to %u", arg_children_max);
        }

        /* set umask before creating any file/directory */
        umask(022);

        r = mac_selinux_init();
        if (r < 0)
                return r;

        r = mkdir_errno_wrapper("/run/udev", 0755);
        if (r < 0 && r != -EEXIST)
                return log_error_errno(r, "Failed to create /run/udev: %m");

        if (getppid() == 1 && sd_booted() > 0) {
                /* Get our own cgroup, we regularly kill everything udev has left behind.
                 * We only do this on systemd systems, and only if we are directly spawned
                 * by PID1. Otherwise we are not guaranteed to have a dedicated cgroup. */
                r = cg_pid_get_path(SYSTEMD_CGROUP_CONTROLLER, 0, &cgroup);
                if (r < 0) {
                        if (IN_SET(r, -ENOENT, -ENOMEDIUM))
                                log_debug_errno(r, "Dedicated cgroup not found: %m");
                        else
                                log_warning_errno(r, "Failed to get cgroup: %m");
                }
        }

        r = listen_fds(&fd_ctrl, &fd_uevent);
        if (r < 0)
                return log_error_errno(r, "Failed to listen on fds: %m");

        r = manager_new(&manager, fd_ctrl, fd_uevent, cgroup);
        if (r < 0)
                return log_error_errno(r, "Failed to create manager: %m");

        if (arg_daemonize) {
                pid_t pid;

                log_info("Starting version " GIT_VERSION);

                /* connect /dev/null to stdin, stdout, stderr */
                if (log_get_max_level() < LOG_DEBUG) {
                        r = make_null_stdio();
                        if (r < 0)
                                log_warning_errno(r, "Failed to redirect standard streams to /dev/null: %m");
                }

                pid = fork();
                if (pid < 0)
                        return log_error_errno(errno, "Failed to fork daemon: %m");
                if (pid > 0)
                        /* parent */
                        return 0;

                /* child */
                (void) setsid();
        }

        return main_loop(manager);
}
