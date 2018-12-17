/* SPDX-License-Identifier: LGPL-2.1+ */

#include <fcntl.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <linux/vt.h>
#if ENABLE_UTMP
#include <utmpx.h>
#endif

#include "alloc-util.h"
#include "bus-error.h"
#include "bus-util.h"
#include "cgroup-util.h"
#include "conf-parser.h"
#include "fd-util.h"
#include "logind.h"
#include "parse-util.h"
#include "path-util.h"
#include "process-util.h"
#include "strv.h"
#include "terminal-util.h"
#include "udev-util.h"
#include "user-util.h"

void manager_reset_config(Manager *m) {
        assert(m);

        m->n_autovts = 6;
        m->reserve_vt = 6;
        m->remove_ipc = true;
        m->inhibit_delay_max = 5 * USEC_PER_SEC;
        m->user_stop_delay = 10 * USEC_PER_SEC;

        m->handle_power_key = HANDLE_POWEROFF;
        m->handle_suspend_key = HANDLE_SUSPEND;
        m->handle_hibernate_key = HANDLE_HIBERNATE;
        m->handle_lid_switch = HANDLE_SUSPEND;
        m->handle_lid_switch_ep = _HANDLE_ACTION_INVALID;
        m->handle_lid_switch_docked = HANDLE_IGNORE;
        m->power_key_ignore_inhibited = false;
        m->suspend_key_ignore_inhibited = false;
        m->hibernate_key_ignore_inhibited = false;
        m->lid_switch_ignore_inhibited = true;

        m->holdoff_timeout_usec = 30 * USEC_PER_SEC;

        m->idle_action_usec = 30 * USEC_PER_MINUTE;
        m->idle_action = HANDLE_IGNORE;

        m->runtime_dir_size = physical_memory_scale(10U, 100U); /* 10% */
        m->user_tasks_max = system_tasks_max_scale(DEFAULT_USER_TASKS_MAX_PERCENTAGE, 100U); /* 33% */
        m->sessions_max = 8192;
        m->inhibitors_max = 8192;

        m->kill_user_processes = KILL_USER_PROCESSES;

        m->kill_only_users = strv_free(m->kill_only_users);
        m->kill_exclude_users = strv_free(m->kill_exclude_users);
}

int manager_parse_config_file(Manager *m) {
        assert(m);

        return config_parse_many_nulstr(PKGSYSCONFDIR "/logind.conf",
                                        CONF_PATHS_NULSTR("systemd/logind.conf.d"),
                                        "Login\0",
                                        config_item_perf_lookup, logind_gperf_lookup,
                                        CONFIG_PARSE_WARN, m);
}

int manager_add_device(Manager *m, const char *sysfs, bool master, Device **_device) {
        Device *d;

        assert(m);
        assert(sysfs);

        d = hashmap_get(m->devices, sysfs);
        if (d)
                /* we support adding master-flags, but not removing them */
                d->master = d->master || master;
        else {
                d = device_new(m, sysfs, master);
                if (!d)
                        return -ENOMEM;
        }

        if (_device)
                *_device = d;

        return 0;
}

int manager_add_seat(Manager *m, const char *id, Seat **_seat) {
        Seat *s;
        int r;

        assert(m);
        assert(id);

        s = hashmap_get(m->seats, id);
        if (!s) {
                r = seat_new(&s, m, id);
                if (r < 0)
                        return r;
        }

        if (_seat)
                *_seat = s;

        return 0;
}

int manager_add_session(Manager *m, const char *id, Session **_session) {
        Session *s;
        int r;

        assert(m);
        assert(id);

        s = hashmap_get(m->sessions, id);
        if (!s) {
                r = session_new(&s, m, id);
                if (r < 0)
                        return r;
        }

        if (_session)
                *_session = s;

        return 0;
}

int manager_add_user(
                Manager *m,
                uid_t uid,
                gid_t gid,
                const char *name,
                const char *home,
                User **_user) {

        User *u;
        int r;

        assert(m);
        assert(name);

        u = hashmap_get(m->users, UID_TO_PTR(uid));
        if (!u) {
                r = user_new(&u, m, uid, gid, name, home);
                if (r < 0)
                        return r;
        }

        if (_user)
                *_user = u;

        return 0;
}

int manager_add_user_by_name(
                Manager *m,
                const char *name,
                User **_user) {

        const char *home = NULL;
        uid_t uid;
        gid_t gid;
        int r;

        assert(m);
        assert(name);

        r = get_user_creds(&name, &uid, &gid, &home, NULL);
        if (r < 0)
                return r;

        return manager_add_user(m, uid, gid, name, home, _user);
}

int manager_add_user_by_uid(Manager *m, uid_t uid, User **_user) {
        struct passwd *p;

        assert(m);

        errno = 0;
        p = getpwuid(uid);
        if (!p)
                return errno > 0 ? -errno : -ENOENT;

        return manager_add_user(m, uid, p->pw_gid, p->pw_name, p->pw_dir, _user);
}

int manager_add_inhibitor(Manager *m, const char* id, Inhibitor **_inhibitor) {
        Inhibitor *i;

        assert(m);
        assert(id);

        i = hashmap_get(m->inhibitors, id);
        if (i) {
                if (_inhibitor)
                        *_inhibitor = i;

                return 0;
        }

        i = inhibitor_new(m, id);
        if (!i)
                return -ENOMEM;

        if (_inhibitor)
                *_inhibitor = i;

        return 0;
}

int manager_add_button(Manager *m, const char *name, Button **_button) {
        Button *b;

        assert(m);
        assert(name);

        b = hashmap_get(m->buttons, name);
        if (!b) {
                b = button_new(m, name);
                if (!b)
                        return -ENOMEM;
        }

        if (_button)
                *_button = b;

        return 0;
}

int manager_process_seat_device(Manager *m, struct udev_device *d) {
        Device *device;
        int r;

        assert(m);

        if (streq_ptr(udev_device_get_action(d), "remove")) {

                device = hashmap_get(m->devices, udev_device_get_syspath(d));
                if (!device)
                        return 0;

                seat_add_to_gc_queue(device->seat);
                device_free(device);

        } else {
                const char *sn;
                Seat *seat = NULL;
                bool master;

                sn = udev_device_get_property_value(d, "ID_SEAT");
                if (isempty(sn))
                        sn = "seat0";

                if (!seat_name_is_valid(sn)) {
                        log_warning("Device with invalid seat name %s found, ignoring.", sn);
                        return 0;
                }

                seat = hashmap_get(m->seats, sn);
                master = udev_device_has_tag(d, "master-of-seat");

                /* Ignore non-master devices for unknown seats */
                if (!master && !seat)
                        return 0;

                r = manager_add_device(m, udev_device_get_syspath(d), master, &device);
                if (r < 0)
                        return r;

                if (!seat) {
                        r = manager_add_seat(m, sn, &seat);
                        if (r < 0) {
                                if (!device->seat)
                                        device_free(device);

                                return r;
                        }
                }

                device_attach(device, seat);
                seat_start(seat);
        }

        return 0;
}

int manager_process_button_device(Manager *m, struct udev_device *d) {
        Button *b;

        int r;

        assert(m);

        if (streq_ptr(udev_device_get_action(d), "remove")) {

                b = hashmap_get(m->buttons, udev_device_get_sysname(d));
                if (!b)
                        return 0;

                button_free(b);

        } else {
                const char *sn;

                r = manager_add_button(m, udev_device_get_sysname(d), &b);
                if (r < 0)
                        return r;

                sn = udev_device_get_property_value(d, "ID_SEAT");
                if (isempty(sn))
                        sn = "seat0";

                button_set_seat(b, sn);

                r = button_open(b);
                if (r < 0) /* event device doesn't have any keys or switches relevant to us? (or any other error
                            * opening the device?) let's close the button again. */
                        button_free(b);
        }

        return 0;
}

int manager_get_session_by_pid(Manager *m, pid_t pid, Session **ret) {
        _cleanup_free_ char *unit = NULL;
        Session *s;
        int r;

        assert(m);

        if (!pid_is_valid(pid))
                return -EINVAL;

        s = hashmap_get(m->sessions_by_leader, PID_TO_PTR(pid));
        if (!s) {
                r = cg_pid_get_unit(pid, &unit);
                if (r < 0)
                        goto not_found;

                s = hashmap_get(m->session_units, unit);
                if (!s)
                        goto not_found;
        }

        if (ret)
                *ret = s;

        return 1;

not_found:
        if (ret)
                *ret = NULL;
        return 0;
}

int manager_get_user_by_pid(Manager *m, pid_t pid, User **ret) {
        _cleanup_free_ char *unit = NULL;
        User *u;
        int r;

        assert(m);

        if (!pid_is_valid(pid))
                return -EINVAL;

        r = cg_pid_get_slice(pid, &unit);
        if (r < 0)
                goto not_found;

        u = hashmap_get(m->user_units, unit);
        if (!u)
                goto not_found;

        if (ret)
                *ret = u;

        return 1;

not_found:
        if (ret)
                *ret = NULL;

        return 0;
}

int manager_get_idle_hint(Manager *m, dual_timestamp *t) {
        Session *s;
        bool idle_hint;
        dual_timestamp ts = DUAL_TIMESTAMP_NULL;
        Iterator i;

        assert(m);

        idle_hint = !manager_is_inhibited(m, INHIBIT_IDLE, INHIBIT_BLOCK, t, false, false, 0, NULL);

        HASHMAP_FOREACH(s, m->sessions, i) {
                dual_timestamp k;
                int ih;

                ih = session_get_idle_hint(s, &k);
                if (ih < 0)
                        return ih;

                if (!ih) {
                        if (!idle_hint) {
                                if (k.monotonic < ts.monotonic)
                                        ts = k;
                        } else {
                                idle_hint = false;
                                ts = k;
                        }
                } else if (idle_hint) {

                        if (k.monotonic > ts.monotonic)
                                ts = k;
                }
        }

        if (t)
                *t = ts;

        return idle_hint;
}

bool manager_shall_kill(Manager *m, const char *user) {
        assert(m);
        assert(user);

        if (!m->kill_exclude_users && streq(user, "root"))
                return false;

        if (strv_contains(m->kill_exclude_users, user))
                return false;

        if (!strv_isempty(m->kill_only_users))
                return strv_contains(m->kill_only_users, user);

        return m->kill_user_processes;
}

int config_parse_n_autovts(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        unsigned *n = data;
        unsigned o;
        int r;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = safe_atou(rvalue, &o);
        if (r < 0) {
                log_syntax(unit, LOG_ERR, filename, line, r, "Failed to parse number of autovts, ignoring: %s", rvalue);
                return 0;
        }

        if (o > 15) {
                log_syntax(unit, LOG_ERR, filename, line, r, "A maximum of 15 autovts are supported, ignoring: %s", rvalue);
                return 0;
        }

        *n = o;
        return 0;
}

static int vt_is_busy(unsigned int vtnr) {
        struct vt_stat vt_stat;
        int r = 0;
        _cleanup_close_ int fd;

        assert(vtnr >= 1);

        /* VT_GETSTATE "cannot return state for more than 16 VTs, since v_state is short" */
        assert(vtnr <= 15);

        /* We explicitly open /dev/tty1 here instead of /dev/tty0. If
         * we'd open the latter we'd open the foreground tty which
         * hence would be unconditionally busy. By opening /dev/tty1
         * we avoid this. Since tty1 is special and needs to be an
         * explicitly loaded getty or DM this is safe. */

        fd = open_terminal("/dev/tty1", O_RDWR|O_NOCTTY|O_CLOEXEC);
        if (fd < 0)
                return -errno;

        if (ioctl(fd, VT_GETSTATE, &vt_stat) < 0)
                r = -errno;
        else
                r = !!(vt_stat.v_state & (1 << vtnr));

        return r;
}

int manager_spawn_autovt(Manager *m, unsigned int vtnr) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        char name[sizeof("autovt@tty.service") + DECIMAL_STR_MAX(unsigned int)];
        int r;

        assert(m);
        assert(vtnr >= 1);

        if (vtnr > m->n_autovts &&
            vtnr != m->reserve_vt)
                return 0;

        if (vtnr != m->reserve_vt) {
                /* If this is the reserved TTY, we'll start the getty
                 * on it in any case, but otherwise only if it is not
                 * busy. */

                r = vt_is_busy(vtnr);
                if (r < 0)
                        return r;
                else if (r > 0)
                        return -EBUSY;
        }

        snprintf(name, sizeof(name), "autovt@tty%u.service", vtnr);
        r = sd_bus_call_method(
                        m->bus,
                        "org.freedesktop.systemd1",
                        "/org/freedesktop/systemd1",
                        "org.freedesktop.systemd1.Manager",
                        "StartUnit",
                        &error,
                        NULL,
                        "ss", name, "fail");
        if (r < 0)
                log_error("Failed to start %s: %s", name, bus_error_message(&error, r));

        return r;
}

static bool manager_is_docked(Manager *m) {
        Iterator i;
        Button *b;

        HASHMAP_FOREACH(b, m->buttons, i)
                if (b->docked)
                        return true;

        return false;
}

static int manager_count_external_displays(Manager *m) {
        _cleanup_(udev_enumerate_unrefp) struct udev_enumerate *e = NULL;
        struct udev_list_entry *item = NULL, *first = NULL;
        int r;
        int n = 0;

        e = udev_enumerate_new(m->udev);
        if (!e)
                return -ENOMEM;

        r = udev_enumerate_add_match_subsystem(e, "drm");
        if (r < 0)
                return r;

        r = udev_enumerate_scan_devices(e);
        if (r < 0)
                return r;

        first = udev_enumerate_get_list_entry(e);
        udev_list_entry_foreach(item, first) {
                _cleanup_(udev_device_unrefp) struct udev_device *d = NULL;
                struct udev_device *p;
                const char *status, *enabled, *dash, *nn, *i;
                bool external = false;

                d = udev_device_new_from_syspath(m->udev, udev_list_entry_get_name(item));
                if (!d)
                        return -ENOMEM;

                p = udev_device_get_parent(d);
                if (!p)
                        continue;

                /* If the parent shares the same subsystem as the
                 * device we are looking at then it is a connector,
                 * which is what we are interested in. */
                if (!streq_ptr(udev_device_get_subsystem(p), "drm"))
                        continue;

                nn = udev_device_get_sysname(d);
                if (!nn)
                        continue;

                /* Ignore internal displays: the type is encoded in
                 * the sysfs name, as the second dash separated item
                 * (the first is the card name, the last the connector
                 * number). We implement a whitelist of external
                 * displays here, rather than a whitelist, to ensure
                 * we don't block suspends too eagerly. */
                dash = strchr(nn, '-');
                if (!dash)
                        continue;

                dash++;
                FOREACH_STRING(i, "VGA-", "DVI-I-", "DVI-D-", "DVI-A-"
                               "Composite-", "SVIDEO-", "Component-",
                               "DIN-", "DP-", "HDMI-A-", "HDMI-B-", "TV-") {

                        if (startswith(dash, i)) {
                                external = true;
                                break;
                        }
                }
                if (!external)
                        continue;

                /* Ignore ports that are not enabled */
                enabled = udev_device_get_sysattr_value(d, "enabled");
                if (!enabled)
                        continue;
                if (!streq_ptr(enabled, "enabled"))
                        continue;

                /* We count any connector which is not explicitly
                 * "disconnected" as connected. */
                status = udev_device_get_sysattr_value(d, "status");
                if (!streq_ptr(status, "disconnected"))
                        n++;
        }

        return n;
}

bool manager_is_docked_or_external_displays(Manager *m) {
        int n;

        /* If we are docked don't react to lid closing */
        if (manager_is_docked(m)) {
                log_debug("System is docked.");
                return true;
        }

        /* If we have more than one display connected,
         * assume that we are docked. */
        n = manager_count_external_displays(m);
        if (n < 0)
                log_warning_errno(n, "Display counting failed: %m");
        else if (n >= 1) {
                log_debug("External (%i) displays connected.", n);
                return true;
        }

        return false;
}

bool manager_is_on_external_power(void) {
        int r;

        /* For now we only check for AC power, but 'external power' can apply
         * to anything that isn't an internal battery */
        r = on_ac_power();
        if (r < 0)
                log_warning_errno(r, "Failed to read AC power status: %m");
        else if (r > 0)
                return true;

        return false;
}

bool manager_all_buttons_ignored(Manager *m) {
        assert(m);

        if (m->handle_power_key != HANDLE_IGNORE)
                return false;
        if (m->handle_suspend_key != HANDLE_IGNORE)
                return false;
        if (m->handle_hibernate_key != HANDLE_IGNORE)
                return false;
        if (m->handle_lid_switch != HANDLE_IGNORE)
                return false;
        if (m->handle_lid_switch_ep != _HANDLE_ACTION_INVALID &&
            m->handle_lid_switch_ep != HANDLE_IGNORE)
                return false;
        if (m->handle_lid_switch_docked != HANDLE_IGNORE)
                return false;

        return true;
}

int manager_read_utmp(Manager *m) {
#if ENABLE_UTMP
        int r;

        assert(m);

        if (utmpxname(_PATH_UTMPX) < 0)
                return log_error_errno(errno, "Failed to set utmp path to " _PATH_UTMPX ": %m");

        setutxent();

        for (;;) {
                _cleanup_free_ char *t = NULL;
                struct utmpx *u;
                const char *c;
                Session *s;

                errno = 0;
                u = getutxent();
                if (!u) {
                        if (errno != 0)
                                log_warning_errno(errno, "Failed to read " _PATH_UTMPX ", ignoring: %m");
                        r = 0;
                        break;
                }

                if (u->ut_type != USER_PROCESS)
                        continue;

                if (!pid_is_valid(u->ut_pid))
                        continue;

                t = strndup(u->ut_line, sizeof(u->ut_line));
                if (!t) {
                        r = log_oom();
                        break;
                }

                c = path_startswith(t, "/dev/");
                if (c) {
                        r = free_and_strdup(&t, c);
                        if (r < 0) {
                                log_oom();
                                break;
                        }
                }

                if (isempty(t))
                        continue;

                s = hashmap_get(m->sessions_by_leader, PID_TO_PTR(u->ut_pid));
                if (!s)
                        continue;

                if (s->tty_validity == TTY_FROM_UTMP && !streq_ptr(s->tty, t)) {
                        /* This may happen on multiplexed SSH connection (i.e. 'SSH connection sharing'). In
                         * this case PAM and utmp sessions don't match. In such a case let's invalidate the TTY
                         * information and never acquire it again. */

                        s->tty = mfree(s->tty);
                        s->tty_validity = TTY_UTMP_INCONSISTENT;
                        log_debug("Session '%s' has inconsistent TTY information, dropping TTY information.", s->id);
                        continue;
                }

                /* Never override what we figured out once */
                if (s->tty || s->tty_validity >= 0)
                        continue;

                s->tty = TAKE_PTR(t);
                s->tty_validity = TTY_FROM_UTMP;
                log_debug("Acquired TTY information '%s' from utmp for session '%s'.", s->tty, s->id);
        }

        endutxent();
        return r;
#else
        return 0;
#endif
}

#if ENABLE_UTMP
static int manager_dispatch_utmp(sd_event_source *s, const struct inotify_event *event, void *userdata) {
        Manager *m = userdata;

        assert(m);

        /* If there's indication the file itself might have been removed or became otherwise unavailable, then let's
         * reestablish the watch on whatever there's now. */
        if ((event->mask & (IN_ATTRIB|IN_DELETE_SELF|IN_MOVE_SELF|IN_Q_OVERFLOW|IN_UNMOUNT)) != 0)
                manager_connect_utmp(m);

        (void) manager_read_utmp(m);
        return 0;
}
#endif

void manager_connect_utmp(Manager *m) {
#if ENABLE_UTMP
        sd_event_source *s = NULL;
        int r;

        assert(m);

        /* Watch utmp for changes via inotify. We do this to deal with tools such as ssh, which will register the PAM
         * session early, and acquire a TTY only much later for the connection. Thus during PAM the TTY won't be known
         * yet. ssh will register itself with utmp when it finally acquired the TTY. Hence, let's make use of this, and
         * watch utmp for the TTY asynchronously. We use the PAM session's leader PID as key, to find the right entry.
         *
         * Yes, relying on utmp is pretty ugly, but it's good enough for informational purposes, as well as idle
         * detection (which, for tty sessions, relies on the TTY used) */

        r = sd_event_add_inotify(m->event, &s, _PATH_UTMPX, IN_MODIFY|IN_MOVE_SELF|IN_DELETE_SELF|IN_ATTRIB, manager_dispatch_utmp, m);
        if (r < 0)
                log_full_errno(r == -ENOENT ? LOG_DEBUG: LOG_WARNING, r, "Failed to create inotify watch on " _PATH_UTMPX ", ignoring: %m");
        else {
                r = sd_event_source_set_priority(s, SD_EVENT_PRIORITY_IDLE);
                if (r < 0)
                        log_warning_errno(r, "Failed to adjust utmp event source priority, ignoring: %m");

                (void) sd_event_source_set_description(s, "utmp");
        }

        sd_event_source_unref(m->utmp_event_source);
        m->utmp_event_source = s;
#endif
}

void manager_reconnect_utmp(Manager *m) {
#if ENABLE_UTMP
        assert(m);

        if (m->utmp_event_source)
                return;

        manager_connect_utmp(m);
#endif
}
