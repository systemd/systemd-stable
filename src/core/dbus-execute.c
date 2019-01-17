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

#include <sys/mount.h>
#include <sys/prctl.h>

#if HAVE_SECCOMP
#include <seccomp.h>
#endif

#include "af-list.h"
#include "alloc-util.h"
#include "bus-util.h"
#include "capability-util.h"
#include "cap-list.h"
#include "dbus-execute.h"
#include "env-util.h"
#include "errno-list.h"
#include "execute.h"
#include "fd-util.h"
#include "fileio.h"
#include "ioprio.h"
#include "missing.h"
#include "mount-util.h"
#include "namespace.h"
#include "parse-util.h"
#include "path-util.h"
#include "process-util.h"
#include "rlimit-util.h"
#if HAVE_SECCOMP
#include "seccomp-util.h"
#endif
#include "securebits-util.h"
#include "strv.h"
#include "syslog-util.h"
#include "unit-printf.h"
#include "user-util.h"
#include "utf8.h"

BUS_DEFINE_PROPERTY_GET_ENUM(bus_property_get_exec_output, exec_output, ExecOutput);
static BUS_DEFINE_PROPERTY_GET_ENUM(property_get_exec_input, exec_input, ExecInput);

static BUS_DEFINE_PROPERTY_GET_ENUM(property_get_exec_utmp_mode, exec_utmp_mode, ExecUtmpMode);
static BUS_DEFINE_PROPERTY_GET_ENUM(property_get_exec_preserve_mode, exec_preserve_mode, ExecPreserveMode);
static BUS_DEFINE_PROPERTY_GET_ENUM(property_get_exec_keyring_mode, exec_keyring_mode, ExecKeyringMode);

static BUS_DEFINE_PROPERTY_GET_ENUM(bus_property_get_protect_home, protect_home, ProtectHome);
static BUS_DEFINE_PROPERTY_GET_ENUM(bus_property_get_protect_system, protect_system, ProtectSystem);

static int property_get_environment_files(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;
        char **j;
        int r;

        assert(bus);
        assert(reply);
        assert(c);

        r = sd_bus_message_open_container(reply, 'a', "(sb)");
        if (r < 0)
                return r;

        STRV_FOREACH(j, c->environment_files) {
                const char *fn = *j;

                r = sd_bus_message_append(reply, "(sb)", fn[0] == '-' ? fn + 1 : fn, fn[0] == '-');
                if (r < 0)
                        return r;
        }

        return sd_bus_message_close_container(reply);
}

static int property_get_oom_score_adjust(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {


        ExecContext *c = userdata;
        int32_t n;

        assert(bus);
        assert(reply);
        assert(c);

        if (c->oom_score_adjust_set)
                n = c->oom_score_adjust;
        else {
                _cleanup_free_ char *t = NULL;

                n = 0;
                if (read_one_line_file("/proc/self/oom_score_adj", &t) >= 0)
                        safe_atoi32(t, &n);
        }

        return sd_bus_message_append(reply, "i", n);
}

static int property_get_nice(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {


        ExecContext *c = userdata;
        int32_t n;

        assert(bus);
        assert(reply);
        assert(c);

        if (c->nice_set)
                n = c->nice;
        else {
                errno = 0;
                n = getpriority(PRIO_PROCESS, 0);
                if (errno > 0)
                        n = 0;
        }

        return sd_bus_message_append(reply, "i", n);
}

static int property_get_ioprio(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {


        ExecContext *c = userdata;

        assert(bus);
        assert(reply);
        assert(c);

        return sd_bus_message_append(reply, "i", exec_context_get_effective_ioprio(c));
}

static int property_get_ioprio_class(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {


        ExecContext *c = userdata;

        assert(bus);
        assert(reply);
        assert(c);

        return sd_bus_message_append(reply, "i", IOPRIO_PRIO_CLASS(exec_context_get_effective_ioprio(c)));
}

static int property_get_ioprio_priority(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {


        ExecContext *c = userdata;

        assert(bus);
        assert(reply);
        assert(c);

        return sd_bus_message_append(reply, "i", IOPRIO_PRIO_DATA(exec_context_get_effective_ioprio(c)));
}

static int property_get_cpu_sched_policy(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;
        int32_t n;

        assert(bus);
        assert(reply);
        assert(c);

        if (c->cpu_sched_set)
                n = c->cpu_sched_policy;
        else {
                n = sched_getscheduler(0);
                if (n < 0)
                        n = SCHED_OTHER;
        }

        return sd_bus_message_append(reply, "i", n);
}

static int property_get_cpu_sched_priority(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;
        int32_t n;

        assert(bus);
        assert(reply);
        assert(c);

        if (c->cpu_sched_set)
                n = c->cpu_sched_priority;
        else {
                struct sched_param p = {};

                if (sched_getparam(0, &p) >= 0)
                        n = p.sched_priority;
                else
                        n = 0;
        }

        return sd_bus_message_append(reply, "i", n);
}

static int property_get_cpu_affinity(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;

        assert(bus);
        assert(reply);
        assert(c);

        if (c->cpuset)
                return sd_bus_message_append_array(reply, 'y', c->cpuset, CPU_ALLOC_SIZE(c->cpuset_ncpus));
        else
                return sd_bus_message_append_array(reply, 'y', NULL, 0);
}

static int property_get_timer_slack_nsec(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;
        uint64_t u;

        assert(bus);
        assert(reply);
        assert(c);

        if (c->timer_slack_nsec != NSEC_INFINITY)
                u = (uint64_t) c->timer_slack_nsec;
        else
                u = (uint64_t) prctl(PR_GET_TIMERSLACK);

        return sd_bus_message_append(reply, "t", u);
}

static int property_get_capability_bounding_set(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;

        assert(bus);
        assert(reply);
        assert(c);

        return sd_bus_message_append(reply, "t", c->capability_bounding_set);
}

static int property_get_ambient_capabilities(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;

        assert(bus);
        assert(reply);
        assert(c);

        return sd_bus_message_append(reply, "t", c->capability_ambient_set);
}

static int property_get_empty_string(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        assert(bus);
        assert(reply);

        return sd_bus_message_append(reply, "s", "");
}

static int property_get_syscall_filter(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;
        _cleanup_strv_free_ char **l = NULL;
        int r;

#if HAVE_SECCOMP
        Iterator i;
        void *id;
#endif

        assert(bus);
        assert(reply);
        assert(c);

        r = sd_bus_message_open_container(reply, 'r', "bas");
        if (r < 0)
                return r;

        r = sd_bus_message_append(reply, "b", c->syscall_whitelist);
        if (r < 0)
                return r;

#if HAVE_SECCOMP
        SET_FOREACH(id, c->syscall_filter, i) {
                char *name;

                name = seccomp_syscall_resolve_num_arch(SCMP_ARCH_NATIVE, PTR_TO_INT(id) - 1);
                if (!name)
                        continue;

                r = strv_consume(&l, name);
                if (r < 0)
                        return r;
        }
#endif

        strv_sort(l);

        r = sd_bus_message_append_strv(reply, l);
        if (r < 0)
                return r;

        return sd_bus_message_close_container(reply);
}

static int property_get_syscall_archs(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;
        _cleanup_strv_free_ char **l = NULL;
        int r;

#if HAVE_SECCOMP
        Iterator i;
        void *id;
#endif

        assert(bus);
        assert(reply);
        assert(c);

#if HAVE_SECCOMP
        SET_FOREACH(id, c->syscall_archs, i) {
                const char *name;

                name = seccomp_arch_to_string(PTR_TO_UINT32(id) - 1);
                if (!name)
                        continue;

                r = strv_extend(&l, name);
                if (r < 0)
                        return -ENOMEM;
        }
#endif

        strv_sort(l);

        r = sd_bus_message_append_strv(reply, l);
        if (r < 0)
                return r;

        return 0;
}

static int property_get_syscall_errno(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;

        assert(bus);
        assert(reply);
        assert(c);

        return sd_bus_message_append(reply, "i", (int32_t) c->syscall_errno);
}

static int property_get_selinux_context(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;

        assert(bus);
        assert(reply);
        assert(c);

        return sd_bus_message_append(reply, "(bs)", c->selinux_context_ignore, c->selinux_context);
}

static int property_get_apparmor_profile(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;

        assert(bus);
        assert(reply);
        assert(c);

        return sd_bus_message_append(reply, "(bs)", c->apparmor_profile_ignore, c->apparmor_profile);
}

static int property_get_smack_process_label(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;

        assert(bus);
        assert(reply);
        assert(c);

        return sd_bus_message_append(reply, "(bs)", c->smack_process_label_ignore, c->smack_process_label);
}

static int property_get_personality(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;

        assert(bus);
        assert(reply);
        assert(c);

        return sd_bus_message_append(reply, "s", personality_to_string(c->personality));
}

static int property_get_address_families(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;
        _cleanup_strv_free_ char **l = NULL;
        Iterator i;
        void *af;
        int r;

        assert(bus);
        assert(reply);
        assert(c);

        r = sd_bus_message_open_container(reply, 'r', "bas");
        if (r < 0)
                return r;

        r = sd_bus_message_append(reply, "b", c->address_families_whitelist);
        if (r < 0)
                return r;

        SET_FOREACH(af, c->address_families, i) {
                const char *name;

                name = af_to_name(PTR_TO_INT(af));
                if (!name)
                        continue;

                r = strv_extend(&l, name);
                if (r < 0)
                        return -ENOMEM;
        }

        strv_sort(l);

        r = sd_bus_message_append_strv(reply, l);
        if (r < 0)
                return r;

        return sd_bus_message_close_container(reply);
}

static int property_get_working_directory(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;
        const char *wd;

        assert(bus);
        assert(reply);
        assert(c);

        if (c->working_directory_home)
                wd = "~";
        else
                wd = c->working_directory;

        if (c->working_directory_missing_ok)
                wd = strjoina("!", wd);

        return sd_bus_message_append(reply, "s", wd);
}

static int property_get_syslog_level(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;

        assert(bus);
        assert(reply);
        assert(c);

        return sd_bus_message_append(reply, "i", LOG_PRI(c->syslog_priority));
}

static int property_get_syslog_facility(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;

        assert(bus);
        assert(reply);
        assert(c);

        return sd_bus_message_append(reply, "i", LOG_FAC(c->syslog_priority));
}

static int property_get_input_fdname(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;
        const char *name;

        assert(bus);
        assert(c);
        assert(property);
        assert(reply);

        name = exec_context_fdname(c, STDIN_FILENO);

        return sd_bus_message_append(reply, "s", name);
}

static int property_get_output_fdname(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;
        const char *name = NULL;

        assert(bus);
        assert(c);
        assert(property);
        assert(reply);

        if (c->std_output == EXEC_OUTPUT_NAMED_FD && streq(property, "StandardOutputFileDescriptorName"))
                name = exec_context_fdname(c, STDOUT_FILENO);
        else if (c->std_error == EXEC_OUTPUT_NAMED_FD && streq(property, "StandardErrorFileDescriptorName"))
                name = exec_context_fdname(c, STDERR_FILENO);

        return sd_bus_message_append(reply, "s", name);
}

static int property_get_bind_paths(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;
        unsigned i;
        bool ro;
        int r;

        assert(bus);
        assert(c);
        assert(property);
        assert(reply);

        ro = !!strstr(property, "ReadOnly");

        r = sd_bus_message_open_container(reply, 'a', "(ssbt)");
        if (r < 0)
                return r;

        for (i = 0; i < c->n_bind_mounts; i++) {

                if (ro != c->bind_mounts[i].read_only)
                        continue;

                r = sd_bus_message_append(
                                reply, "(ssbt)",
                                c->bind_mounts[i].source,
                                c->bind_mounts[i].destination,
                                c->bind_mounts[i].ignore_enoent,
                                c->bind_mounts[i].recursive ? (uint64_t) MS_REC : (uint64_t) 0);
                if (r < 0)
                        return r;
        }

        return sd_bus_message_close_container(reply);
}

const sd_bus_vtable bus_exec_vtable[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_PROPERTY("Environment", "as", NULL, offsetof(ExecContext, environment), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("EnvironmentFiles", "a(sb)", property_get_environment_files, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("PassEnvironment", "as", NULL, offsetof(ExecContext, pass_environment), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("UnsetEnvironment", "as", NULL, offsetof(ExecContext, unset_environment), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("UMask", "u", bus_property_get_mode, offsetof(ExecContext, umask), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitCPU", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_CPU]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitCPUSoft", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_CPU]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitFSIZE", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_FSIZE]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitFSIZESoft", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_FSIZE]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitDATA", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_DATA]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitDATASoft", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_DATA]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitSTACK", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_STACK]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitSTACKSoft", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_STACK]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitCORE", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_CORE]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitCORESoft", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_CORE]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitRSS", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_RSS]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitRSSSoft", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_RSS]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitNOFILE", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_NOFILE]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitNOFILESoft", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_NOFILE]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitAS", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_AS]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitASSoft", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_AS]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitNPROC", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_NPROC]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitNPROCSoft", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_NPROC]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitMEMLOCK", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_MEMLOCK]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitMEMLOCKSoft", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_MEMLOCK]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitLOCKS", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_LOCKS]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitLOCKSSoft", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_LOCKS]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitSIGPENDING", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_SIGPENDING]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitSIGPENDINGSoft", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_SIGPENDING]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitMSGQUEUE", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_MSGQUEUE]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitMSGQUEUESoft", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_MSGQUEUE]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitNICE", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_NICE]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitNICESoft", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_NICE]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitRTPRIO", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_RTPRIO]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitRTPRIOSoft", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_RTPRIO]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitRTTIME", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_RTTIME]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitRTTIMESoft", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_RTTIME]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("WorkingDirectory", "s", property_get_working_directory, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("RootDirectory", "s", NULL, offsetof(ExecContext, root_directory), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("RootImage", "s", NULL, offsetof(ExecContext, root_image), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("OOMScoreAdjust", "i", property_get_oom_score_adjust, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Nice", "i", property_get_nice, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("IOSchedulingClass", "i", property_get_ioprio_class, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("IOSchedulingPriority", "i", property_get_ioprio_priority, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("CPUSchedulingPolicy", "i", property_get_cpu_sched_policy, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("CPUSchedulingPriority", "i", property_get_cpu_sched_priority, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("CPUAffinity", "ay", property_get_cpu_affinity, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("TimerSlackNSec", "t", property_get_timer_slack_nsec, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("CPUSchedulingResetOnFork", "b", bus_property_get_bool, offsetof(ExecContext, cpu_sched_reset_on_fork), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("NonBlocking", "b", bus_property_get_bool, offsetof(ExecContext, non_blocking), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("StandardInput", "s", property_get_exec_input, offsetof(ExecContext, std_input), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("StandardInputFileDescriptorName", "s", property_get_input_fdname, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("StandardOutput", "s", bus_property_get_exec_output, offsetof(ExecContext, std_output), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("StandardOutputFileDescriptorName", "s", property_get_output_fdname, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("StandardError", "s", bus_property_get_exec_output, offsetof(ExecContext, std_error), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("StandardErrorFileDescriptorName", "s", property_get_output_fdname, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("TTYPath", "s", NULL, offsetof(ExecContext, tty_path), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("TTYReset", "b", bus_property_get_bool, offsetof(ExecContext, tty_reset), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("TTYVHangup", "b", bus_property_get_bool, offsetof(ExecContext, tty_vhangup), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("TTYVTDisallocate", "b", bus_property_get_bool, offsetof(ExecContext, tty_vt_disallocate), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("SyslogPriority", "i", bus_property_get_int, offsetof(ExecContext, syslog_priority), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("SyslogIdentifier", "s", NULL, offsetof(ExecContext, syslog_identifier), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("SyslogLevelPrefix", "b", bus_property_get_bool, offsetof(ExecContext, syslog_level_prefix), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("SyslogLevel", "i", property_get_syslog_level, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("SyslogFacility", "i", property_get_syslog_facility, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("SecureBits", "i", bus_property_get_int, offsetof(ExecContext, secure_bits), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("CapabilityBoundingSet", "t", property_get_capability_bounding_set, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("AmbientCapabilities", "t", property_get_ambient_capabilities, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("User", "s", NULL, offsetof(ExecContext, user), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Group", "s", NULL, offsetof(ExecContext, group), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("DynamicUser", "b", bus_property_get_bool, offsetof(ExecContext, dynamic_user), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("RemoveIPC", "b", bus_property_get_bool, offsetof(ExecContext, remove_ipc), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("SupplementaryGroups", "as", NULL, offsetof(ExecContext, supplementary_groups), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("PAMName", "s", NULL, offsetof(ExecContext, pam_name), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("ReadWritePaths", "as", NULL, offsetof(ExecContext, read_write_paths), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("ReadOnlyPaths", "as", NULL, offsetof(ExecContext, read_only_paths), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("InaccessiblePaths", "as", NULL, offsetof(ExecContext, inaccessible_paths), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("MountFlags", "t", bus_property_get_ulong, offsetof(ExecContext, mount_flags), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("PrivateTmp", "b", bus_property_get_bool, offsetof(ExecContext, private_tmp), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("PrivateDevices", "b", bus_property_get_bool, offsetof(ExecContext, private_devices), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("ProtectKernelTunables", "b", bus_property_get_bool, offsetof(ExecContext, protect_kernel_tunables), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("ProtectKernelModules", "b", bus_property_get_bool, offsetof(ExecContext, protect_kernel_modules), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("ProtectControlGroups", "b", bus_property_get_bool, offsetof(ExecContext, protect_control_groups), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("PrivateNetwork", "b", bus_property_get_bool, offsetof(ExecContext, private_network), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("PrivateUsers", "b", bus_property_get_bool, offsetof(ExecContext, private_users), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("ProtectHome", "s", bus_property_get_protect_home, offsetof(ExecContext, protect_home), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("ProtectSystem", "s", bus_property_get_protect_system, offsetof(ExecContext, protect_system), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("SameProcessGroup", "b", bus_property_get_bool, offsetof(ExecContext, same_pgrp), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("UtmpIdentifier", "s", NULL, offsetof(ExecContext, utmp_id), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("UtmpMode", "s", property_get_exec_utmp_mode, offsetof(ExecContext, utmp_mode), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("SELinuxContext", "(bs)", property_get_selinux_context, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("AppArmorProfile", "(bs)", property_get_apparmor_profile, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("SmackProcessLabel", "(bs)", property_get_smack_process_label, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("IgnoreSIGPIPE", "b", bus_property_get_bool, offsetof(ExecContext, ignore_sigpipe), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("NoNewPrivileges", "b", bus_property_get_bool, offsetof(ExecContext, no_new_privileges), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("SystemCallFilter", "(bas)", property_get_syscall_filter, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("SystemCallArchitectures", "as", property_get_syscall_archs, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("SystemCallErrorNumber", "i", property_get_syscall_errno, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Personality", "s", property_get_personality, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LockPersonality", "b", bus_property_get_bool, offsetof(ExecContext, lock_personality), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("RestrictAddressFamilies", "(bas)", property_get_address_families, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("RuntimeDirectoryPreserve", "s", property_get_exec_preserve_mode, offsetof(ExecContext, runtime_directory_preserve_mode), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("RuntimeDirectoryMode", "u", bus_property_get_mode, offsetof(ExecContext, directories[EXEC_DIRECTORY_RUNTIME].mode), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("RuntimeDirectory", "as", NULL, offsetof(ExecContext, directories[EXEC_DIRECTORY_RUNTIME].paths), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("StateDirectoryMode", "u", bus_property_get_mode, offsetof(ExecContext, directories[EXEC_DIRECTORY_STATE].mode), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("StateDirectory", "as", NULL, offsetof(ExecContext, directories[EXEC_DIRECTORY_STATE].paths), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("CacheDirectoryMode", "u", bus_property_get_mode, offsetof(ExecContext, directories[EXEC_DIRECTORY_CACHE].mode), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("CacheDirectory", "as", NULL, offsetof(ExecContext, directories[EXEC_DIRECTORY_CACHE].paths), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LogsDirectoryMode", "u", bus_property_get_mode, offsetof(ExecContext, directories[EXEC_DIRECTORY_LOGS].mode), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LogsDirectory", "as", NULL, offsetof(ExecContext, directories[EXEC_DIRECTORY_LOGS].paths), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("ConfigurationDirectoryMode", "u", bus_property_get_mode, offsetof(ExecContext, directories[EXEC_DIRECTORY_CONFIGURATION].mode), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("ConfigurationDirectory", "as", NULL, offsetof(ExecContext, directories[EXEC_DIRECTORY_CONFIGURATION].paths), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("MemoryDenyWriteExecute", "b", bus_property_get_bool, offsetof(ExecContext, memory_deny_write_execute), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("RestrictRealtime", "b", bus_property_get_bool, offsetof(ExecContext, restrict_realtime), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("RestrictNamespaces", "t", bus_property_get_ulong, offsetof(ExecContext, restrict_namespaces), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("BindPaths", "a(ssbt)", property_get_bind_paths, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("BindReadOnlyPaths", "a(ssbt)", property_get_bind_paths, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("MountAPIVFS", "b", bus_property_get_bool, offsetof(ExecContext, mount_apivfs), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("KeyringMode", "s", property_get_exec_keyring_mode, offsetof(ExecContext, keyring_mode), SD_BUS_VTABLE_PROPERTY_CONST),

        /* Obsolete/redundant properties: */
        SD_BUS_PROPERTY("Capabilities", "s", property_get_empty_string, 0, SD_BUS_VTABLE_PROPERTY_CONST|SD_BUS_VTABLE_HIDDEN),
        SD_BUS_PROPERTY("ReadWriteDirectories", "as", NULL, offsetof(ExecContext, read_write_paths), SD_BUS_VTABLE_PROPERTY_CONST|SD_BUS_VTABLE_HIDDEN),
        SD_BUS_PROPERTY("ReadOnlyDirectories", "as", NULL, offsetof(ExecContext, read_only_paths), SD_BUS_VTABLE_PROPERTY_CONST|SD_BUS_VTABLE_HIDDEN),
        SD_BUS_PROPERTY("InaccessibleDirectories", "as", NULL, offsetof(ExecContext, inaccessible_paths), SD_BUS_VTABLE_PROPERTY_CONST|SD_BUS_VTABLE_HIDDEN),
        SD_BUS_PROPERTY("IOScheduling", "i", property_get_ioprio, 0, SD_BUS_VTABLE_PROPERTY_CONST|SD_BUS_VTABLE_HIDDEN),

        SD_BUS_VTABLE_END
};

static int append_exec_command(sd_bus_message *reply, ExecCommand *c) {
        int r;

        assert(reply);
        assert(c);

        if (!c->path)
                return 0;

        r = sd_bus_message_open_container(reply, 'r', "sasbttttuii");
        if (r < 0)
                return r;

        r = sd_bus_message_append(reply, "s", c->path);
        if (r < 0)
                return r;

        r = sd_bus_message_append_strv(reply, c->argv);
        if (r < 0)
                return r;

        r = sd_bus_message_append(reply, "bttttuii",
                                  !!(c->flags & EXEC_COMMAND_IGNORE_FAILURE),
                                  c->exec_status.start_timestamp.realtime,
                                  c->exec_status.start_timestamp.monotonic,
                                  c->exec_status.exit_timestamp.realtime,
                                  c->exec_status.exit_timestamp.monotonic,
                                  (uint32_t) c->exec_status.pid,
                                  (int32_t) c->exec_status.code,
                                  (int32_t) c->exec_status.status);
        if (r < 0)
                return r;

        return sd_bus_message_close_container(reply);
}

int bus_property_get_exec_command(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *ret_error) {

        ExecCommand *c = (ExecCommand*) userdata;
        int r;

        assert(bus);
        assert(reply);

        r = sd_bus_message_open_container(reply, 'a', "(sasbttttuii)");
        if (r < 0)
                return r;

        r = append_exec_command(reply, c);
        if (r < 0)
                return r;

        return sd_bus_message_close_container(reply);
}

int bus_property_get_exec_command_list(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *ret_error) {

        ExecCommand *c = *(ExecCommand**) userdata;
        int r;

        assert(bus);
        assert(reply);

        r = sd_bus_message_open_container(reply, 'a', "(sasbttttuii)");
        if (r < 0)
                return r;

        LIST_FOREACH(command, c, c) {
                r = append_exec_command(reply, c);
                if (r < 0)
                        return r;
        }

        return sd_bus_message_close_container(reply);
}

int bus_exec_context_set_transient_property(
                Unit *u,
                ExecContext *c,
                const char *name,
                sd_bus_message *message,
                UnitSetPropertiesMode mode,
                sd_bus_error *error) {

        const char *soft = NULL;
        int r, ri;

        assert(u);
        assert(c);
        assert(name);
        assert(message);

        if (streq(name, "User")) {
                const char *uu;

                r = sd_bus_message_read(message, "s", &uu);
                if (r < 0)
                        return r;

                if (!isempty(uu) && !valid_user_group_name_or_id(uu))
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid user name: %s", uu);

                if (mode != UNIT_CHECK) {

                        if (isempty(uu))
                                c->user = mfree(c->user);
                        else if (free_and_strdup(&c->user, uu) < 0)
                                return -ENOMEM;

                        unit_write_drop_in_private_format(u, mode, name, "User=%s", uu);
                }

                return 1;

        } else if (streq(name, "Group")) {
                const char *gg;

                r = sd_bus_message_read(message, "s", &gg);
                if (r < 0)
                        return r;

                if (!isempty(gg) && !valid_user_group_name_or_id(gg))
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid group name: %s", gg);

                if (mode != UNIT_CHECK) {

                        if (isempty(gg))
                                c->group = mfree(c->group);
                        else if (free_and_strdup(&c->group, gg) < 0)
                                return -ENOMEM;

                        unit_write_drop_in_private_format(u, mode, name, "Group=%s", gg);
                }

                return 1;

        } else if (streq(name, "SupplementaryGroups")) {
                _cleanup_strv_free_ char **l = NULL;
                char **p;

                r = sd_bus_message_read_strv(message, &l);
                if (r < 0)
                        return r;

                STRV_FOREACH(p, l) {
                        if (!isempty(*p) && !valid_user_group_name_or_id(*p))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid supplementary group names");
                }

                if (mode != UNIT_CHECK) {
                        if (strv_length(l) == 0) {
                                c->supplementary_groups = strv_free(c->supplementary_groups);
                                unit_write_drop_in_private_format(u, mode, name, "%s=", name);
                        } else {
                                _cleanup_free_ char *joined = NULL;

                                r = strv_extend_strv(&c->supplementary_groups, l, true);
                                if (r < 0)
                                        return -ENOMEM;

                                joined = strv_join(c->supplementary_groups, " ");
                                if (!joined)
                                        return -ENOMEM;

                                unit_write_drop_in_private_format(u, mode, name, "%s=%s", name, joined);
                        }
                }

                return 1;

        } else if (streq(name, "SyslogIdentifier")) {
                const char *id;

                r = sd_bus_message_read(message, "s", &id);
                if (r < 0)
                        return r;

                if (mode != UNIT_CHECK) {

                        if (isempty(id))
                                c->syslog_identifier = mfree(c->syslog_identifier);
                        else if (free_and_strdup(&c->syslog_identifier, id) < 0)
                                return -ENOMEM;

                        unit_write_drop_in_private_format(u, mode, name, "SyslogIdentifier=%s", id);
                }

                return 1;
        } else if (streq(name, "SyslogLevel")) {
                int32_t level;

                r = sd_bus_message_read(message, "i", &level);
                if (r < 0)
                        return r;

                if (!log_level_is_valid(level))
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Log level value out of range");

                if (mode != UNIT_CHECK) {
                        c->syslog_priority = (c->syslog_priority & LOG_FACMASK) | level;
                        unit_write_drop_in_private_format(u, mode, name, "SyslogLevel=%i", level);
                }

                return 1;
        } else if (streq(name, "SyslogFacility")) {
                int32_t facility;

                r = sd_bus_message_read(message, "i", &facility);
                if (r < 0)
                        return r;

                if (!log_facility_unshifted_is_valid(facility))
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Log facility value out of range");

                if (mode != UNIT_CHECK) {
                        c->syslog_priority = (facility << 3) | LOG_PRI(c->syslog_priority);
                        unit_write_drop_in_private_format(u, mode, name, "SyslogFacility=%i", facility);
                }

                return 1;
        } else if (streq(name, "SecureBits")) {
                int n;

                r = sd_bus_message_read(message, "i", &n);
                if (r < 0)
                        return r;

                if (!secure_bits_is_valid(n))
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid secure bits");

                if (mode != UNIT_CHECK) {
                        _cleanup_free_ char *str = NULL;

                        c->secure_bits = n;
                        r = secure_bits_to_string_alloc(n, &str);
                        if (r < 0)
                                return r;

                        unit_write_drop_in_private_format(u, mode, name, "SecureBits=%s", str);
                }

                return 1;
        } else if (STR_IN_SET(name, "CapabilityBoundingSet", "AmbientCapabilities")) {
                uint64_t n;

                r = sd_bus_message_read(message, "t", &n);
                if (r < 0)
                        return r;

                if (mode != UNIT_CHECK) {
                        _cleanup_free_ char *str = NULL;

                        if (streq(name, "CapabilityBoundingSet"))
                                c->capability_bounding_set = n;
                        else /* "AmbientCapabilities" */
                                c->capability_ambient_set = n;

                        r = capability_set_to_string_alloc(n, &str);
                        if (r < 0)
                                return r;

                        unit_write_drop_in_private_format(u, mode, name, "%s=%s", name, str);
                }

                return 1;

        } else if (streq(name, "Personality")) {
                const char *s;
                unsigned long p;

                r = sd_bus_message_read(message, "s", &s);
                if (r < 0)
                        return r;

                p = personality_from_string(s);
                if (p == PERSONALITY_INVALID)
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid personality");

                if (mode != UNIT_CHECK) {
                        _cleanup_free_ char *str = NULL;

                        c->personality = p;
                        unit_write_drop_in_private_format(u, mode, name, "%s=%s", name, s);
                }

                return 1;

#if HAVE_SECCOMP

        } else if (streq(name, "SystemCallFilter")) {
                int whitelist;
                _cleanup_strv_free_ char **l = NULL;

                r = sd_bus_message_enter_container(message, 'r', "bas");
                if (r < 0)
                        return r;

                r = sd_bus_message_read(message, "b", &whitelist);
                if (r < 0)
                        return r;

                r = sd_bus_message_read_strv(message, &l);
                if (r < 0)
                        return r;

                r = sd_bus_message_exit_container(message);
                if (r < 0)
                        return r;

                if (mode != UNIT_CHECK) {
                        _cleanup_free_ char *joined = NULL;

                        if (strv_length(l) == 0) {
                                c->syscall_whitelist = false;
                                c->syscall_filter = set_free(c->syscall_filter);
                        } else {
                                char **s;

                                c->syscall_whitelist = whitelist;

                                r = set_ensure_allocated(&c->syscall_filter, NULL);
                                if (r < 0)
                                        return r;

                                STRV_FOREACH(s, l) {
                                        if (**s == '@') {
                                                const SyscallFilterSet *set;
                                                const char *i;

                                                set = syscall_filter_set_find(*s);
                                                if (!set)
                                                        return -EINVAL;

                                                NULSTR_FOREACH(i, set->value) {
                                                        int id;

                                                        id = seccomp_syscall_resolve_name(i);
                                                        if (id == __NR_SCMP_ERROR)
                                                                return -EINVAL;

                                                        r = set_put(c->syscall_filter, INT_TO_PTR(id + 1));
                                                        if (r < 0)
                                                                return r;
                                                }

                                        } else {
                                                int id;

                                                id = seccomp_syscall_resolve_name(*s);
                                                if (id == __NR_SCMP_ERROR)
                                                        return -EINVAL;

                                                r = set_put(c->syscall_filter, INT_TO_PTR(id + 1));
                                                if (r < 0)
                                                        return r;
                                        }
                                }
                        }

                        joined = strv_join(l, " ");
                        if (!joined)
                                return -ENOMEM;

                        unit_write_drop_in_private_format(u, mode, name, "SystemCallFilter=%s%s", whitelist ? "" : "~", joined);
                }

                return 1;

        } else if (streq(name, "SystemCallArchitectures")) {
                _cleanup_strv_free_ char **l = NULL;

                r = sd_bus_message_read_strv(message, &l);
                if (r < 0)
                        return r;

                if (mode != UNIT_CHECK) {
                        _cleanup_free_ char *joined = NULL;

                        if (strv_length(l) == 0)
                                c->syscall_archs = set_free(c->syscall_archs);
                        else {
                                char **s;

                                r = set_ensure_allocated(&c->syscall_archs, NULL);
                                if (r < 0)
                                        return r;

                                STRV_FOREACH(s, l) {
                                        uint32_t a;

                                        r = seccomp_arch_from_string(*s, &a);
                                        if (r < 0)
                                                return r;

                                        r = set_put(c->syscall_archs, UINT32_TO_PTR(a + 1));
                                        if (r < 0)
                                                return r;
                                }

                        }

                        joined = strv_join(l, " ");
                        if (!joined)
                                return -ENOMEM;

                        unit_write_drop_in_private_format(u, mode, name, "%s=%s", name, joined);
                }

                return 1;

        } else if (streq(name, "SystemCallErrorNumber")) {
                int32_t n;
                const char *str;

                r = sd_bus_message_read(message, "i", &n);
                if (r < 0)
                        return r;

                str = errno_to_name(n);
                if (!str)
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid SystemCallErrorNumber");

                if (mode != UNIT_CHECK) {
                        c->syscall_errno = n;

                        unit_write_drop_in_private_format(u, mode, name, "SystemCallErrorNumber=%s", str);
                }

                return 1;

        } else if (streq(name, "RestrictAddressFamilies")) {
                int whitelist;
                _cleanup_strv_free_ char **l = NULL;

                r = sd_bus_message_enter_container(message, 'r', "bas");
                if (r < 0)
                        return r;

                r = sd_bus_message_read(message, "b", &whitelist);
                if (r < 0)
                        return r;

                r = sd_bus_message_read_strv(message, &l);
                if (r < 0)
                        return r;

                r = sd_bus_message_exit_container(message);
                if (r < 0)
                        return r;

                if (mode != UNIT_CHECK) {
                        _cleanup_free_ char *joined = NULL;

                        if (strv_length(l) == 0) {
                                c->address_families_whitelist = false;
                                c->address_families = set_free(c->address_families);
                        } else {
                                char **s;

                                c->address_families_whitelist = whitelist;

                                r = set_ensure_allocated(&c->address_families, NULL);
                                if (r < 0)
                                        return r;

                                STRV_FOREACH(s, l) {
                                        int af;

                                        af = af_from_name(*s);
                                        if (af <= 0)
                                                return -EINVAL;

                                        r = set_put(c->address_families, INT_TO_PTR(af));
                                        if (r < 0)
                                                return r;
                                }
                        }

                        joined = strv_join(l, " ");
                        if (!joined)
                                return -ENOMEM;

                        unit_write_drop_in_private_format(u, mode, name, "RestrictAddressFamilies=%s%s", whitelist ? "" : "~", joined);
                }

                return 1;
#endif

        } else if (streq(name, "CPUSchedulingPolicy")) {
                int32_t n;

                r = sd_bus_message_read(message, "i", &n);
                if (r < 0)
                        return r;

                if (!sched_policy_is_valid(n))
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid CPU scheduling policy");

                if (mode != UNIT_CHECK) {
                        _cleanup_free_ char *str = NULL;

                        c->cpu_sched_policy = n;
                        r = sched_policy_to_string_alloc(n, &str);
                        if (r < 0)
                                return r;

                        unit_write_drop_in_private_format(u, mode, name, "CPUSchedulingPolicy=%s", str);
                }

                return 1;

        } else if (streq(name, "CPUSchedulingPriority")) {
                int32_t n;

                r = sd_bus_message_read(message, "i", &n);
                if (r < 0)
                        return r;

                if (!ioprio_priority_is_valid(n))
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid CPU scheduling priority");

                if (mode != UNIT_CHECK) {
                        c->cpu_sched_priority = n;
                        unit_write_drop_in_private_format(u, mode, name, "CPUSchedulingPriority=%i", n);
                }

                return 1;

        } else if (streq(name, "CPUAffinity")) {
                const void *a;
                size_t n = 0;

                r = sd_bus_message_read_array(message, 'y', &a, &n);
                if (r < 0)
                        return r;

                if (mode != UNIT_CHECK) {
                        if (n == 0) {
                                c->cpuset = mfree(c->cpuset);
                                unit_write_drop_in_private_format(u, mode, name, "%s=", name);
                        } else {
                                _cleanup_free_ char *str = NULL;
                                uint8_t *l;
                                size_t allocated = 0, len = 0, i;

                                c->cpuset = (cpu_set_t*) memdup(a, sizeof(cpu_set_t) * n);
                                if (c->cpuset)
                                        return -ENOMEM;

                                l = (uint8_t*) a;
                                for (i = 0; i < n; i++) {
                                        _cleanup_free_ char *p = NULL;
                                        size_t add;

                                        r = asprintf(&p, "%hhi", l[i]);
                                        if (r < 0)
                                                return -ENOMEM;

                                        add = strlen(p);

                                        if (GREEDY_REALLOC(str, allocated, len + add + 2))
                                                return -ENOMEM;

                                        strcpy(mempcpy(str + len, p, add), " ");
                                        len += add + 1;
                                }

                                if (len != 0)
                                        str[len - 1] = '\0';

                                unit_write_drop_in_private_format(u, mode, name, "%s=%s", name, str);
                        }
                }

                return 1;
        } else if (streq(name, "Nice")) {
                int32_t n;

                r = sd_bus_message_read(message, "i", &n);
                if (r < 0)
                        return r;

                if (!nice_is_valid(n))
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Nice value out of range");

                if (mode != UNIT_CHECK) {
                        c->nice = n;
                        unit_write_drop_in_private_format(u, mode, name, "Nice=%i", n);
                }

                return 1;

        } else if (streq(name, "IOSchedulingClass")) {
                int32_t q;

                r = sd_bus_message_read(message, "i", &q);
                if (r < 0)
                        return r;

                if (!ioprio_class_is_valid(q))
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid IO scheduling class: %i", q);

                if (mode != UNIT_CHECK) {
                        _cleanup_free_ char *s = NULL;

                        r = ioprio_class_to_string_alloc(q, &s);
                        if (r < 0)
                                return r;

                        c->ioprio = IOPRIO_PRIO_VALUE(q, IOPRIO_PRIO_DATA(c->ioprio));
                        c->ioprio_set = true;

                        unit_write_drop_in_private_format(u, mode, name, "IOSchedulingClass=%s", s);
                }

                return 1;

        } else if (streq(name, "IOSchedulingPriority")) {
                int32_t p;

                r = sd_bus_message_read(message, "i", &p);
                if (r < 0)
                        return r;

                if (!ioprio_priority_is_valid(p))
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid IO scheduling priority: %i", p);

                if (mode != UNIT_CHECK) {
                        c->ioprio = IOPRIO_PRIO_VALUE(IOPRIO_PRIO_CLASS(c->ioprio), p);
                        c->ioprio_set = true;

                        unit_write_drop_in_private_format(u, mode, name, "IOSchedulingPriority=%i", p);
                }

                return 1;

        } else if (STR_IN_SET(name, "TTYPath", "RootDirectory", "RootImage")) {
                const char *s;

                r = sd_bus_message_read(message, "s", &s);
                if (r < 0)
                        return r;

                if (!path_is_absolute(s))
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "%s takes an absolute path", name);

                if (mode != UNIT_CHECK) {
                        if (streq(name, "TTYPath"))
                                r = free_and_strdup(&c->tty_path, s);
                        else if (streq(name, "RootImage"))
                                r = free_and_strdup(&c->root_image, s);
                        else {
                                assert(streq(name, "RootDirectory"));
                                r = free_and_strdup(&c->root_directory, s);
                        }
                        if (r < 0)
                                return r;

                        unit_write_drop_in_private_format(u, mode, name, "%s=%s", name, s);
                }

                return 1;

        } else if (streq(name, "WorkingDirectory")) {
                const char *s;
                bool missing_ok;

                r = sd_bus_message_read(message, "s", &s);
                if (r < 0)
                        return r;

                if (s[0] == '-') {
                        missing_ok = true;
                        s++;
                } else
                        missing_ok = false;

                if (!streq(s, "~") && !path_is_absolute(s))
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "WorkingDirectory= expects an absolute path or '~'");

                if (mode != UNIT_CHECK) {
                        if (streq(s, "~")) {
                                c->working_directory = mfree(c->working_directory);
                                c->working_directory_home = true;
                        } else {
                                r = free_and_strdup(&c->working_directory, s);
                                if (r < 0)
                                        return r;

                                c->working_directory_home = false;
                        }

                        c->working_directory_missing_ok = missing_ok;
                        unit_write_drop_in_private_format(u, mode, name, "WorkingDirectory=%s%s", missing_ok ? "-" : "", s);
                }

                return 1;

        } else if (streq(name, "StandardInput")) {
                const char *s;
                ExecInput p;

                r = sd_bus_message_read(message, "s", &s);
                if (r < 0)
                        return r;

                p = exec_input_from_string(s);
                if (p < 0)
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid standard input name");

                if (mode != UNIT_CHECK) {
                        c->std_input = p;

                        unit_write_drop_in_private_format(u, mode, name, "StandardInput=%s", exec_input_to_string(p));
                }

                return 1;

        } else if (streq(name, "StandardOutput")) {
                const char *s;
                ExecOutput p;

                r = sd_bus_message_read(message, "s", &s);
                if (r < 0)
                        return r;

                p = exec_output_from_string(s);
                if (p < 0)
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid standard output name");

                if (mode != UNIT_CHECK) {
                        c->std_output = p;

                        unit_write_drop_in_private_format(u, mode, name, "StandardOutput=%s", exec_output_to_string(p));
                }

                return 1;

        } else if (streq(name, "StandardError")) {
                const char *s;
                ExecOutput p;

                r = sd_bus_message_read(message, "s", &s);
                if (r < 0)
                        return r;

                p = exec_output_from_string(s);
                if (p < 0)
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid standard error name");

                if (mode != UNIT_CHECK) {
                        c->std_error = p;

                        unit_write_drop_in_private_format(u, mode, name, "StandardError=%s", exec_output_to_string(p));
                }

                return 1;

        } else if (STR_IN_SET(name,
                              "StandardInputFileDescriptorName", "StandardOutputFileDescriptorName", "StandardErrorFileDescriptorName")) {
                const char *s;

                r = sd_bus_message_read(message, "s", &s);
                if (r < 0)
                        return r;

                if (!fdname_is_valid(s))
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid file descriptor name");

                if (mode != UNIT_CHECK) {
                        if (streq(name, "StandardInputFileDescriptorName")) {
                                c->std_input = EXEC_INPUT_NAMED_FD;
                                r = free_and_strdup(&c->stdio_fdname[STDIN_FILENO], s);
                                if (r < 0)
                                        return r;
                                unit_write_drop_in_private_format(u, mode, name, "StandardInput=fd:%s", s);
                        } else if (streq(name, "StandardOutputFileDescriptorName")) {
                                c->std_output = EXEC_OUTPUT_NAMED_FD;
                                r = free_and_strdup(&c->stdio_fdname[STDOUT_FILENO], s);
                                if (r < 0)
                                        return r;
                                unit_write_drop_in_private_format(u, mode, name, "StandardOutput=fd:%s", s);
                        } else if (streq(name, "StandardErrorFileDescriptorName")) {
                                c->std_error = EXEC_OUTPUT_NAMED_FD;
                                r = free_and_strdup(&c->stdio_fdname[STDERR_FILENO], s);
                                if (r < 0)
                                        return r;
                                unit_write_drop_in_private_format(u, mode, name, "StandardError=fd:%s", s);
                        }
                }

                return 1;

        } else if (STR_IN_SET(name,
                              "IgnoreSIGPIPE", "TTYVHangup", "TTYReset", "TTYVTDisallocate",
                              "PrivateTmp", "PrivateDevices", "PrivateNetwork", "PrivateUsers",
                              "NoNewPrivileges", "SyslogLevelPrefix", "MemoryDenyWriteExecute",
                              "RestrictRealtime", "DynamicUser", "RemoveIPC", "ProtectKernelTunables",
                              "ProtectKernelModules", "ProtectControlGroups", "MountAPIVFS",
                              "CPUSchedulingResetOnFork", "NonBlocking", "LockPersonality")) {
                int b;

                r = sd_bus_message_read(message, "b", &b);
                if (r < 0)
                        return r;

                if (mode != UNIT_CHECK) {
                        if (streq(name, "IgnoreSIGPIPE"))
                                c->ignore_sigpipe = b;
                        else if (streq(name, "TTYVHangup"))
                                c->tty_vhangup = b;
                        else if (streq(name, "TTYReset"))
                                c->tty_reset = b;
                        else if (streq(name, "TTYVTDisallocate"))
                                c->tty_vt_disallocate = b;
                        else if (streq(name, "PrivateTmp"))
                                c->private_tmp = b;
                        else if (streq(name, "PrivateDevices"))
                                c->private_devices = b;
                        else if (streq(name, "PrivateNetwork"))
                                c->private_network = b;
                        else if (streq(name, "PrivateUsers"))
                                c->private_users = b;
                        else if (streq(name, "NoNewPrivileges"))
                                c->no_new_privileges = b;
                        else if (streq(name, "SyslogLevelPrefix"))
                                c->syslog_level_prefix = b;
                        else if (streq(name, "MemoryDenyWriteExecute"))
                                c->memory_deny_write_execute = b;
                        else if (streq(name, "RestrictRealtime"))
                                c->restrict_realtime = b;
                        else if (streq(name, "DynamicUser"))
                                c->dynamic_user = b;
                        else if (streq(name, "RemoveIPC"))
                                c->remove_ipc = b;
                        else if (streq(name, "ProtectKernelTunables"))
                                c->protect_kernel_tunables = b;
                        else if (streq(name, "ProtectKernelModules"))
                                c->protect_kernel_modules = b;
                        else if (streq(name, "ProtectControlGroups"))
                                c->protect_control_groups = b;
                        else if (streq(name, "MountAPIVFS"))
                                c->mount_apivfs = b;
                        else if (streq(name, "CPUSchedulingResetOnFork"))
                                c->cpu_sched_reset_on_fork = b;
                        else if (streq(name, "NonBlocking"))
                                c->non_blocking = b;
                        else if (streq(name, "LockPersonality"))
                                c->lock_personality = b;

                        unit_write_drop_in_private_format(u, mode, name, "%s=%s", name, yes_no(b));
                }

                return 1;

        } else if (streq(name, "UtmpIdentifier")) {
                const char *id;

                r = sd_bus_message_read(message, "s", &id);
                if (r < 0)
                        return r;

                if (mode != UNIT_CHECK) {
                        if (isempty(id))
                                c->utmp_id = mfree(c->utmp_id);
                        else if (free_and_strdup(&c->utmp_id, id) < 0)
                                return -ENOMEM;

                        unit_write_drop_in_private_format(u, mode, name, "UtmpIdentifier=%s", strempty(id));
                }

                return 1;

        } else if (streq(name, "UtmpMode")) {
                const char *s;
                ExecUtmpMode m;

                r = sd_bus_message_read(message, "s", &s);
                if (r < 0)
                        return r;

                m = exec_utmp_mode_from_string(s);
                if (m < 0)
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid utmp mode");

                if (mode != UNIT_CHECK) {
                        c->utmp_mode = m;

                        unit_write_drop_in_private_format(u, mode, name, "UtmpMode=%s", exec_utmp_mode_to_string(m));
                }

                return 1;

        } else if (streq(name, "PAMName")) {
                const char *n;

                r = sd_bus_message_read(message, "s", &n);
                if (r < 0)
                        return r;

                if (mode != UNIT_CHECK) {
                        if (isempty(n))
                                c->pam_name = mfree(c->pam_name);
                        else if (free_and_strdup(&c->pam_name, n) < 0)
                                return -ENOMEM;

                        unit_write_drop_in_private_format(u, mode, name, "PAMName=%s", strempty(n));
                }

                return 1;

        } else if (streq(name, "Environment")) {

                _cleanup_strv_free_ char **l = NULL, **q = NULL;

                r = sd_bus_message_read_strv(message, &l);
                if (r < 0)
                        return r;

                r = unit_full_printf_strv(u, l, &q);
                if (r < 0)
                        return r;

                if (!strv_env_is_valid(q))
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid environment block.");

                if (mode != UNIT_CHECK) {
                        if (strv_length(q) == 0) {
                                c->environment = strv_free(c->environment);
                                unit_write_drop_in_private_format(u, mode, name, "Environment=");
                        } else {
                                _cleanup_free_ char *joined = NULL;
                                char **e;

                                e = strv_env_merge(2, c->environment, q);
                                if (!e)
                                        return -ENOMEM;

                                strv_free(c->environment);
                                c->environment = e;

                                /* We write just the new settings out to file, with unresolved specifiers */
                                joined = strv_join_quoted(l);
                                if (!joined)
                                        return -ENOMEM;

                                unit_write_drop_in_private_format(u, mode, name, "Environment=%s", joined);
                        }
                }

                return 1;

        } else if (streq(name, "UnsetEnvironment")) {

                _cleanup_strv_free_ char **l = NULL, **q = NULL;

                r = sd_bus_message_read_strv(message, &l);
                if (r < 0)
                        return r;

                r = unit_full_printf_strv(u, l, &q);
                if (r < 0)
                        return r;

                if (!strv_env_name_or_assignment_is_valid(q))
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid UnsetEnvironment= list.");

                if (mode != UNIT_CHECK) {
                        if (strv_length(q) == 0) {
                                c->unset_environment = strv_free(c->unset_environment);
                                unit_write_drop_in_private_format(u, mode, name, "UnsetEnvironment=");
                        } else {
                                _cleanup_free_ char *joined = NULL;
                                char **e;

                                e = strv_env_merge(2, c->unset_environment, q);
                                if (!e)
                                        return -ENOMEM;

                                strv_free(c->unset_environment);
                                c->unset_environment = e;

                                /* We write just the new settings out to file, with unresolved specifiers */
                                joined = strv_join_quoted(l);
                                if (!joined)
                                        return -ENOMEM;

                                unit_write_drop_in_private_format(u, mode, name, "UnsetEnvironment=%s", joined);
                        }
                }

                return 1;

        } else if (streq(name, "TimerSlackNSec")) {

                nsec_t n;

                r = sd_bus_message_read(message, "t", &n);
                if (r < 0)
                        return r;

                if (mode != UNIT_CHECK) {
                        c->timer_slack_nsec = n;
                        unit_write_drop_in_private_format(u, mode, name, "TimerSlackNSec=" NSEC_FMT, n);
                }

                return 1;

        } else if (streq(name, "OOMScoreAdjust")) {
                int oa;

                r = sd_bus_message_read(message, "i", &oa);
                if (r < 0)
                        return r;

                if (!oom_score_adjust_is_valid(oa))
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "OOM score adjust value out of range");

                if (mode != UNIT_CHECK) {
                        c->oom_score_adjust = oa;
                        c->oom_score_adjust_set = true;
                        unit_write_drop_in_private_format(u, mode, name, "OOMScoreAdjust=%i", oa);
                }

                return 1;

        } else if (streq(name, "EnvironmentFiles")) {

                _cleanup_free_ char *joined = NULL;
                _cleanup_fclose_ FILE *f = NULL;
                _cleanup_strv_free_ char **l = NULL;
                size_t size = 0;
                char **i;

                r = sd_bus_message_enter_container(message, 'a', "(sb)");
                if (r < 0)
                        return r;

                f = open_memstream(&joined, &size);
                if (!f)
                        return -ENOMEM;

                STRV_FOREACH(i, c->environment_files)
                        fprintf(f, "EnvironmentFile=%s", *i);

                while ((r = sd_bus_message_enter_container(message, 'r', "sb")) > 0) {
                        const char *path;
                        int b;

                        r = sd_bus_message_read(message, "sb", &path, &b);
                        if (r < 0)
                                return r;

                        r = sd_bus_message_exit_container(message);
                        if (r < 0)
                                return r;

                        if (!path_is_absolute(path))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Path %s is not absolute.", path);

                        if (mode != UNIT_CHECK) {
                                char *buf;

                                buf = strjoin(b ? "-" : "", path);
                                if (!buf)
                                        return -ENOMEM;

                                fprintf(f, "EnvironmentFile=%s", buf);

                                r = strv_consume(&l, buf);
                                if (r < 0)
                                        return r;
                        }
                }
                if (r < 0)
                        return r;

                r = sd_bus_message_exit_container(message);
                if (r < 0)
                        return r;

                r = fflush_and_check(f);
                if (r < 0)
                        return r;

                if (mode != UNIT_CHECK) {
                        if (strv_isempty(l)) {
                                c->environment_files = strv_free(c->environment_files);
                                unit_write_drop_in_private(u, mode, name, "EnvironmentFile=");
                        } else {
                                r = strv_extend_strv(&c->environment_files, l, true);
                                if (r < 0)
                                        return r;

                                unit_write_drop_in_private(u, mode, name, joined);
                        }
                }

                return 1;

        } else if (streq(name, "PassEnvironment")) {

                _cleanup_strv_free_ char **l = NULL, **q = NULL;

                r = sd_bus_message_read_strv(message, &l);
                if (r < 0)
                        return r;

                r = unit_full_printf_strv(u, l, &q);
                if (r < 0)
                        return r;

                if (!strv_env_name_is_valid(q))
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid PassEnvironment= block.");

                if (mode != UNIT_CHECK) {
                        if (strv_isempty(l)) {
                                c->pass_environment = strv_free(c->pass_environment);
                                unit_write_drop_in_private_format(u, mode, name, "PassEnvironment=");
                        } else {
                                _cleanup_free_ char *joined = NULL;

                                r = strv_extend_strv(&c->pass_environment, q, true);
                                if (r < 0)
                                        return r;

                                /* We write just the new settings out to file, with unresolved specifiers. */
                                joined = strv_join_quoted(l);
                                if (!joined)
                                        return -ENOMEM;

                                unit_write_drop_in_private_format(u, mode, name, "PassEnvironment=%s", joined);
                        }
                }

                return 1;

        } else if (STR_IN_SET(name, "ReadWriteDirectories", "ReadOnlyDirectories", "InaccessibleDirectories",
                              "ReadWritePaths", "ReadOnlyPaths", "InaccessiblePaths")) {
                _cleanup_strv_free_ char **l = NULL;
                char ***dirs;
                char **p;

                r = sd_bus_message_read_strv(message, &l);
                if (r < 0)
                        return r;

                STRV_FOREACH(p, l) {
                        const char *i = *p;
                        size_t offset;

                        if (!utf8_is_valid(i))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid %s", name);

                        offset = i[0] == '-';
                        offset += i[offset] == '+';
                        if (!path_is_absolute(i + offset))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid %s", name);
                }

                if (mode != UNIT_CHECK) {
                        _cleanup_free_ char *joined = NULL;

                        if (STR_IN_SET(name, "ReadWriteDirectories", "ReadWritePaths"))
                                dirs = &c->read_write_paths;
                        else if (STR_IN_SET(name, "ReadOnlyDirectories", "ReadOnlyPaths"))
                                dirs = &c->read_only_paths;
                        else /* "InaccessiblePaths" */
                                dirs = &c->inaccessible_paths;

                        if (strv_length(l) == 0) {
                                *dirs = strv_free(*dirs);
                                unit_write_drop_in_private_format(u, mode, name, "%s=", name);
                        } else {
                                r = strv_extend_strv(dirs, l, true);
                                if (r < 0)
                                        return -ENOMEM;

                                joined = strv_join_quoted(*dirs);
                                if (!joined)
                                        return -ENOMEM;

                                unit_write_drop_in_private_format(u, mode, name, "%s=%s", name, joined);
                        }

                }

                return 1;

        } else if (streq(name, "ProtectSystem")) {
                const char *s;
                ProtectSystem ps;

                r = sd_bus_message_read(message, "s", &s);
                if (r < 0)
                        return r;

                r = parse_boolean(s);
                if (r > 0)
                        ps = PROTECT_SYSTEM_YES;
                else if (r == 0)
                        ps = PROTECT_SYSTEM_NO;
                else {
                        ps = protect_system_from_string(s);
                        if (ps < 0)
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Failed to parse protect system value");
                }

                if (mode != UNIT_CHECK) {
                        c->protect_system = ps;
                        unit_write_drop_in_private_format(u, mode, name, "%s=%s", name, s);
                }

                return 1;

        } else if (streq(name, "ProtectHome")) {
                const char *s;
                ProtectHome ph;

                r = sd_bus_message_read(message, "s", &s);
                if (r < 0)
                        return r;

                r = parse_boolean(s);
                if (r > 0)
                        ph = PROTECT_HOME_YES;
                else if (r == 0)
                        ph = PROTECT_HOME_NO;
                else {
                        ph = protect_home_from_string(s);
                        if (ph < 0)
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Failed to parse protect home value");
                }

                if (mode != UNIT_CHECK) {
                        c->protect_home = ph;
                        unit_write_drop_in_private_format(u, mode, name, "%s=%s", name, s);
                }

                return 1;

        } else if (streq(name, "KeyringMode")) {

                const char *s;
                ExecKeyringMode m;

                r = sd_bus_message_read(message, "s", &s);
                if (r < 0)
                        return r;

                m = exec_keyring_mode_from_string(s);
                if (m < 0)
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid keyring mode");

                if (mode != UNIT_CHECK) {
                        c->keyring_mode = m;

                        unit_write_drop_in_private_format(u, mode, name, "KeyringMode=%s", exec_keyring_mode_to_string(m));
                }

                return 1;

        } else if (streq(name, "RuntimeDirectoryPreserve")) {
                const char *s;
                ExecPreserveMode m;

                r = sd_bus_message_read(message, "s", &s);
                if (r < 0)
                        return r;

                m = exec_preserve_mode_from_string(s);
                if (m < 0)
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid preserve mode");

                if (mode != UNIT_CHECK) {
                        c->runtime_directory_preserve_mode = m;

                        unit_write_drop_in_private_format(u, mode, name, "RuntimeDirectoryPreserve=%s", exec_preserve_mode_to_string(m));
                }

                return 1;

        } else if (STR_IN_SET(name, "RuntimeDirectoryMode", "StateDirectoryMode", "CacheDirectoryMode", "LogsDirectoryMode", "ConfigurationDirectoryMode", "UMask")) {
                mode_t m;

                r = sd_bus_message_read(message, "u", &m);
                if (r < 0)
                        return r;

                if (mode != UNIT_CHECK) {
                        ExecDirectoryType i;

                        if (streq(name, "UMask"))
                                c->umask = m;
                        else
                                for (i = 0; i < _EXEC_DIRECTORY_TYPE_MAX; i++)
                                        if (startswith(name, exec_directory_type_to_string(i))) {
                                                c->directories[i].mode = m;
                                                break;
                                        }

                        unit_write_drop_in_private_format(u, mode, name, "%s=%040o", name, m);
                }

                return 1;

        } else if (STR_IN_SET(name, "RuntimeDirectory", "StateDirectory", "CacheDirectory", "LogsDirectory", "ConfigurationDirectory")) {
                _cleanup_strv_free_ char **l = NULL;
                char **p;

                r = sd_bus_message_read_strv(message, &l);
                if (r < 0)
                        return r;

                STRV_FOREACH(p, l) {
                        if (!path_is_safe(*p) || path_is_absolute(*p))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "%s= path is not valid: %s", name, *p);
                }

                if (mode != UNIT_CHECK) {
                        _cleanup_free_ char *joined = NULL;
                        char ***dirs = NULL;
                        ExecDirectoryType i;

                        for (i = 0; i < _EXEC_DIRECTORY_TYPE_MAX; i++)
                                if (streq(name, exec_directory_type_to_string(i))) {
                                        dirs = &c->directories[i].paths;
                                        break;
                                }

                        assert(dirs);

                        if (strv_isempty(l)) {
                                *dirs = strv_free(*dirs);
                                unit_write_drop_in_private_format(u, mode, name, "%s=", name);
                        } else {
                                r = strv_extend_strv(dirs, l, true);
                                if (r < 0)
                                        return -ENOMEM;

                                joined = strv_join_quoted(*dirs);
                                if (!joined)
                                        return -ENOMEM;

                                unit_write_drop_in_private_format(u, mode, name, "%s=%s", name, joined);
                        }
                }

                return 1;

        } else if (streq(name, "SELinuxContext")) {
                const char *s;
                r = sd_bus_message_read(message, "s", &s);
                if (r < 0)
                        return r;

                if (mode != UNIT_CHECK) {
                        if (isempty(s))
                                c->selinux_context = mfree(c->selinux_context);
                        else if (free_and_strdup(&c->selinux_context, s) < 0)
                                return -ENOMEM;

                        unit_write_drop_in_private_format(u, mode, name, "%s=%s", name, strempty(s));
                }

                return 1;

        } else if (STR_IN_SET(name, "AppArmorProfile", "SmackProcessLabel")) {
                int ignore;
                const char *s;

                r = sd_bus_message_enter_container(message, 'r', "bs");
                if (r < 0)
                        return r;

                r = sd_bus_message_read(message, "bs", &ignore, &s);
                if (r < 0)
                        return r;

                if (mode != UNIT_CHECK) {
                        char **p;
                        bool *b;

                        if (streq(name, "AppArmorProfile")) {
                                p = &c->apparmor_profile;
                                b = &c->apparmor_profile_ignore;
                        } else { /* "SmackProcessLabel" */
                                p = &c->smack_process_label;
                                b = &c->smack_process_label_ignore;
                        }

                        if (isempty(s)) {
                                *p = mfree(*p);
                                *b = false;
                        } else {
                                if (free_and_strdup(p, s) < 0)
                                        return -ENOMEM;
                                *b = ignore;
                        }

                        unit_write_drop_in_private_format(u, mode, name, "%s=%s%s", name, ignore ? "-" : "", strempty(s));
                }

                return 1;

        } else if (streq(name, "RestrictNamespaces")) {
                uint64_t flags;

                r = sd_bus_message_read(message, "t", &flags);
                if (r < 0)
                        return r;
                if ((flags & NAMESPACE_FLAGS_ALL) != flags)
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Unknown namespace types");

                if (mode != UNIT_CHECK) {
                        _cleanup_free_ char *s = NULL;

                        r = namespace_flag_to_string_many(flags, &s);
                        if (r < 0)
                                return r;

                        c->restrict_namespaces = flags;
                        unit_write_drop_in_private_format(u, mode, name, "%s=%s", name, s);
                }

                return 1;
        } else if (streq(name, "MountFlags")) {
                uint64_t flags;

                r = sd_bus_message_read(message, "t", &flags);
                if (r < 0)
                        return r;
                if (!IN_SET(flags, 0, MS_SHARED, MS_PRIVATE, MS_SLAVE))
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Unknown mount propagation flags");

                if (mode != UNIT_CHECK) {
                        c->mount_flags = flags;

                        unit_write_drop_in_private_format(u, mode, name, "%s=%s", name, mount_propagation_flags_to_string(flags));
                }

                return 1;
        } else if (STR_IN_SET(name, "BindPaths", "BindReadOnlyPaths")) {
                unsigned empty = true;

                r = sd_bus_message_enter_container(message, 'a', "(ssbt)");
                if (r < 0)
                        return r;

                while ((r = sd_bus_message_enter_container(message, 'r', "ssbt")) > 0) {
                        const char *source, *destination;
                        int ignore_enoent;
                        uint64_t mount_flags;

                        r = sd_bus_message_read(message, "ssbt", &source, &destination, &ignore_enoent, &mount_flags);
                        if (r < 0)
                                return r;

                        r = sd_bus_message_exit_container(message);
                        if (r < 0)
                                return r;

                        if (!path_is_absolute(source))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Source path %s is not absolute.", source);
                        if (!path_is_absolute(destination))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Destination path %s is not absolute.", destination);
                        if (!IN_SET(mount_flags, 0, MS_REC))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Unknown mount flags.");

                        if (mode != UNIT_CHECK) {
                                r = bind_mount_add(&c->bind_mounts, &c->n_bind_mounts,
                                                   &(BindMount) {
                                                           .source = strdup(source),
                                                           .destination = strdup(destination),
                                                           .read_only = !!strstr(name, "ReadOnly"),
                                                           .recursive = !!(mount_flags & MS_REC),
                                                           .ignore_enoent = ignore_enoent,
                                                   });
                                if (r < 0)
                                        return r;

                                unit_write_drop_in_private_format(
                                                u, mode, name,
                                                "%s=%s%s:%s:%s",
                                                name,
                                                ignore_enoent ? "-" : "",
                                                source,
                                                destination,
                                                (mount_flags & MS_REC) ? "rbind" : "norbind");
                        }

                        empty = false;
                }
                if (r < 0)
                        return r;

                r = sd_bus_message_exit_container(message);
                if (r < 0)
                        return r;

                if (empty) {
                        bind_mount_free_many(c->bind_mounts, c->n_bind_mounts);
                        c->bind_mounts = NULL;
                        c->n_bind_mounts = 0;
                }

                return 1;
        }

        ri = rlimit_from_string(name);
        if (ri < 0) {
                soft = endswith(name, "Soft");
                if (soft) {
                        const char *n;

                        n = strndupa(name, soft - name);
                        ri = rlimit_from_string(n);
                        if (ri >= 0)
                                name = n;

                }
        }

        if (ri >= 0) {
                uint64_t rl;
                rlim_t x;

                r = sd_bus_message_read(message, "t", &rl);
                if (r < 0)
                        return r;

                if (rl == (uint64_t) -1)
                        x = RLIM_INFINITY;
                else {
                        x = (rlim_t) rl;

                        if ((uint64_t) x != rl)
                                return -ERANGE;
                }

                if (mode != UNIT_CHECK) {
                        _cleanup_free_ char *f = NULL;
                        struct rlimit nl;

                        if (c->rlimit[ri]) {
                                nl = *c->rlimit[ri];

                                if (soft)
                                        nl.rlim_cur = x;
                                else
                                        nl.rlim_max = x;
                        } else
                                /* When the resource limit is not initialized yet, then assign the value to both fields */
                                nl = (struct rlimit) {
                                        .rlim_cur = x,
                                        .rlim_max = x,
                                };

                        r = rlimit_format(&nl, &f);
                        if (r < 0)
                                return r;

                        if (c->rlimit[ri])
                                *c->rlimit[ri] = nl;
                        else {
                                c->rlimit[ri] = newdup(struct rlimit, &nl, 1);
                                if (!c->rlimit[ri])
                                        return -ENOMEM;
                        }

                        unit_write_drop_in_private_format(u, mode, name, "%s=%s", name, f);
                }

                return 1;
        }

        return 0;
}
