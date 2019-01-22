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

#ifdef HAVE_SECCOMP
#include <seccomp.h>
#endif

#include "af-list.h"
#include "alloc-util.h"
#include "bus-util.h"
#include "capability-util.h"
#include "dbus-execute.h"
#include "env-util.h"
#include "execute.h"
#include "fd-util.h"
#include "fileio.h"
#include "ioprio.h"
#include "missing.h"
#include "namespace.h"
#include "parse-util.h"
#include "path-util.h"
#include "process-util.h"
#include "rlimit-util.h"
#ifdef HAVE_SECCOMP
#include "seccomp-util.h"
#endif
#include "strv.h"
#include "syslog-util.h"
#include "user-util.h"
#include "utf8.h"

BUS_DEFINE_PROPERTY_GET_ENUM(bus_property_get_exec_output, exec_output, ExecOutput);

static BUS_DEFINE_PROPERTY_GET_ENUM(property_get_exec_input, exec_input, ExecInput);

static BUS_DEFINE_PROPERTY_GET_ENUM(property_get_exec_utmp_mode, exec_utmp_mode, ExecUtmpMode);

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
        int32_t n;

        assert(bus);
        assert(reply);
        assert(c);

        if (c->ioprio_set)
                n = c->ioprio;
        else {
                n = ioprio_get(IOPRIO_WHO_PROCESS, 0);
                if (n < 0)
                        n = IOPRIO_PRIO_VALUE(IOPRIO_CLASS_BE, 4);
        }

        return sd_bus_message_append(reply, "i", n);
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

#ifdef HAVE_SECCOMP
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

#ifdef HAVE_SECCOMP
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

#ifdef HAVE_SECCOMP
        Iterator i;
        void *id;
#endif

        assert(bus);
        assert(reply);
        assert(c);

#ifdef HAVE_SECCOMP
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

const sd_bus_vtable bus_exec_vtable[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_PROPERTY("Environment", "as", NULL, offsetof(ExecContext, environment), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("EnvironmentFiles", "a(sb)", property_get_environment_files, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("PassEnvironment", "as", NULL, offsetof(ExecContext, pass_environment), SD_BUS_VTABLE_PROPERTY_CONST),
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
        SD_BUS_PROPERTY("OOMScoreAdjust", "i", property_get_oom_score_adjust, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Nice", "i", property_get_nice, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("IOScheduling", "i", property_get_ioprio, 0, SD_BUS_VTABLE_PROPERTY_CONST),
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
        SD_BUS_PROPERTY("Capabilities", "s", property_get_empty_string, 0, SD_BUS_VTABLE_PROPERTY_CONST|SD_BUS_VTABLE_HIDDEN),
        SD_BUS_PROPERTY("SecureBits", "i", bus_property_get_int, offsetof(ExecContext, secure_bits), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("CapabilityBoundingSet", "t", property_get_capability_bounding_set, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("AmbientCapabilities", "t", property_get_ambient_capabilities, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("User", "s", NULL, offsetof(ExecContext, user), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Group", "s", NULL, offsetof(ExecContext, group), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("DynamicUser", "b", bus_property_get_bool, offsetof(ExecContext, dynamic_user), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("RemoveIPC", "b", bus_property_get_bool, offsetof(ExecContext, remove_ipc), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("SupplementaryGroups", "as", NULL, offsetof(ExecContext, supplementary_groups), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("PAMName", "s", NULL, offsetof(ExecContext, pam_name), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("ReadWriteDirectories", "as", NULL, offsetof(ExecContext, read_write_paths), SD_BUS_VTABLE_PROPERTY_CONST|SD_BUS_VTABLE_HIDDEN),
        SD_BUS_PROPERTY("ReadOnlyDirectories", "as", NULL, offsetof(ExecContext, read_only_paths), SD_BUS_VTABLE_PROPERTY_CONST|SD_BUS_VTABLE_HIDDEN),
        SD_BUS_PROPERTY("InaccessibleDirectories", "as", NULL, offsetof(ExecContext, inaccessible_paths), SD_BUS_VTABLE_PROPERTY_CONST|SD_BUS_VTABLE_HIDDEN),
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
        SD_BUS_PROPERTY("RestrictAddressFamilies", "(bas)", property_get_address_families, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("RuntimeDirectoryMode", "u", bus_property_get_mode, offsetof(ExecContext, runtime_directory_mode), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("RuntimeDirectory", "as", NULL, offsetof(ExecContext, runtime_directory), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("MemoryDenyWriteExecute", "b", bus_property_get_bool, offsetof(ExecContext, memory_deny_write_execute), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("RestrictRealtime", "b", bus_property_get_bool, offsetof(ExecContext, restrict_realtime), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("RestrictNamespace", "t", bus_property_get_ulong, offsetof(ExecContext, restrict_namespaces), SD_BUS_VTABLE_PROPERTY_CONST),
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
                                  c->ignore,
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
                int level;

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
                int facility;

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
        } else if (streq(name, "Nice")) {
                int n;

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

        } else if (STR_IN_SET(name, "TTYPath", "RootDirectory")) {
                const char *s;

                r = sd_bus_message_read(message, "s", &s);
                if (r < 0)
                        return r;

                if (!path_is_absolute(s))
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "%s takes an absolute path", name);

                if (mode != UNIT_CHECK) {
                        if (streq(name, "TTYPath"))
                                r = free_and_strdup(&c->tty_path, s);
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
                              "IgnoreSIGPIPE", "TTYVHangup", "TTYReset",
                              "PrivateTmp", "PrivateDevices", "PrivateNetwork", "PrivateUsers",
                              "NoNewPrivileges", "SyslogLevelPrefix", "MemoryDenyWriteExecute",
                              "RestrictRealtime", "DynamicUser", "RemoveIPC", "ProtectKernelTunables",
                              "ProtectKernelModules", "ProtectControlGroups")) {
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

                _cleanup_strv_free_ char **l = NULL;

                r = sd_bus_message_read_strv(message, &l);
                if (r < 0)
                        return r;

                if (!strv_env_is_valid(l))
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid environment block.");

                if (mode != UNIT_CHECK) {
                        _cleanup_free_ char *joined = NULL;
                        char **e;

                        if (strv_length(l) == 0) {
                                c->environment = strv_free(c->environment);
                                unit_write_drop_in_private_format(u, mode, name, "Environment=");
                        } else {
                                e = strv_env_merge(2, c->environment, l);
                                if (!e)
                                        return -ENOMEM;

                                strv_free(c->environment);
                                c->environment = e;

                                joined = strv_join_quoted(c->environment);
                                if (!joined)
                                        return -ENOMEM;

                                unit_write_drop_in_private_format(u, mode, name, "Environment=%s", joined);
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
                _cleanup_free_ char **l = NULL;
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

                        if (!isempty(path) && !path_is_absolute(path))
                                return sd_bus_error_set_errnof(error, EINVAL, "Path %s is not absolute.", path);

                        if (mode != UNIT_CHECK) {
                                char *buf = NULL;

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

                _cleanup_strv_free_ char **l = NULL;

                r = sd_bus_message_read_strv(message, &l);
                if (r < 0)
                        return r;

                if (!strv_env_name_is_valid(l))
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid PassEnvironment block.");

                if (mode != UNIT_CHECK) {
                        if (strv_isempty(l)) {
                                c->pass_environment = strv_free(c->pass_environment);
                                unit_write_drop_in_private_format(u, mode, name, "PassEnvironment=");
                        } else {
                                _cleanup_free_ char *joined = NULL;

                                r = strv_extend_strv(&c->pass_environment, l, true);
                                if (r < 0)
                                        return r;

                                joined = strv_join_quoted(c->pass_environment);
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
                        int offset;
                        if (!utf8_is_valid(*p))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid %s", name);

                        offset = **p == '-';
                        if (!path_is_absolute(*p + offset))
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

        } else if (streq(name, "RuntimeDirectory")) {
                _cleanup_strv_free_ char **l = NULL;
                char **p;

                r = sd_bus_message_read_strv(message, &l);
                if (r < 0)
                        return r;

                STRV_FOREACH(p, l) {
                        if (!filename_is_valid(*p))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Runtime directory is not valid %s", *p);
                }

                if (mode != UNIT_CHECK) {
                        _cleanup_free_ char *joined = NULL;

                        if (strv_isempty(l)) {
                                c->runtime_directory = strv_free(c->runtime_directory);
                                unit_write_drop_in_private_format(u, mode, name, "%s=", name);
                        } else {
                                r = strv_extend_strv(&c->runtime_directory, l, true);

                                if (r < 0)
                                        return -ENOMEM;

                                joined = strv_join_quoted(c->runtime_directory);
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
