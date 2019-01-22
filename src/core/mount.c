/***
  This file is part of systemd.

  Copyright 2010 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/epoll.h>

#include <libmount.h>

#include "sd-messages.h"

#include "alloc-util.h"
#include "dbus-mount.h"
#include "escape.h"
#include "exit-status.h"
#include "formats-util.h"
#include "fstab-util.h"
#include "log.h"
#include "manager.h"
#include "mkdir.h"
#include "mount-setup.h"
#include "mount-util.h"
#include "mount.h"
#include "parse-util.h"
#include "path-util.h"
#include "process-util.h"
#include "special.h"
#include "string-table.h"
#include "string-util.h"
#include "strv.h"
#include "unit-name.h"
#include "unit.h"

#define RETRY_UMOUNT_MAX 32

DEFINE_TRIVIAL_CLEANUP_FUNC(struct libmnt_table*, mnt_free_table);
DEFINE_TRIVIAL_CLEANUP_FUNC(struct libmnt_iter*, mnt_free_iter);

static const UnitActiveState state_translation_table[_MOUNT_STATE_MAX] = {
        [MOUNT_DEAD] = UNIT_INACTIVE,
        [MOUNT_MOUNTING] = UNIT_ACTIVATING,
        [MOUNT_MOUNTING_DONE] = UNIT_ACTIVE,
        [MOUNT_MOUNTED] = UNIT_ACTIVE,
        [MOUNT_REMOUNTING] = UNIT_RELOADING,
        [MOUNT_UNMOUNTING] = UNIT_DEACTIVATING,
        [MOUNT_MOUNTING_SIGTERM] = UNIT_DEACTIVATING,
        [MOUNT_MOUNTING_SIGKILL] = UNIT_DEACTIVATING,
        [MOUNT_REMOUNTING_SIGTERM] = UNIT_RELOADING,
        [MOUNT_REMOUNTING_SIGKILL] = UNIT_RELOADING,
        [MOUNT_UNMOUNTING_SIGTERM] = UNIT_DEACTIVATING,
        [MOUNT_UNMOUNTING_SIGKILL] = UNIT_DEACTIVATING,
        [MOUNT_FAILED] = UNIT_FAILED
};

static int mount_dispatch_timer(sd_event_source *source, usec_t usec, void *userdata);
static int mount_dispatch_io(sd_event_source *source, int fd, uint32_t revents, void *userdata);

static bool mount_needs_network(const char *options, const char *fstype) {
        if (fstab_test_option(options, "_netdev\0"))
                return true;

        if (fstype && fstype_is_network(fstype))
                return true;

        return false;
}

static bool mount_is_network(const MountParameters *p) {
        assert(p);

        return mount_needs_network(p->options, p->fstype);
}

static bool mount_is_loop(const MountParameters *p) {
        assert(p);

        if (fstab_test_option(p->options, "loop\0"))
                return true;

        return false;
}

static bool mount_is_bind(const MountParameters *p) {
        assert(p);

        if (fstab_test_option(p->options, "bind\0" "rbind\0"))
                return true;

        if (p->fstype && STR_IN_SET(p->fstype, "bind", "rbind"))
                return true;

        return false;
}

static bool mount_is_auto(const MountParameters *p) {
        assert(p);

        return !fstab_test_option(p->options, "noauto\0");
}

static bool mount_is_automount(const MountParameters *p) {
        assert(p);

        return fstab_test_option(p->options,
                                 "comment=systemd.automount\0"
                                 "x-systemd.automount\0");
}

static bool mount_state_active(MountState state) {
        return IN_SET(state,
                      MOUNT_MOUNTING,
                      MOUNT_MOUNTING_DONE,
                      MOUNT_REMOUNTING,
                      MOUNT_UNMOUNTING,
                      MOUNT_MOUNTING_SIGTERM,
                      MOUNT_MOUNTING_SIGKILL,
                      MOUNT_UNMOUNTING_SIGTERM,
                      MOUNT_UNMOUNTING_SIGKILL,
                      MOUNT_REMOUNTING_SIGTERM,
                      MOUNT_REMOUNTING_SIGKILL);
}

static bool needs_quota(const MountParameters *p) {
        assert(p);

        /* Quotas are not enabled on network filesystems,
         * but we want them, for example, on storage connected via iscsi */
        if (p->fstype && fstype_is_network(p->fstype))
                return false;

        if (mount_is_bind(p))
                return false;

        return fstab_test_option(p->options,
                                 "usrquota\0" "grpquota\0" "quota\0" "usrjquota\0" "grpjquota\0");
}

static void mount_init(Unit *u) {
        Mount *m = MOUNT(u);

        assert(u);
        assert(u->load_state == UNIT_STUB);

        m->timeout_usec = u->manager->default_timeout_start_usec;
        m->directory_mode = 0755;

        /* We need to make sure that /usr/bin/mount is always called
         * in the same process group as us, so that the autofs kernel
         * side doesn't send us another mount request while we are
         * already trying to comply its last one. */
        m->exec_context.same_pgrp = true;

        m->control_command_id = _MOUNT_EXEC_COMMAND_INVALID;

        u->ignore_on_isolate = true;
}

static int mount_arm_timer(Mount *m, usec_t usec) {
        int r;

        assert(m);

        if (m->timer_event_source) {
                r = sd_event_source_set_time(m->timer_event_source, usec);
                if (r < 0)
                        return r;

                return sd_event_source_set_enabled(m->timer_event_source, SD_EVENT_ONESHOT);
        }

        if (usec == USEC_INFINITY)
                return 0;

        r = sd_event_add_time(
                        UNIT(m)->manager->event,
                        &m->timer_event_source,
                        CLOCK_MONOTONIC,
                        usec, 0,
                        mount_dispatch_timer, m);
        if (r < 0)
                return r;

        (void) sd_event_source_set_description(m->timer_event_source, "mount-timer");

        return 0;
}

static void mount_unwatch_control_pid(Mount *m) {
        assert(m);

        if (m->control_pid <= 0)
                return;

        unit_unwatch_pid(UNIT(m), m->control_pid);
        m->control_pid = 0;
}

static void mount_parameters_done(MountParameters *p) {
        assert(p);

        free(p->what);
        free(p->options);
        free(p->fstype);

        p->what = p->options = p->fstype = NULL;
}

static void mount_done(Unit *u) {
        Mount *m = MOUNT(u);

        assert(m);

        m->where = mfree(m->where);

        mount_parameters_done(&m->parameters_proc_self_mountinfo);
        mount_parameters_done(&m->parameters_fragment);

        m->exec_runtime = exec_runtime_unref(m->exec_runtime);
        exec_command_done_array(m->exec_command, _MOUNT_EXEC_COMMAND_MAX);
        m->control_command = NULL;

        dynamic_creds_unref(&m->dynamic_creds);

        mount_unwatch_control_pid(m);

        m->timer_event_source = sd_event_source_unref(m->timer_event_source);
}

_pure_ static MountParameters* get_mount_parameters_fragment(Mount *m) {
        assert(m);

        if (m->from_fragment)
                return &m->parameters_fragment;

        return NULL;
}

_pure_ static MountParameters* get_mount_parameters(Mount *m) {
        assert(m);

        if (m->from_proc_self_mountinfo)
                return &m->parameters_proc_self_mountinfo;

        return get_mount_parameters_fragment(m);
}

static int mount_add_mount_links(Mount *m) {
        _cleanup_free_ char *parent = NULL;
        MountParameters *pm;
        Unit *other;
        Iterator i;
        Set *s;
        int r;

        assert(m);

        if (!path_equal(m->where, "/")) {
                /* Adds in links to other mount points that might lie further
                 * up in the hierarchy */

                parent = dirname_malloc(m->where);
                if (!parent)
                        return -ENOMEM;

                r = unit_require_mounts_for(UNIT(m), parent);
                if (r < 0)
                        return r;
        }

        /* Adds in links to other mount points that might be needed
         * for the source path (if this is a bind mount or a loop mount) to be
         * available. */
        pm = get_mount_parameters_fragment(m);
        if (pm && pm->what &&
            path_is_absolute(pm->what) &&
            (mount_is_bind(pm) || mount_is_loop(pm) || !mount_is_network(pm))) {

                r = unit_require_mounts_for(UNIT(m), pm->what);
                if (r < 0)
                        return r;
        }

        /* Adds in links to other units that use this path or paths
         * further down in the hierarchy */
        s = manager_get_units_requiring_mounts_for(UNIT(m)->manager, m->where);
        SET_FOREACH(other, s, i) {

                if (other->load_state != UNIT_LOADED)
                        continue;

                if (other == UNIT(m))
                        continue;

                r = unit_add_dependency(other, UNIT_AFTER, UNIT(m), true);
                if (r < 0)
                        return r;

                if (UNIT(m)->fragment_path) {
                        /* If we have fragment configuration, then make this dependency required */
                        r = unit_add_dependency(other, UNIT_REQUIRES, UNIT(m), true);
                        if (r < 0)
                                return r;
                }
        }

        return 0;
}

static int mount_add_device_links(Mount *m) {
        MountParameters *p;
        bool device_wants_mount = false;
        int r;

        assert(m);

        p = get_mount_parameters(m);
        if (!p)
                return 0;

        if (!p->what)
                return 0;

        if (mount_is_bind(p))
                return 0;

        if (!is_device_path(p->what))
                return 0;

        /* /dev/root is a really weird thing, it's not a real device,
         * but just a path the kernel exports for the root file system
         * specified on the kernel command line. Ignore it here. */
        if (path_equal(p->what, "/dev/root"))
                return 0;

        if (path_equal(m->where, "/"))
                return 0;

        if (mount_is_auto(p) && !mount_is_automount(p) && MANAGER_IS_SYSTEM(UNIT(m)->manager))
                device_wants_mount = true;

        r = unit_add_node_link(UNIT(m), p->what, device_wants_mount, m->from_fragment ? UNIT_BINDS_TO : UNIT_REQUIRES);
        if (r < 0)
                return r;

        return 0;
}

static int mount_add_quota_links(Mount *m) {
        int r;
        MountParameters *p;

        assert(m);

        if (!MANAGER_IS_SYSTEM(UNIT(m)->manager))
                return 0;

        p = get_mount_parameters_fragment(m);
        if (!p)
                return 0;

        if (!needs_quota(p))
                return 0;

        r = unit_add_two_dependencies_by_name(UNIT(m), UNIT_BEFORE, UNIT_WANTS, SPECIAL_QUOTACHECK_SERVICE, NULL, true);
        if (r < 0)
                return r;

        r = unit_add_two_dependencies_by_name(UNIT(m), UNIT_BEFORE, UNIT_WANTS, SPECIAL_QUOTAON_SERVICE, NULL, true);
        if (r < 0)
                return r;

        return 0;
}

static bool should_umount(Mount *m) {
        MountParameters *p;

        if (PATH_IN_SET(m->where, "/", "/usr") ||
            path_startswith(m->where, "/run/initramfs"))
                return false;

        p = get_mount_parameters(m);
        if (p && fstab_test_option(p->options, "x-initrd.mount\0") &&
            !in_initrd())
                return false;

        return true;
}

static int mount_add_default_dependencies(Mount *m) {
        MountParameters *p;
        const char *after;
        int r;

        assert(m);

        if (!UNIT(m)->default_dependencies)
                return 0;

        if (!MANAGER_IS_SYSTEM(UNIT(m)->manager))
                return 0;

        /* We do not add any default dependencies to /, /usr or
         * /run/initramfs/, since they are guaranteed to stay
         * mounted the whole time, since our system is on it.
         * Also, don't bother with anything mounted below virtual
         * file systems, it's also going to be virtual, and hence
         * not worth the effort. */
        if (PATH_IN_SET(m->where, "/", "/usr") ||
            path_startswith(m->where, "/run/initramfs") ||
            path_startswith(m->where, "/proc") ||
            path_startswith(m->where, "/sys") ||
            path_startswith(m->where, "/dev"))
                return 0;

        p = get_mount_parameters(m);
        if (!p)
                return 0;

        if (mount_is_network(p)) {
                /* We order ourselves after network.target. This is
                 * primarily useful at shutdown: services that take
                 * down the network should order themselves before
                 * network.target, so that they are shut down only
                 * after this mount unit is stopped. */

                r = unit_add_dependency_by_name(UNIT(m), UNIT_AFTER, SPECIAL_NETWORK_TARGET, NULL, true);
                if (r < 0)
                        return r;

                /* We pull in network-online.target, and order
                 * ourselves after it. This is useful at start-up to
                 * actively pull in tools that want to be started
                 * before we start mounting network file systems, and
                 * whose purpose it is to delay this until the network
                 * is "up". */

                r = unit_add_two_dependencies_by_name(UNIT(m), UNIT_WANTS, UNIT_AFTER, SPECIAL_NETWORK_ONLINE_TARGET, NULL, true);
                if (r < 0)
                        return r;

                after = SPECIAL_REMOTE_FS_PRE_TARGET;
        } else
                after = SPECIAL_LOCAL_FS_PRE_TARGET;

        r = unit_add_dependency_by_name(UNIT(m), UNIT_AFTER, after, NULL, true);
        if (r < 0)
                return r;

        if (should_umount(m)) {
                r = unit_add_two_dependencies_by_name(UNIT(m), UNIT_BEFORE, UNIT_CONFLICTS, SPECIAL_UMOUNT_TARGET, NULL, true);
                if (r < 0)
                        return r;
        }

        return 0;
}

static int mount_verify(Mount *m) {
        _cleanup_free_ char *e = NULL;
        MountParameters *p;
        int r;

        assert(m);

        if (UNIT(m)->load_state != UNIT_LOADED)
                return 0;

        if (!m->from_fragment && !m->from_proc_self_mountinfo)
                return -ENOENT;

        r = unit_name_from_path(m->where, ".mount", &e);
        if (r < 0)
                return log_unit_error_errno(UNIT(m), r, "Failed to generate unit name from mount path: %m");

        if (!unit_has_name(UNIT(m), e)) {
                log_unit_error(UNIT(m), "Where= setting doesn't match unit name. Refusing.");
                return -EINVAL;
        }

        if (mount_point_is_api(m->where) || mount_point_ignore(m->where)) {
                log_unit_error(UNIT(m), "Cannot create mount unit for API file system %s. Refusing.", m->where);
                return -EINVAL;
        }

        p = get_mount_parameters_fragment(m);
        if (p && !p->what) {
                log_unit_error(UNIT(m), "What= setting is missing. Refusing.");
                return -EBADMSG;
        }

        if (m->exec_context.pam_name && m->kill_context.kill_mode != KILL_CONTROL_GROUP) {
                log_unit_error(UNIT(m), "Unit has PAM enabled. Kill mode must be set to control-group'. Refusing.");
                return -EINVAL;
        }

        return 0;
}

static int mount_add_extras(Mount *m) {
        Unit *u = UNIT(m);
        int r;

        assert(m);

        if (u->fragment_path)
                m->from_fragment = true;

        if (!m->where) {
                r = unit_name_to_path(u->id, &m->where);
                if (r < 0)
                        return r;
        }

        path_kill_slashes(m->where);

        if (!u->description) {
                r = unit_set_description(u, m->where);
                if (r < 0)
                        return r;
        }

        r = mount_add_device_links(m);
        if (r < 0)
                return r;

        r = mount_add_mount_links(m);
        if (r < 0)
                return r;

        r = mount_add_quota_links(m);
        if (r < 0)
                return r;

        r = unit_patch_contexts(u);
        if (r < 0)
                return r;

        r = unit_add_exec_dependencies(u, &m->exec_context);
        if (r < 0)
                return r;

        r = unit_set_default_slice(u);
        if (r < 0)
                return r;

        r = mount_add_default_dependencies(m);
        if (r < 0)
                return r;

        return 0;
}

static int mount_load_root_mount(Unit *u) {
        assert(u);

        if (!unit_has_name(u, SPECIAL_ROOT_MOUNT))
                return 0;

        u->perpetual = true;
        u->default_dependencies = false;

        /* The stdio/kmsg bridge socket is on /, in order to avoid a dep loop, don't use kmsg logging for -.mount */
        MOUNT(u)->exec_context.std_output = EXEC_OUTPUT_NULL;
        MOUNT(u)->exec_context.std_input = EXEC_INPUT_NULL;

        if (!u->description)
                u->description = strdup("Root Mount");

        return 1;
}

static int mount_load(Unit *u) {
        Mount *m = MOUNT(u);
        int r;

        assert(u);
        assert(u->load_state == UNIT_STUB);

        r = mount_load_root_mount(u);
        if (r < 0)
                return r;

        if (m->from_proc_self_mountinfo || u->perpetual)
                r = unit_load_fragment_and_dropin_optional(u);
        else
                r = unit_load_fragment_and_dropin(u);
        if (r < 0)
                return r;

        /* This is a new unit? Then let's add in some extras */
        if (u->load_state == UNIT_LOADED) {
                r = mount_add_extras(m);
                if (r < 0)
                        return r;
        }

        return mount_verify(m);
}

static void mount_set_state(Mount *m, MountState state) {
        MountState old_state;
        assert(m);

        old_state = m->state;
        m->state = state;

        if (!mount_state_active(state)) {
                m->timer_event_source = sd_event_source_unref(m->timer_event_source);
                mount_unwatch_control_pid(m);
                m->control_command = NULL;
                m->control_command_id = _MOUNT_EXEC_COMMAND_INVALID;
        }

        if (state != old_state)
                log_unit_debug(UNIT(m), "Changed %s -> %s", mount_state_to_string(old_state), mount_state_to_string(state));

        unit_notify(UNIT(m), state_translation_table[old_state], state_translation_table[state], m->reload_result == MOUNT_SUCCESS);
        m->reload_result = MOUNT_SUCCESS;
}

static int mount_coldplug(Unit *u) {
        Mount *m = MOUNT(u);
        MountState new_state = MOUNT_DEAD;
        int r;

        assert(m);
        assert(m->state == MOUNT_DEAD);

        if (m->deserialized_state != m->state)
                new_state = m->deserialized_state;
        else if (m->from_proc_self_mountinfo)
                new_state = MOUNT_MOUNTED;

        if (new_state == m->state)
                return 0;

        if (m->control_pid > 0 &&
            pid_is_unwaited(m->control_pid) &&
            mount_state_active(new_state)) {

                r = unit_watch_pid(UNIT(m), m->control_pid);
                if (r < 0)
                        return r;

                r = mount_arm_timer(m, usec_add(u->state_change_timestamp.monotonic, m->timeout_usec));
                if (r < 0)
                        return r;
        }

        if (!IN_SET(new_state, MOUNT_DEAD, MOUNT_FAILED))
                (void) unit_setup_dynamic_creds(u);

        mount_set_state(m, new_state);
        return 0;
}

static void mount_dump(Unit *u, FILE *f, const char *prefix) {
        Mount *m = MOUNT(u);
        MountParameters *p;

        assert(m);
        assert(f);

        p = get_mount_parameters(m);

        fprintf(f,
                "%sMount State: %s\n"
                "%sResult: %s\n"
                "%sWhere: %s\n"
                "%sWhat: %s\n"
                "%sFile System Type: %s\n"
                "%sOptions: %s\n"
                "%sFrom /proc/self/mountinfo: %s\n"
                "%sFrom fragment: %s\n"
                "%sDirectoryMode: %04o\n"
                "%sSloppyOptions: %s\n"
                "%sLazyUnmount: %s\n"
                "%sForceUnmount: %s\n",
                prefix, mount_state_to_string(m->state),
                prefix, mount_result_to_string(m->result),
                prefix, m->where,
                prefix, p ? strna(p->what) : "n/a",
                prefix, p ? strna(p->fstype) : "n/a",
                prefix, p ? strna(p->options) : "n/a",
                prefix, yes_no(m->from_proc_self_mountinfo),
                prefix, yes_no(m->from_fragment),
                prefix, m->directory_mode,
                prefix, yes_no(m->sloppy_options),
                prefix, yes_no(m->lazy_unmount),
                prefix, yes_no(m->force_unmount));

        if (m->control_pid > 0)
                fprintf(f,
                        "%sControl PID: "PID_FMT"\n",
                        prefix, m->control_pid);

        exec_context_dump(&m->exec_context, f, prefix);
        kill_context_dump(&m->kill_context, f, prefix);
}

static int mount_spawn(Mount *m, ExecCommand *c, pid_t *_pid) {
        pid_t pid;
        int r;
        ExecParameters exec_params = {
                .flags      = EXEC_APPLY_PERMISSIONS|EXEC_APPLY_CHROOT|EXEC_APPLY_TTY_STDIN,
                .stdin_fd   = -1,
                .stdout_fd  = -1,
                .stderr_fd  = -1,
        };

        assert(m);
        assert(c);
        assert(_pid);

        (void) unit_realize_cgroup(UNIT(m));
        if (m->reset_cpu_usage) {
                (void) unit_reset_cpu_usage(UNIT(m));
                m->reset_cpu_usage = false;
        }

        r = unit_setup_exec_runtime(UNIT(m));
        if (r < 0)
                return r;

        r = unit_setup_dynamic_creds(UNIT(m));
        if (r < 0)
                return r;

        r = mount_arm_timer(m, usec_add(now(CLOCK_MONOTONIC), m->timeout_usec));
        if (r < 0)
                return r;

        exec_params.environment = UNIT(m)->manager->environment;
        exec_params.flags |= UNIT(m)->manager->confirm_spawn ? EXEC_CONFIRM_SPAWN : 0;
        exec_params.cgroup_supported = UNIT(m)->manager->cgroup_supported;
        exec_params.cgroup_path = UNIT(m)->cgroup_path;
        exec_params.cgroup_delegate = m->cgroup_context.delegate;
        exec_params.runtime_prefix = manager_get_runtime_prefix(UNIT(m)->manager);

        r = exec_spawn(UNIT(m),
                       c,
                       &m->exec_context,
                       &exec_params,
                       m->exec_runtime,
                       &m->dynamic_creds,
                       &pid);
        if (r < 0)
                return r;

        r = unit_watch_pid(UNIT(m), pid);
        if (r < 0)
                /* FIXME: we need to do something here */
                return r;

        *_pid = pid;

        return 0;
}

static void mount_enter_dead(Mount *m, MountResult f) {
        assert(m);

        if (m->result == MOUNT_SUCCESS)
                m->result = f;

        mount_set_state(m, m->result != MOUNT_SUCCESS ? MOUNT_FAILED : MOUNT_DEAD);

        exec_runtime_destroy(m->exec_runtime);
        m->exec_runtime = exec_runtime_unref(m->exec_runtime);

        exec_context_destroy_runtime_directory(&m->exec_context, manager_get_runtime_prefix(UNIT(m)->manager));

        unit_unref_uid_gid(UNIT(m), true);

        dynamic_creds_destroy(&m->dynamic_creds);
}

static void mount_enter_mounted(Mount *m, MountResult f) {
        assert(m);

        if (m->result == MOUNT_SUCCESS)
                m->result = f;

        mount_set_state(m, MOUNT_MOUNTED);
}

static void mount_enter_signal(Mount *m, MountState state, MountResult f) {
        int r;

        assert(m);

        if (m->result == MOUNT_SUCCESS)
                m->result = f;

        r = unit_kill_context(
                        UNIT(m),
                        &m->kill_context,
                        (state != MOUNT_MOUNTING_SIGTERM && state != MOUNT_UNMOUNTING_SIGTERM && state != MOUNT_REMOUNTING_SIGTERM) ?
                        KILL_KILL : KILL_TERMINATE,
                        -1,
                        m->control_pid,
                        false);
        if (r < 0)
                goto fail;

        if (r > 0) {
                r = mount_arm_timer(m, usec_add(now(CLOCK_MONOTONIC), m->timeout_usec));
                if (r < 0)
                        goto fail;

                mount_set_state(m, state);
        } else if (state == MOUNT_REMOUNTING_SIGTERM)
                mount_enter_signal(m, MOUNT_REMOUNTING_SIGKILL, MOUNT_SUCCESS);
        else if (state == MOUNT_REMOUNTING_SIGKILL)
                mount_enter_mounted(m, MOUNT_SUCCESS);
        else if (state == MOUNT_MOUNTING_SIGTERM)
                mount_enter_signal(m, MOUNT_MOUNTING_SIGKILL, MOUNT_SUCCESS);
        else if (state == MOUNT_UNMOUNTING_SIGTERM)
                mount_enter_signal(m, MOUNT_UNMOUNTING_SIGKILL, MOUNT_SUCCESS);
        else
                mount_enter_dead(m, MOUNT_SUCCESS);

        return;

fail:
        log_unit_warning_errno(UNIT(m), r, "Failed to kill processes: %m");

        if (IN_SET(state, MOUNT_REMOUNTING_SIGTERM, MOUNT_REMOUNTING_SIGKILL))
                mount_enter_mounted(m, MOUNT_FAILURE_RESOURCES);
        else
                mount_enter_dead(m, MOUNT_FAILURE_RESOURCES);
}

static void mount_enter_unmounting(Mount *m) {
        int r;

        assert(m);

        /* Start counting our attempts */
        if (!IN_SET(m->state,
                    MOUNT_UNMOUNTING,
                    MOUNT_UNMOUNTING_SIGTERM,
                    MOUNT_UNMOUNTING_SIGKILL))
                m->n_retry_umount = 0;

        m->control_command_id = MOUNT_EXEC_UNMOUNT;
        m->control_command = m->exec_command + MOUNT_EXEC_UNMOUNT;

        r = exec_command_set(m->control_command, UMOUNT_PATH, m->where, NULL);
        if (r >= 0 && m->lazy_unmount)
                r = exec_command_append(m->control_command, "-l", NULL);
        if (r >= 0 && m->force_unmount)
                r = exec_command_append(m->control_command, "-f", NULL);
        if (r < 0)
                goto fail;

        mount_unwatch_control_pid(m);

        r = mount_spawn(m, m->control_command, &m->control_pid);
        if (r < 0)
                goto fail;

        mount_set_state(m, MOUNT_UNMOUNTING);

        return;

fail:
        log_unit_warning_errno(UNIT(m), r, "Failed to run 'umount' task: %m");
        mount_enter_mounted(m, MOUNT_FAILURE_RESOURCES);
}

static void mount_enter_mounting(Mount *m) {
        int r;
        MountParameters *p;

        assert(m);

        m->control_command_id = MOUNT_EXEC_MOUNT;
        m->control_command = m->exec_command + MOUNT_EXEC_MOUNT;

        r = unit_fail_if_symlink(UNIT(m), m->where);
        if (r < 0)
                goto fail;

        (void) mkdir_p_label(m->where, m->directory_mode);

        unit_warn_if_dir_nonempty(UNIT(m), m->where);

        /* Create the source directory for bind-mounts if needed */
        p = get_mount_parameters_fragment(m);
        if (p && mount_is_bind(p))
                (void) mkdir_p_label(p->what, m->directory_mode);

        if (p) {
                _cleanup_free_ char *opts = NULL;

                r = fstab_filter_options(p->options, "nofail\0" "noauto\0" "auto\0", NULL, NULL, &opts);
                if (r < 0)
                        goto fail;

                r = exec_command_set(m->control_command, MOUNT_PATH, p->what, m->where, NULL);
                if (r >= 0 && m->sloppy_options)
                        r = exec_command_append(m->control_command, "-s", NULL);
                if (r >= 0 && p->fstype)
                        r = exec_command_append(m->control_command, "-t", p->fstype, NULL);
                if (r >= 0 && !isempty(opts))
                        r = exec_command_append(m->control_command, "-o", opts, NULL);
        } else
                r = -ENOENT;

        if (r < 0)
                goto fail;

        mount_unwatch_control_pid(m);

        r = mount_spawn(m, m->control_command, &m->control_pid);
        if (r < 0)
                goto fail;

        mount_set_state(m, MOUNT_MOUNTING);

        return;

fail:
        log_unit_warning_errno(UNIT(m), r, "Failed to run 'mount' task: %m");
        mount_enter_dead(m, MOUNT_FAILURE_RESOURCES);
}

static void mount_enter_remounting(Mount *m) {
        int r;
        MountParameters *p;

        assert(m);

        m->control_command_id = MOUNT_EXEC_REMOUNT;
        m->control_command = m->exec_command + MOUNT_EXEC_REMOUNT;

        p = get_mount_parameters_fragment(m);
        if (p) {
                const char *o;

                if (p->options)
                        o = strjoina("remount,", p->options);
                else
                        o = "remount";

                r = exec_command_set(m->control_command, MOUNT_PATH,
                                     p->what, m->where,
                                     "-o", o, NULL);
                if (r >= 0 && m->sloppy_options)
                        r = exec_command_append(m->control_command, "-s", NULL);
                if (r >= 0 && p->fstype)
                        r = exec_command_append(m->control_command, "-t", p->fstype, NULL);
        } else
                r = -ENOENT;

        if (r < 0)
                goto fail;

        mount_unwatch_control_pid(m);

        r = mount_spawn(m, m->control_command, &m->control_pid);
        if (r < 0)
                goto fail;

        mount_set_state(m, MOUNT_REMOUNTING);

        return;

fail:
        log_unit_warning_errno(UNIT(m), r, "Failed to run 'remount' task: %m");
        m->reload_result = MOUNT_FAILURE_RESOURCES;
        mount_enter_mounted(m, MOUNT_SUCCESS);
}

static int mount_start(Unit *u) {
        Mount *m = MOUNT(u);
        int r;

        assert(m);

        /* We cannot fulfill this request right now, try again later
         * please! */
        if (IN_SET(m->state,
                   MOUNT_UNMOUNTING,
                   MOUNT_UNMOUNTING_SIGTERM,
                   MOUNT_UNMOUNTING_SIGKILL,
                   MOUNT_MOUNTING_SIGTERM,
                   MOUNT_MOUNTING_SIGKILL))
                return -EAGAIN;

        /* Already on it! */
        if (m->state == MOUNT_MOUNTING)
                return 0;

        assert(IN_SET(m->state, MOUNT_DEAD, MOUNT_FAILED));

        r = unit_start_limit_test(u);
        if (r < 0) {
                mount_enter_dead(m, MOUNT_FAILURE_START_LIMIT_HIT);
                return r;
        }

        r = unit_acquire_invocation_id(u);
        if (r < 0)
                return r;

        m->result = MOUNT_SUCCESS;
        m->reload_result = MOUNT_SUCCESS;
        m->reset_cpu_usage = true;

        mount_enter_mounting(m);
        return 1;
}

static int mount_stop(Unit *u) {
        Mount *m = MOUNT(u);

        assert(m);

        /* Already on it */
        if (IN_SET(m->state,
                   MOUNT_UNMOUNTING,
                   MOUNT_UNMOUNTING_SIGKILL,
                   MOUNT_UNMOUNTING_SIGTERM,
                   MOUNT_MOUNTING_SIGTERM,
                   MOUNT_MOUNTING_SIGKILL))
                return 0;

        assert(IN_SET(m->state,
                      MOUNT_MOUNTING,
                      MOUNT_MOUNTING_DONE,
                      MOUNT_MOUNTED,
                      MOUNT_REMOUNTING,
                      MOUNT_REMOUNTING_SIGTERM,
                      MOUNT_REMOUNTING_SIGKILL));

        mount_enter_unmounting(m);
        return 1;
}

static int mount_reload(Unit *u) {
        Mount *m = MOUNT(u);

        assert(m);

        if (m->state == MOUNT_MOUNTING_DONE)
                return -EAGAIN;

        assert(m->state == MOUNT_MOUNTED);

        mount_enter_remounting(m);
        return 1;
}

static int mount_serialize(Unit *u, FILE *f, FDSet *fds) {
        Mount *m = MOUNT(u);

        assert(m);
        assert(f);
        assert(fds);

        unit_serialize_item(u, f, "state", mount_state_to_string(m->state));
        unit_serialize_item(u, f, "result", mount_result_to_string(m->result));
        unit_serialize_item(u, f, "reload-result", mount_result_to_string(m->reload_result));

        if (m->control_pid > 0)
                unit_serialize_item_format(u, f, "control-pid", PID_FMT, m->control_pid);

        if (m->control_command_id >= 0)
                unit_serialize_item(u, f, "control-command", mount_exec_command_to_string(m->control_command_id));

        return 0;
}

static int mount_deserialize_item(Unit *u, const char *key, const char *value, FDSet *fds) {
        Mount *m = MOUNT(u);

        assert(u);
        assert(key);
        assert(value);
        assert(fds);

        if (streq(key, "state")) {
                MountState state;

                if ((state = mount_state_from_string(value)) < 0)
                        log_unit_debug(u, "Failed to parse state value: %s", value);
                else
                        m->deserialized_state = state;
        } else if (streq(key, "result")) {
                MountResult f;

                f = mount_result_from_string(value);
                if (f < 0)
                        log_unit_debug(u, "Failed to parse result value: %s", value);
                else if (f != MOUNT_SUCCESS)
                        m->result = f;

        } else if (streq(key, "reload-result")) {
                MountResult f;

                f = mount_result_from_string(value);
                if (f < 0)
                        log_unit_debug(u, "Failed to parse reload result value: %s", value);
                else if (f != MOUNT_SUCCESS)
                        m->reload_result = f;

        } else if (streq(key, "control-pid")) {
                pid_t pid;

                if (parse_pid(value, &pid) < 0)
                        log_unit_debug(u, "Failed to parse control-pid value: %s", value);
                else
                        m->control_pid = pid;
        } else if (streq(key, "control-command")) {
                MountExecCommand id;

                id = mount_exec_command_from_string(value);
                if (id < 0)
                        log_unit_debug(u, "Failed to parse exec-command value: %s", value);
                else {
                        m->control_command_id = id;
                        m->control_command = m->exec_command + id;
                }
        } else
                log_unit_debug(u, "Unknown serialization key: %s", key);

        return 0;
}

_pure_ static UnitActiveState mount_active_state(Unit *u) {
        assert(u);

        return state_translation_table[MOUNT(u)->state];
}

_pure_ static const char *mount_sub_state_to_string(Unit *u) {
        assert(u);

        return mount_state_to_string(MOUNT(u)->state);
}

_pure_ static bool mount_check_gc(Unit *u) {
        Mount *m = MOUNT(u);

        assert(m);

        return m->from_proc_self_mountinfo;
}

static void mount_sigchld_event(Unit *u, pid_t pid, int code, int status) {
        Mount *m = MOUNT(u);
        MountResult f;

        assert(m);
        assert(pid >= 0);

        if (pid != m->control_pid)
                return;

        m->control_pid = 0;

        if (is_clean_exit(code, status, EXIT_CLEAN_COMMAND, NULL))
                f = MOUNT_SUCCESS;
        else if (code == CLD_EXITED)
                f = MOUNT_FAILURE_EXIT_CODE;
        else if (code == CLD_KILLED)
                f = MOUNT_FAILURE_SIGNAL;
        else if (code == CLD_DUMPED)
                f = MOUNT_FAILURE_CORE_DUMP;
        else
                assert_not_reached("Unknown code");

        if (m->result == MOUNT_SUCCESS)
                m->result = f;

        if (m->control_command) {
                exec_status_exit(&m->control_command->exec_status, &m->exec_context, pid, code, status);

                m->control_command = NULL;
                m->control_command_id = _MOUNT_EXEC_COMMAND_INVALID;
        }

        log_unit_full(u, f == MOUNT_SUCCESS ? LOG_DEBUG : LOG_NOTICE, 0,
                      "Mount process exited, code=%s status=%i", sigchld_code_to_string(code), status);

        /* Note that mount(8) returning and the kernel sending us a
         * mount table change event might happen out-of-order. If an
         * operation succeed we assume the kernel will follow soon too
         * and already change into the resulting state.  If it fails
         * we check if the kernel still knows about the mount. and
         * change state accordingly. */

        switch (m->state) {

        case MOUNT_MOUNTING:
        case MOUNT_MOUNTING_DONE:
        case MOUNT_MOUNTING_SIGKILL:
        case MOUNT_MOUNTING_SIGTERM:

                if (f == MOUNT_SUCCESS || m->from_proc_self_mountinfo)
                        /* If /bin/mount returned success, or if we see the mount point in /proc/self/mountinfo we are
                         * happy. If we see the first condition first, we should see the the second condition
                         * immediately after – or /bin/mount lies to us and is broken. */
                        mount_enter_mounted(m, f);
                else
                        mount_enter_dead(m, f);
                break;

        case MOUNT_REMOUNTING:
        case MOUNT_REMOUNTING_SIGKILL:
        case MOUNT_REMOUNTING_SIGTERM:

                m->reload_result = f;
                if (m->from_proc_self_mountinfo)
                        mount_enter_mounted(m, MOUNT_SUCCESS);
                else
                        mount_enter_dead(m, MOUNT_SUCCESS);

                break;

        case MOUNT_UNMOUNTING:
        case MOUNT_UNMOUNTING_SIGKILL:
        case MOUNT_UNMOUNTING_SIGTERM:

                if (f == MOUNT_SUCCESS) {

                        if (m->from_proc_self_mountinfo) {

                                /* Still a mount point? If so, let's
                                 * try again. Most likely there were
                                 * multiple mount points stacked on
                                 * top of each other. Note that due to
                                 * the io event priority logic we can
                                 * be sure the new mountinfo is loaded
                                 * before we process the SIGCHLD for
                                 * the mount command. */

                                if (m->n_retry_umount < RETRY_UMOUNT_MAX) {
                                        log_unit_debug(u, "Mount still present, trying again.");
                                        m->n_retry_umount++;
                                        mount_enter_unmounting(m);
                                } else {
                                        log_unit_debug(u, "Mount still present after %u attempts to unmount, giving up.", m->n_retry_umount);
                                        mount_enter_mounted(m, f);
                                }
                        } else
                                mount_enter_dead(m, f);

                } else if (m->from_proc_self_mountinfo)
                        mount_enter_mounted(m, f);
                else
                        mount_enter_dead(m, f);
                break;

        default:
                assert_not_reached("Uh, control process died at wrong time.");
        }

        /* Notify clients about changed exit status */
        unit_add_to_dbus_queue(u);
}

static int mount_dispatch_timer(sd_event_source *source, usec_t usec, void *userdata) {
        Mount *m = MOUNT(userdata);

        assert(m);
        assert(m->timer_event_source == source);

        switch (m->state) {

        case MOUNT_MOUNTING:
        case MOUNT_MOUNTING_DONE:
                log_unit_warning(UNIT(m), "Mounting timed out. Stopping.");
                mount_enter_signal(m, MOUNT_MOUNTING_SIGTERM, MOUNT_FAILURE_TIMEOUT);
                break;

        case MOUNT_REMOUNTING:
                log_unit_warning(UNIT(m), "Remounting timed out. Stopping.");
                m->reload_result = MOUNT_FAILURE_TIMEOUT;
                mount_enter_mounted(m, MOUNT_SUCCESS);
                break;

        case MOUNT_UNMOUNTING:
                log_unit_warning(UNIT(m), "Unmounting timed out. Stopping.");
                mount_enter_signal(m, MOUNT_UNMOUNTING_SIGTERM, MOUNT_FAILURE_TIMEOUT);
                break;

        case MOUNT_MOUNTING_SIGTERM:
                if (m->kill_context.send_sigkill) {
                        log_unit_warning(UNIT(m), "Mounting timed out. Killing.");
                        mount_enter_signal(m, MOUNT_MOUNTING_SIGKILL, MOUNT_FAILURE_TIMEOUT);
                } else {
                        log_unit_warning(UNIT(m), "Mounting timed out. Skipping SIGKILL. Ignoring.");

                        if (m->from_proc_self_mountinfo)
                                mount_enter_mounted(m, MOUNT_FAILURE_TIMEOUT);
                        else
                                mount_enter_dead(m, MOUNT_FAILURE_TIMEOUT);
                }
                break;

        case MOUNT_REMOUNTING_SIGTERM:
                if (m->kill_context.send_sigkill) {
                        log_unit_warning(UNIT(m), "Remounting timed out. Killing.");
                        mount_enter_signal(m, MOUNT_REMOUNTING_SIGKILL, MOUNT_FAILURE_TIMEOUT);
                } else {
                        log_unit_warning(UNIT(m), "Remounting timed out. Skipping SIGKILL. Ignoring.");

                        if (m->from_proc_self_mountinfo)
                                mount_enter_mounted(m, MOUNT_FAILURE_TIMEOUT);
                        else
                                mount_enter_dead(m, MOUNT_FAILURE_TIMEOUT);
                }
                break;

        case MOUNT_UNMOUNTING_SIGTERM:
                if (m->kill_context.send_sigkill) {
                        log_unit_warning(UNIT(m), "Unmounting timed out. Killing.");
                        mount_enter_signal(m, MOUNT_UNMOUNTING_SIGKILL, MOUNT_FAILURE_TIMEOUT);
                } else {
                        log_unit_warning(UNIT(m), "Unmounting timed out. Skipping SIGKILL. Ignoring.");

                        if (m->from_proc_self_mountinfo)
                                mount_enter_mounted(m, MOUNT_FAILURE_TIMEOUT);
                        else
                                mount_enter_dead(m, MOUNT_FAILURE_TIMEOUT);
                }
                break;

        case MOUNT_MOUNTING_SIGKILL:
        case MOUNT_REMOUNTING_SIGKILL:
        case MOUNT_UNMOUNTING_SIGKILL:
                log_unit_warning(UNIT(m),"Mount process still around after SIGKILL. Ignoring.");

                if (m->from_proc_self_mountinfo)
                        mount_enter_mounted(m, MOUNT_FAILURE_TIMEOUT);
                else
                        mount_enter_dead(m, MOUNT_FAILURE_TIMEOUT);
                break;

        default:
                assert_not_reached("Timeout at wrong time.");
        }

        return 0;
}

static int mount_setup_unit(
                Manager *m,
                const char *what,
                const char *where,
                const char *options,
                const char *fstype,
                bool set_flags) {

        _cleanup_free_ char *e = NULL, *w = NULL, *o = NULL, *f = NULL;
        bool load_extras = false;
        MountParameters *p;
        bool delete, changed = false;
        Unit *u;
        int r;

        assert(m);
        assert(what);
        assert(where);
        assert(options);
        assert(fstype);

        /* Ignore API mount points. They should never be referenced in
         * dependencies ever. */
        if (mount_point_is_api(where) || mount_point_ignore(where))
                return 0;

        if (streq(fstype, "autofs"))
                return 0;

        /* probably some kind of swap, ignore */
        if (!is_path(where))
                return 0;

        r = unit_name_from_path(where, ".mount", &e);
        if (r < 0)
                return r;

        u = manager_get_unit(m, e);
        if (!u) {
                delete = true;

                r = unit_new_for_name(m, sizeof(Mount), e, &u);
                if (r < 0)
                        goto fail;

                MOUNT(u)->where = strdup(where);
                if (!MOUNT(u)->where) {
                        r = -ENOMEM;
                        goto fail;
                }

                u->source_path = strdup("/proc/self/mountinfo");
                if (!u->source_path) {
                        r = -ENOMEM;
                        goto fail;
                }

                if (MANAGER_IS_SYSTEM(m)) {
                        const char* target;

                        target = mount_needs_network(options, fstype) ?  SPECIAL_REMOTE_FS_TARGET : SPECIAL_LOCAL_FS_TARGET;
                        r = unit_add_dependency_by_name(u, UNIT_BEFORE, target, NULL, true);
                        if (r < 0)
                                goto fail;

                        if (should_umount(MOUNT(u))) {
                                r = unit_add_dependency_by_name(u, UNIT_CONFLICTS, SPECIAL_UMOUNT_TARGET, NULL, true);
                                if (r < 0)
                                        goto fail;
                        }
                }

                unit_add_to_load_queue(u);
                changed = true;
        } else {
                delete = false;

                if (!MOUNT(u)->where) {
                        MOUNT(u)->where = strdup(where);
                        if (!MOUNT(u)->where) {
                                r = -ENOMEM;
                                goto fail;
                        }
                }

                if (MANAGER_IS_SYSTEM(m) &&
                    mount_needs_network(options, fstype)) {
                        /* _netdev option may have shown up late, or on a
                         * remount. Add remote-fs dependencies, even though
                         * local-fs ones may already be there. */
                        unit_add_dependency_by_name(u, UNIT_BEFORE, SPECIAL_REMOTE_FS_TARGET, NULL, true);
                        load_extras = true;
                }

                if (u->load_state == UNIT_NOT_FOUND) {
                        u->load_state = UNIT_LOADED;
                        u->load_error = 0;

                        /* Load in the extras later on, after we
                         * finished initialization of the unit */
                        load_extras = true;
                        changed = true;
                }
        }

        w = strdup(what);
        o = strdup(options);
        f = strdup(fstype);
        if (!w || !o || !f) {
                r = -ENOMEM;
                goto fail;
        }

        p = &MOUNT(u)->parameters_proc_self_mountinfo;

        changed = changed ||
                !streq_ptr(p->options, options) ||
                !streq_ptr(p->what, what) ||
                !streq_ptr(p->fstype, fstype);

        if (set_flags) {
                MOUNT(u)->is_mounted = true;
                MOUNT(u)->just_mounted = !MOUNT(u)->from_proc_self_mountinfo;
                MOUNT(u)->just_changed = changed;
        }

        MOUNT(u)->from_proc_self_mountinfo = true;

        free_and_replace(p->what, w);
        free_and_replace(p->options, o);
        free_and_replace(p->fstype, f);

        if (load_extras) {
                r = mount_add_extras(MOUNT(u));
                if (r < 0)
                        goto fail;
        }

        if (changed)
                unit_add_to_dbus_queue(u);

        return 0;

fail:
        log_warning_errno(r, "Failed to set up mount unit: %m");

        if (delete && u)
                unit_free(u);

        return r;
}

static int mount_load_proc_self_mountinfo(Manager *m, bool set_flags) {
        _cleanup_(mnt_free_tablep) struct libmnt_table *t = NULL;
        _cleanup_(mnt_free_iterp) struct libmnt_iter *i = NULL;
        int r = 0;

        assert(m);

        t = mnt_new_table();
        if (!t)
                return log_oom();

        i = mnt_new_iter(MNT_ITER_FORWARD);
        if (!i)
                return log_oom();

        r = mnt_table_parse_mtab(t, NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to parse /proc/self/mountinfo: %m");

        r = 0;
        for (;;) {
                const char *device, *path, *options, *fstype;
                _cleanup_free_ char *d = NULL, *p = NULL;
                struct libmnt_fs *fs;
                int k;

                k = mnt_table_next_fs(t, i, &fs);
                if (k == 1)
                        break;
                if (k < 0)
                        return log_error_errno(k, "Failed to get next entry from /proc/self/mountinfo: %m");

                device = mnt_fs_get_source(fs);
                path = mnt_fs_get_target(fs);
                options = mnt_fs_get_options(fs);
                fstype = mnt_fs_get_fstype(fs);

                if (!device || !path)
                        continue;

                if (cunescape(device, UNESCAPE_RELAX, &d) < 0)
                        return log_oom();

                if (cunescape(path, UNESCAPE_RELAX, &p) < 0)
                        return log_oom();

                (void) device_found_node(m, d, true, DEVICE_FOUND_MOUNT, set_flags);

                k = mount_setup_unit(m, d, p, options, fstype, set_flags);
                if (r == 0 && k < 0)
                        r = k;
        }

        return r;
}

static void mount_shutdown(Manager *m) {

        assert(m);

        m->mount_event_source = sd_event_source_unref(m->mount_event_source);

        mnt_unref_monitor(m->mount_monitor);
        m->mount_monitor = NULL;
}

static int mount_get_timeout(Unit *u, usec_t *timeout) {
        Mount *m = MOUNT(u);
        usec_t t;
        int r;

        if (!m->timer_event_source)
                return 0;

        r = sd_event_source_get_time(m->timer_event_source, &t);
        if (r < 0)
                return r;
        if (t == USEC_INFINITY)
                return 0;

        *timeout = t;
        return 1;
}

static int synthesize_root_mount(Manager *m) {
        Unit *u;
        int r;

        assert(m);

        /* Whatever happens, we know for sure that the root directory is around, and cannot go away. Let's
         * unconditionally synthesize it here and mark it as perpetual. */

        u = manager_get_unit(m, SPECIAL_ROOT_MOUNT);
        if (!u) {
                r = unit_new_for_name(m, sizeof(Mount), SPECIAL_ROOT_MOUNT, &u);
                if (r < 0)
                        return log_error_errno(r, "Failed to allocate the special " SPECIAL_ROOT_MOUNT " unit: %m");
        }

        u->perpetual = true;
        MOUNT(u)->deserialized_state = MOUNT_MOUNTED;

        unit_add_to_load_queue(u);
        unit_add_to_dbus_queue(u);

        return 0;
}

static bool mount_is_mounted(Mount *m) {
        assert(m);

        return UNIT(m)->perpetual || m->is_mounted;
}

static void mount_enumerate(Manager *m) {
        int r;

        assert(m);

        r = synthesize_root_mount(m);
        if (r < 0)
                goto fail;

        mnt_init_debug(0);

        if (!m->mount_monitor) {
                int fd;

                m->mount_monitor = mnt_new_monitor();
                if (!m->mount_monitor) {
                        log_oom();
                        goto fail;
                }

                r = mnt_monitor_enable_kernel(m->mount_monitor, 1);
                if (r < 0) {
                        log_error_errno(r, "Failed to enable watching of kernel mount events: %m");
                        goto fail;
                }

                r = mnt_monitor_enable_userspace(m->mount_monitor, 1, NULL);
                if (r < 0) {
                        log_error_errno(r, "Failed to enable watching of userspace mount events: %m");
                        goto fail;
                }

                /* mnt_unref_monitor() will close the fd */
                fd = r = mnt_monitor_get_fd(m->mount_monitor);
                if (r < 0) {
                        log_error_errno(r, "Failed to acquire watch file descriptor: %m");
                        goto fail;
                }

                r = sd_event_add_io(m->event, &m->mount_event_source, fd, EPOLLIN, mount_dispatch_io, m);
                if (r < 0) {
                        log_error_errno(r, "Failed to watch mount file descriptor: %m");
                        goto fail;
                }

                r = sd_event_source_set_priority(m->mount_event_source, -10);
                if (r < 0) {
                        log_error_errno(r, "Failed to adjust mount watch priority: %m");
                        goto fail;
                }

                (void) sd_event_source_set_description(m->mount_event_source, "mount-monitor-dispatch");
        }

        r = mount_load_proc_self_mountinfo(m, false);
        if (r < 0)
                goto fail;

        return;

fail:
        mount_shutdown(m);
}

static int mount_dispatch_io(sd_event_source *source, int fd, uint32_t revents, void *userdata) {
        _cleanup_set_free_ Set *around = NULL, *gone = NULL;
        Manager *m = userdata;
        const char *what;
        Iterator i;
        Unit *u;
        int r;

        assert(m);
        assert(revents & EPOLLIN);

        if (fd == mnt_monitor_get_fd(m->mount_monitor)) {
                bool rescan = false;

                /* Drain all events and verify that the event is valid.
                 *
                 * Note that libmount also monitors /run/mount mkdir if the
                 * directory does not exist yet. The mkdir may generate event
                 * which is irrelevant for us.
                 *
                 * error: r < 0; valid: r == 0, false positive: rc == 1 */
                do {
                        r = mnt_monitor_next_change(m->mount_monitor, NULL, NULL);
                        if (r == 0)
                                rescan = true;
                        else if (r < 0)
                                return log_error_errno(r, "Failed to drain libmount events");
                } while (r == 0);

                log_debug("libmount event [rescan: %s]", yes_no(rescan));
                if (!rescan)
                        return 0;
        }

        r = mount_load_proc_self_mountinfo(m, true);
        if (r < 0) {
                /* Reset flags, just in case, for later calls */
                LIST_FOREACH(units_by_type, u, m->units_by_type[UNIT_MOUNT]) {
                        Mount *mount = MOUNT(u);

                        mount->is_mounted = mount->just_mounted = mount->just_changed = false;
                }

                return 0;
        }

        manager_dispatch_load_queue(m);

        LIST_FOREACH(units_by_type, u, m->units_by_type[UNIT_MOUNT]) {
                Mount *mount = MOUNT(u);

                if (!mount_is_mounted(mount)) {

                        /* A mount point is not around right now. It
                         * might be gone, or might never have
                         * existed. */

                        if (mount->from_proc_self_mountinfo &&
                            mount->parameters_proc_self_mountinfo.what) {

                                /* Remember that this device might just have disappeared */
                                if (set_ensure_allocated(&gone, &string_hash_ops) < 0 ||
                                    set_put(gone, mount->parameters_proc_self_mountinfo.what) < 0)
                                        log_oom(); /* we don't care too much about OOM here... */
                        }

                        mount->from_proc_self_mountinfo = false;

                        switch (mount->state) {

                        case MOUNT_MOUNTED:
                                /* This has just been unmounted by
                                 * somebody else, follow the state
                                 * change. */
                                mount->result = MOUNT_SUCCESS; /* make sure we forget any earlier umount failures */
                                mount_enter_dead(mount, MOUNT_SUCCESS);
                                break;

                        default:
                                break;
                        }

                } else if (mount->just_mounted || mount->just_changed) {

                        /* A mount point was added or changed */

                        switch (mount->state) {

                        case MOUNT_DEAD:
                        case MOUNT_FAILED:

                                /* This has just been mounted by somebody else, follow the state change, but let's
                                 * generate a new invocation ID for this implicitly and automatically. */
                                (void) unit_acquire_invocation_id(UNIT(mount));
                                mount_enter_mounted(mount, MOUNT_SUCCESS);
                                break;

                        case MOUNT_MOUNTING:
                                mount_set_state(mount, MOUNT_MOUNTING_DONE);
                                break;

                        default:
                                /* Nothing really changed, but let's
                                 * issue an notification call
                                 * nonetheless, in case somebody is
                                 * waiting for this. (e.g. file system
                                 * ro/rw remounts.) */
                                mount_set_state(mount, mount->state);
                                break;
                        }
                }

                if (mount_is_mounted(mount) &&
                    mount->from_proc_self_mountinfo &&
                    mount->parameters_proc_self_mountinfo.what) {

                        if (set_ensure_allocated(&around, &string_hash_ops) < 0 ||
                            set_put(around, mount->parameters_proc_self_mountinfo.what) < 0)
                                log_oom();
                }

                /* Reset the flags for later calls */
                mount->is_mounted = mount->just_mounted = mount->just_changed = false;
        }

        SET_FOREACH(what, gone, i) {
                if (set_contains(around, what))
                        continue;

                /* Let the device units know that the device is no longer mounted */
                (void) device_found_node(m, what, false, DEVICE_FOUND_MOUNT, true);
        }

        return 0;
}

static void mount_reset_failed(Unit *u) {
        Mount *m = MOUNT(u);

        assert(m);

        if (m->state == MOUNT_FAILED)
                mount_set_state(m, MOUNT_DEAD);

        m->result = MOUNT_SUCCESS;
        m->reload_result = MOUNT_SUCCESS;
}

static int mount_kill(Unit *u, KillWho who, int signo, sd_bus_error *error) {
        return unit_kill_common(u, who, signo, -1, MOUNT(u)->control_pid, error);
}

static int mount_control_pid(Unit *u) {
        Mount *m = MOUNT(u);

        assert(m);

        return m->control_pid;
}

static const char* const mount_exec_command_table[_MOUNT_EXEC_COMMAND_MAX] = {
        [MOUNT_EXEC_MOUNT] = "ExecMount",
        [MOUNT_EXEC_UNMOUNT] = "ExecUnmount",
        [MOUNT_EXEC_REMOUNT] = "ExecRemount",
};

DEFINE_STRING_TABLE_LOOKUP(mount_exec_command, MountExecCommand);

static const char* const mount_result_table[_MOUNT_RESULT_MAX] = {
        [MOUNT_SUCCESS] = "success",
        [MOUNT_FAILURE_RESOURCES] = "resources",
        [MOUNT_FAILURE_TIMEOUT] = "timeout",
        [MOUNT_FAILURE_EXIT_CODE] = "exit-code",
        [MOUNT_FAILURE_SIGNAL] = "signal",
        [MOUNT_FAILURE_CORE_DUMP] = "core-dump",
        [MOUNT_FAILURE_START_LIMIT_HIT] = "start-limit-hit",
};

DEFINE_STRING_TABLE_LOOKUP(mount_result, MountResult);

const UnitVTable mount_vtable = {
        .object_size = sizeof(Mount),
        .exec_context_offset = offsetof(Mount, exec_context),
        .cgroup_context_offset = offsetof(Mount, cgroup_context),
        .kill_context_offset = offsetof(Mount, kill_context),
        .exec_runtime_offset = offsetof(Mount, exec_runtime),
        .dynamic_creds_offset = offsetof(Mount, dynamic_creds),

        .sections =
                "Unit\0"
                "Mount\0"
                "Install\0",
        .private_section = "Mount",

        .init = mount_init,
        .load = mount_load,
        .done = mount_done,

        .coldplug = mount_coldplug,

        .dump = mount_dump,

        .start = mount_start,
        .stop = mount_stop,
        .reload = mount_reload,

        .kill = mount_kill,

        .serialize = mount_serialize,
        .deserialize_item = mount_deserialize_item,

        .active_state = mount_active_state,
        .sub_state_to_string = mount_sub_state_to_string,

        .check_gc = mount_check_gc,

        .sigchld_event = mount_sigchld_event,

        .reset_failed = mount_reset_failed,

        .control_pid = mount_control_pid,

        .bus_vtable = bus_mount_vtable,
        .bus_set_property = bus_mount_set_property,
        .bus_commit_properties = bus_mount_commit_properties,

        .get_timeout = mount_get_timeout,

        .can_transient = true,

        .enumerate = mount_enumerate,
        .shutdown = mount_shutdown,

        .status_message_formats = {
                .starting_stopping = {
                        [0] = "Mounting %s...",
                        [1] = "Unmounting %s...",
                },
                .finished_start_job = {
                        [JOB_DONE]       = "Mounted %s.",
                        [JOB_FAILED]     = "Failed to mount %s.",
                        [JOB_TIMEOUT]    = "Timed out mounting %s.",
                },
                .finished_stop_job = {
                        [JOB_DONE]       = "Unmounted %s.",
                        [JOB_FAILED]     = "Failed unmounting %s.",
                        [JOB_TIMEOUT]    = "Timed out unmounting %s.",
                },
        },
};
