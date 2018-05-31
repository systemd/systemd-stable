/* SPDX-License-Identifier: LGPL-2.1+ */
/***
  This file is part of systemd.

  Copyright 2010 Lennart Poettering
  Copyright 2013 Marc-Antoine Perennou

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
#include <fcntl.h>
#include <getopt.h>
#include <locale.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <unistd.h>

#include "sd-bus.h"
#include "sd-daemon.h"
#include "sd-login.h"

#include "alloc-util.h"
#include "bootspec.h"
#include "bus-common-errors.h"
#include "bus-error.h"
#include "bus-message.h"
#include "bus-unit-util.h"
#include "bus-util.h"
#include "cgroup-show.h"
#include "cgroup-util.h"
#include "copy.h"
#include "dropin.h"
#include "efivars.h"
#include "env-util.h"
#include "escape.h"
#include "exit-status.h"
#include "fd-util.h"
#include "fileio.h"
#include "format-util.h"
#include "fs-util.h"
#include "glob-util.h"
#include "hexdecoct.h"
#include "hostname-util.h"
#include "initreq.h"
#include "install.h"
#include "io-util.h"
#include "list.h"
#include "locale-util.h"
#include "log.h"
#include "logs-show.h"
#include "macro.h"
#include "mkdir.h"
#include "pager.h"
#include "parse-util.h"
#include "path-lookup.h"
#include "path-util.h"
#include "process-util.h"
#include "reboot-util.h"
#include "rlimit-util.h"
#include "set.h"
#include "sigbus.h"
#include "signal-util.h"
#include "socket-util.h"
#include "spawn-ask-password-agent.h"
#include "spawn-polkit-agent.h"
#include "special.h"
#include "stat-util.h"
#include "strv.h"
#include "terminal-util.h"
#include "unit-def.h"
#include "unit-name.h"
#include "user-util.h"
#include "util.h"
#include "utmp-wtmp.h"
#include "verbs.h"
#include "virt.h"

/* The init script exit status codes
   0       program is running or service is OK
   1       program is dead and /var/run pid file exists
   2       program is dead and /var/lock lock file exists
   3       program is not running
   4       program or service status is unknown
   5-99    reserved for future LSB use
   100-149 reserved for distribution use
   150-199 reserved for application use
   200-254 reserved
*/
enum {
        EXIT_PROGRAM_RUNNING_OR_SERVICE_OK        = 0,
        EXIT_PROGRAM_DEAD_AND_PID_EXISTS          = 1,
        EXIT_PROGRAM_DEAD_AND_LOCK_FILE_EXISTS    = 2,
        EXIT_PROGRAM_NOT_RUNNING                  = 3,
        EXIT_PROGRAM_OR_SERVICES_STATUS_UNKNOWN   = 4,
};

static char **arg_types = NULL;
static char **arg_states = NULL;
static char **arg_properties = NULL;
static bool arg_all = false;
static enum dependency {
        DEPENDENCY_FORWARD,
        DEPENDENCY_REVERSE,
        DEPENDENCY_AFTER,
        DEPENDENCY_BEFORE,
        _DEPENDENCY_MAX
} arg_dependency = DEPENDENCY_FORWARD;
static const char *arg_job_mode = "replace";
static UnitFileScope arg_scope = UNIT_FILE_SYSTEM;
static bool arg_wait = false;
static bool arg_no_block = false;
static bool arg_no_legend = false;
static bool arg_no_pager = false;
static bool arg_no_wtmp = false;
static bool arg_no_sync = false;
static bool arg_no_wall = false;
static bool arg_no_reload = false;
static bool arg_value = false;
static bool arg_show_types = false;
static bool arg_ignore_inhibitors = false;
static bool arg_dry_run = false;
static bool arg_quiet = false;
static bool arg_full = false;
static bool arg_recursive = false;
static int arg_force = 0;
static bool arg_ask_password = false;
static bool arg_runtime = false;
static UnitFilePresetMode arg_preset_mode = UNIT_FILE_PRESET_FULL;
static char **arg_wall = NULL;
static const char *arg_kill_who = NULL;
static int arg_signal = SIGTERM;
static char *arg_root = NULL;
static usec_t arg_when = 0;
static char *arg_esp_path = NULL;
static char *argv_cmdline = NULL;
static enum action {
        ACTION_SYSTEMCTL,
        ACTION_HALT,
        ACTION_POWEROFF,
        ACTION_REBOOT,
        ACTION_KEXEC,
        ACTION_EXIT,
        ACTION_SUSPEND,
        ACTION_HIBERNATE,
        ACTION_HYBRID_SLEEP,
        ACTION_RUNLEVEL2,
        ACTION_RUNLEVEL3,
        ACTION_RUNLEVEL4,
        ACTION_RUNLEVEL5,
        ACTION_RESCUE,
        ACTION_EMERGENCY,
        ACTION_DEFAULT,
        ACTION_RELOAD,
        ACTION_REEXEC,
        ACTION_RUNLEVEL,
        ACTION_CANCEL_SHUTDOWN,
        _ACTION_MAX,
        _ACTION_INVALID = -1
} arg_action = ACTION_SYSTEMCTL;
static BusTransport arg_transport = BUS_TRANSPORT_LOCAL;
static const char *arg_host = NULL;
static unsigned arg_lines = 10;
static OutputMode arg_output = OUTPUT_SHORT;
static bool arg_plain = false;
static bool arg_firmware_setup = false;
static bool arg_now = false;
static bool arg_jobs_before = false;
static bool arg_jobs_after = false;

static int daemon_reload(int argc, char *argv[], void* userdata);
static int trivial_method(int argc, char *argv[], void *userdata);
static int halt_now(enum action a);
static int get_state_one_unit(sd_bus *bus, const char *name, UnitActiveState *active_state);

static bool original_stdout_is_tty;

typedef enum BusFocus {
        BUS_FULL,      /* The full bus indicated via --system or --user */
        BUS_MANAGER,   /* The manager itself, possibly directly, possibly via the bus */
        _BUS_FOCUS_MAX
} BusFocus;

static sd_bus *busses[_BUS_FOCUS_MAX] = {};

static UnitFileFlags args_to_flags(void) {
        return (arg_runtime ? UNIT_FILE_RUNTIME : 0) |
               (arg_force   ? UNIT_FILE_FORCE   : 0);
}

static int acquire_bus(BusFocus focus, sd_bus **ret) {
        int r;

        assert(focus < _BUS_FOCUS_MAX);
        assert(ret);

        /* We only go directly to the manager, if we are using a local transport */
        if (arg_transport != BUS_TRANSPORT_LOCAL)
                focus = BUS_FULL;

        if (getenv_bool("SYSTEMCTL_FORCE_BUS") > 0)
                focus = BUS_FULL;

        if (!busses[focus]) {
                bool user;

                user = arg_scope != UNIT_FILE_SYSTEM;

                if (!user && sd_booted() <= 0) {
                        /* Print a friendly message when the local system is actually not running systemd as PID 1. */
                        log_error("System has not been booted with systemd as init system (PID 1). Can't operate.");
                        return -EHOSTDOWN;
                }

                if (focus == BUS_MANAGER)
                        r = bus_connect_transport_systemd(arg_transport, arg_host, user, &busses[focus]);
                else
                        r = bus_connect_transport(arg_transport, arg_host, user, &busses[focus]);
                if (r < 0)
                        return log_error_errno(r, "Failed to connect to bus: %m");

                (void) sd_bus_set_allow_interactive_authorization(busses[focus], arg_ask_password);
        }

        *ret = busses[focus];
        return 0;
}

static void release_busses(void) {
        BusFocus w;

        for (w = 0; w < _BUS_FOCUS_MAX; w++)
                busses[w] = sd_bus_flush_close_unref(busses[w]);
}

static int map_string_no_copy(sd_bus *bus, const char *member, sd_bus_message *m, sd_bus_error *error, void *userdata) {
        char *s;
        const char **p = userdata;
        int r;

        r = sd_bus_message_read_basic(m, SD_BUS_TYPE_STRING, &s);
        if (r < 0)
                return r;

        if (!isempty(s))
                *p = s;

        return 0;
}

static void ask_password_agent_open_if_enabled(void) {

        /* Open the password agent as a child process if necessary */

        if (arg_dry_run)
                return;

        if (!arg_ask_password)
                return;

        if (arg_scope != UNIT_FILE_SYSTEM)
                return;

        if (arg_transport != BUS_TRANSPORT_LOCAL)
                return;

        ask_password_agent_open();
}

static void polkit_agent_open_maybe(void) {
        /* Open the polkit agent as a child process if necessary */

        if (arg_scope != UNIT_FILE_SYSTEM)
                return;

        polkit_agent_open_if_enabled(arg_transport, arg_ask_password);
}

static OutputFlags get_output_flags(void) {
        return
                arg_all * OUTPUT_SHOW_ALL |
                (arg_full || !on_tty() || pager_have()) * OUTPUT_FULL_WIDTH |
                colors_enabled() * OUTPUT_COLOR |
                !arg_quiet * OUTPUT_WARN_CUTOFF;
}

static int translate_bus_error_to_exit_status(int r, const sd_bus_error *error) {
        assert(error);

        if (!sd_bus_error_is_set(error))
                return r;

        if (sd_bus_error_has_name(error, SD_BUS_ERROR_ACCESS_DENIED) ||
            sd_bus_error_has_name(error, BUS_ERROR_ONLY_BY_DEPENDENCY) ||
            sd_bus_error_has_name(error, BUS_ERROR_NO_ISOLATION) ||
            sd_bus_error_has_name(error, BUS_ERROR_TRANSACTION_IS_DESTRUCTIVE))
                return EXIT_NOPERMISSION;

        if (sd_bus_error_has_name(error, BUS_ERROR_NO_SUCH_UNIT))
                return EXIT_NOTINSTALLED;

        if (sd_bus_error_has_name(error, BUS_ERROR_JOB_TYPE_NOT_APPLICABLE) ||
            sd_bus_error_has_name(error, SD_BUS_ERROR_NOT_SUPPORTED))
                return EXIT_NOTIMPLEMENTED;

        if (sd_bus_error_has_name(error, BUS_ERROR_LOAD_FAILED))
                return EXIT_NOTCONFIGURED;

        if (r != 0)
                return r;

        return EXIT_FAILURE;
}

static bool install_client_side(void) {

        /* Decides when to execute enable/disable/... operations
         * client-side rather than server-side. */

        if (running_in_chroot_or_offline())
                return true;

        if (sd_booted() <= 0)
                return true;

        if (!isempty(arg_root))
                return true;

        if (arg_scope == UNIT_FILE_GLOBAL)
                return true;

        /* Unsupported environment variable, mostly for debugging purposes */
        if (getenv_bool("SYSTEMCTL_INSTALL_CLIENT_SIDE") > 0)
                return true;

        return false;
}

static int compare_unit_info(const void *a, const void *b) {
        const UnitInfo *u = a, *v = b;
        const char *d1, *d2;
        int r;

        /* First, order by machine */
        if (!u->machine && v->machine)
                return -1;
        if (u->machine && !v->machine)
                return 1;
        if (u->machine && v->machine) {
                r = strcasecmp(u->machine, v->machine);
                if (r != 0)
                        return r;
        }

        /* Second, order by unit type */
        d1 = strrchr(u->id, '.');
        d2 = strrchr(v->id, '.');
        if (d1 && d2) {
                r = strcasecmp(d1, d2);
                if (r != 0)
                        return r;
        }

        /* Third, order by name */
        return strcasecmp(u->id, v->id);
}

static const char* unit_type_suffix(const char *name) {
        const char *dot;

        dot = strrchr(name, '.');
        if (!dot)
                return "";

        return dot + 1;
}

static bool output_show_unit(const UnitInfo *u, char **patterns) {
        assert(u);

        if (!strv_fnmatch_or_empty(patterns, u->id, FNM_NOESCAPE))
                return false;

        if (arg_types && !strv_find(arg_types, unit_type_suffix(u->id)))
                return false;

        if (arg_all)
                return true;

        /* Note that '--all' is not purely a state filter, but also a
         * filter that hides units that "follow" other units (which is
         * used for device units that appear under different names). */
        if (!isempty(u->following))
                return false;

        if (!strv_isempty(arg_states))
                return true;

        /* By default show all units except the ones in inactive
         * state and with no pending job */
        if (u->job_id > 0)
                return true;

        if (streq(u->active_state, "inactive"))
                return false;

        return true;
}

static int output_units_list(const UnitInfo *unit_infos, unsigned c) {
        unsigned circle_len = 0, id_len, max_id_len, load_len, active_len, sub_len, job_len, desc_len, max_desc_len;
        const UnitInfo *u;
        unsigned n_shown = 0;
        int job_count = 0;

        max_id_len = STRLEN("UNIT");
        load_len = STRLEN("LOAD");
        active_len = STRLEN("ACTIVE");
        sub_len = STRLEN("SUB");
        job_len = STRLEN("JOB");
        max_desc_len = STRLEN("DESCRIPTION");

        for (u = unit_infos; u < unit_infos + c; u++) {
                max_id_len = MAX(max_id_len, strlen(u->id) + (u->machine ? strlen(u->machine)+1 : 0));
                load_len = MAX(load_len, strlen(u->load_state));
                active_len = MAX(active_len, strlen(u->active_state));
                sub_len = MAX(sub_len, strlen(u->sub_state));
                max_desc_len = MAX(max_desc_len, strlen(u->description));

                if (u->job_id != 0) {
                        job_len = MAX(job_len, strlen(u->job_type));
                        job_count++;
                }

                if (!arg_no_legend &&
                    (streq(u->active_state, "failed") ||
                     STR_IN_SET(u->load_state, "error", "not-found", "masked")))
                        circle_len = 2;
        }

        if (!arg_full && original_stdout_is_tty) {
                unsigned basic_len;

                id_len = MIN(max_id_len, 25u); /* as much as it needs, but at most 25 for now */
                basic_len = circle_len + 1 + id_len + 1 + load_len + 1 + active_len + 1 + sub_len + 1;

                if (job_count)
                        basic_len += job_len + 1;

                if (basic_len < (unsigned) columns()) {
                        unsigned extra_len, incr;
                        extra_len = columns() - basic_len;

                        /* Either UNIT already got 25, or is fully satisfied.
                         * Grant up to 25 to DESC now. */
                        incr = MIN(extra_len, 25u);
                        desc_len = incr;
                        extra_len -= incr;

                        /* Of the remainder give as much as the ID needs to the ID, and give the rest to the
                         * description but not more than it needs. */
                        if (extra_len > 0) {
                                incr = MIN(max_id_len - id_len, extra_len);
                                id_len += incr;
                                desc_len += MIN(extra_len - incr, max_desc_len - desc_len);
                        }
                } else
                        desc_len = 0;
        } else {
                id_len = max_id_len;
                desc_len = max_desc_len;
        }

        for (u = unit_infos; u < unit_infos + c; u++) {
                _cleanup_free_ char *e = NULL, *j = NULL;
                const char *on_underline = "", *off_underline = "";
                const char *on_loaded = "", *off_loaded = "";
                const char *on_active = "", *off_active = "";
                const char *on_circle = "", *off_circle = "";
                const char *id;
                bool circle = false, underline = false;

                if (!n_shown && !arg_no_legend) {

                        if (circle_len > 0)
                                fputs("  ", stdout);

                        printf("%s%-*s %-*s %-*s %-*s ",
                               ansi_underline(),
                               id_len, "UNIT",
                               load_len, "LOAD",
                               active_len, "ACTIVE",
                               sub_len, "SUB");

                        if (job_count)
                                printf("%-*s ", job_len, "JOB");

                        printf("%-*.*s%s\n",
                               desc_len,
                               !arg_full && arg_no_pager ? (int) desc_len : -1,
                               "DESCRIPTION",
                               ansi_normal());
                }

                n_shown++;

                if (u + 1 < unit_infos + c &&
                    !streq(unit_type_suffix(u->id), unit_type_suffix((u + 1)->id))) {
                        on_underline = ansi_underline();
                        off_underline = ansi_normal();
                        underline = true;
                }

                if (STR_IN_SET(u->load_state, "error", "not-found", "masked") && !arg_plain) {
                        on_circle = ansi_highlight_yellow();
                        off_circle = ansi_normal();
                        circle = true;
                        on_loaded = underline ? ansi_highlight_red_underline() : ansi_highlight_red();
                        off_loaded = underline ? on_underline : ansi_normal();
                } else if (streq(u->active_state, "failed") && !arg_plain) {
                        on_circle = ansi_highlight_red();
                        off_circle = ansi_normal();
                        circle = true;
                        on_active = underline ? ansi_highlight_red_underline() : ansi_highlight_red();
                        off_active = underline ? on_underline : ansi_normal();
                }

                if (u->machine) {
                        j = strjoin(u->machine, ":", u->id);
                        if (!j)
                                return log_oom();

                        id = j;
                } else
                        id = u->id;

                if (arg_full) {
                        e = ellipsize(id, id_len, 33);
                        if (!e)
                                return log_oom();

                        id = e;
                }

                if (circle_len > 0)
                        printf("%s%s%s ", on_circle, circle ? special_glyph(BLACK_CIRCLE) : " ", off_circle);

                printf("%s%s%-*s%s %s%-*s%s %s%-*s %-*s%s %-*s",
                       on_underline,
                       on_active, id_len, id, off_active,
                       on_loaded, load_len, u->load_state, off_loaded,
                       on_active, active_len, u->active_state,
                       sub_len, u->sub_state, off_active,
                       job_count ? job_len + 1 : 0, u->job_id ? u->job_type : "");

                printf("%-*.*s%s\n",
                       desc_len,
                       !arg_full && arg_no_pager ? (int) desc_len : -1,
                       u->description,
                       off_underline);
        }

        if (!arg_no_legend) {
                const char *on, *off;

                if (n_shown) {
                        puts("\n"
                             "LOAD   = Reflects whether the unit definition was properly loaded.\n"
                             "ACTIVE = The high-level unit activation state, i.e. generalization of SUB.\n"
                             "SUB    = The low-level unit activation state, values depend on unit type.");
                        puts(job_count ? "JOB    = Pending job for the unit.\n" : "");
                        on = ansi_highlight();
                        off = ansi_normal();
                } else {
                        on = ansi_highlight_red();
                        off = ansi_normal();
                }

                if (arg_all)
                        printf("%s%u loaded units listed.%s\n"
                               "To show all installed unit files use 'systemctl list-unit-files'.\n",
                               on, n_shown, off);
                else
                        printf("%s%u loaded units listed.%s Pass --all to see loaded but inactive units, too.\n"
                               "To show all installed unit files use 'systemctl list-unit-files'.\n",
                               on, n_shown, off);
        }

        return 0;
}

static int get_unit_list(
                sd_bus *bus,
                const char *machine,
                char **patterns,
                UnitInfo **unit_infos,
                int c,
                sd_bus_message **_reply) {

        _cleanup_(sd_bus_message_unrefp) sd_bus_message *m = NULL;
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
        size_t size = c;
        int r;
        UnitInfo u;
        bool fallback = false;

        assert(bus);
        assert(unit_infos);
        assert(_reply);

        r = sd_bus_message_new_method_call(
                        bus,
                        &m,
                        "org.freedesktop.systemd1",
                        "/org/freedesktop/systemd1",
                        "org.freedesktop.systemd1.Manager",
                        "ListUnitsByPatterns");
        if (r < 0)
                return bus_log_create_error(r);

        r = sd_bus_message_append_strv(m, arg_states);
        if (r < 0)
                return bus_log_create_error(r);

        r = sd_bus_message_append_strv(m, patterns);
        if (r < 0)
                return bus_log_create_error(r);

        r = sd_bus_call(bus, m, 0, &error, &reply);
        if (r < 0 && (sd_bus_error_has_name(&error, SD_BUS_ERROR_UNKNOWN_METHOD) ||
                      sd_bus_error_has_name(&error, SD_BUS_ERROR_ACCESS_DENIED))) {
                /* Fallback to legacy ListUnitsFiltered method */
                fallback = true;
                log_debug_errno(r, "Failed to list units: %s Falling back to ListUnitsFiltered method.", bus_error_message(&error, r));
                m = sd_bus_message_unref(m);
                sd_bus_error_free(&error);

                r = sd_bus_message_new_method_call(
                                bus,
                                &m,
                                "org.freedesktop.systemd1",
                                "/org/freedesktop/systemd1",
                                "org.freedesktop.systemd1.Manager",
                                "ListUnitsFiltered");
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_append_strv(m, arg_states);
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_call(bus, m, 0, &error, &reply);
        }
        if (r < 0)
                return log_error_errno(r, "Failed to list units: %s", bus_error_message(&error, r));

        r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "(ssssssouso)");
        if (r < 0)
                return bus_log_parse_error(r);

        while ((r = bus_parse_unit_info(reply, &u)) > 0) {
                u.machine = machine;

                if (!output_show_unit(&u, fallback ? patterns : NULL))
                        continue;

                if (!GREEDY_REALLOC(*unit_infos, size, c+1))
                        return log_oom();

                (*unit_infos)[c++] = u;
        }
        if (r < 0)
                return bus_log_parse_error(r);

        r = sd_bus_message_exit_container(reply);
        if (r < 0)
                return bus_log_parse_error(r);

        *_reply = reply;
        reply = NULL;

        return c;
}

static void message_set_freep(Set **set) {
        set_free_with_destructor(*set, sd_bus_message_unref);
}

static int get_unit_list_recursive(
                sd_bus *bus,
                char **patterns,
                UnitInfo **_unit_infos,
                Set **_replies,
                char ***_machines) {

        _cleanup_free_ UnitInfo *unit_infos = NULL;
        _cleanup_(message_set_freep) Set *replies;
        sd_bus_message *reply;
        int c, r;

        assert(bus);
        assert(_replies);
        assert(_unit_infos);
        assert(_machines);

        replies = set_new(NULL);
        if (!replies)
                return log_oom();

        c = get_unit_list(bus, NULL, patterns, &unit_infos, 0, &reply);
        if (c < 0)
                return c;

        r = set_put(replies, reply);
        if (r < 0) {
                sd_bus_message_unref(reply);
                return log_oom();
        }

        if (arg_recursive) {
                _cleanup_strv_free_ char **machines = NULL;
                char **i;

                r = sd_get_machine_names(&machines);
                if (r < 0)
                        return log_error_errno(r, "Failed to get machine names: %m");

                STRV_FOREACH(i, machines) {
                        _cleanup_(sd_bus_flush_close_unrefp) sd_bus *container = NULL;
                        int k;

                        r = sd_bus_open_system_machine(&container, *i);
                        if (r < 0) {
                                log_warning_errno(r, "Failed to connect to container %s, ignoring: %m", *i);
                                continue;
                        }

                        k = get_unit_list(container, *i, patterns, &unit_infos, c, &reply);
                        if (k < 0)
                                return k;

                        c = k;

                        r = set_put(replies, reply);
                        if (r < 0) {
                                sd_bus_message_unref(reply);
                                return log_oom();
                        }
                }

                *_machines = machines;
                machines = NULL;
        } else
                *_machines = NULL;

        *_unit_infos = unit_infos;
        unit_infos = NULL;

        *_replies = replies;
        replies = NULL;

        return c;
}

static int list_units(int argc, char *argv[], void *userdata) {
        _cleanup_free_ UnitInfo *unit_infos = NULL;
        _cleanup_(message_set_freep) Set *replies = NULL;
        _cleanup_strv_free_ char **machines = NULL;
        sd_bus *bus;
        int r;

        r = acquire_bus(BUS_MANAGER, &bus);
        if (r < 0)
                return r;

        pager_open(arg_no_pager, false);

        r = get_unit_list_recursive(bus, strv_skip(argv, 1), &unit_infos, &replies, &machines);
        if (r < 0)
                return r;

        qsort_safe(unit_infos, r, sizeof(UnitInfo), compare_unit_info);
        return output_units_list(unit_infos, r);
}

static int get_triggered_units(
                sd_bus *bus,
                const char* path,
                char*** ret) {

        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        int r;

        assert(bus);
        assert(path);
        assert(ret);

        r = sd_bus_get_property_strv(
                        bus,
                        "org.freedesktop.systemd1",
                        path,
                        "org.freedesktop.systemd1.Unit",
                        "Triggers",
                        &error,
                        ret);
        if (r < 0)
                return log_error_errno(r, "Failed to determine triggers: %s", bus_error_message(&error, r));

        return 0;
}

static int get_listening(
                sd_bus *bus,
                const char* unit_path,
                char*** listening) {

        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
        const char *type, *path;
        int r, n = 0;

        r = sd_bus_get_property(
                        bus,
                        "org.freedesktop.systemd1",
                        unit_path,
                        "org.freedesktop.systemd1.Socket",
                        "Listen",
                        &error,
                        &reply,
                        "a(ss)");
        if (r < 0)
                return log_error_errno(r, "Failed to get list of listening sockets: %s", bus_error_message(&error, r));

        r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "(ss)");
        if (r < 0)
                return bus_log_parse_error(r);

        while ((r = sd_bus_message_read(reply, "(ss)", &type, &path)) > 0) {

                r = strv_extend(listening, type);
                if (r < 0)
                        return log_oom();

                r = strv_extend(listening, path);
                if (r < 0)
                        return log_oom();

                n++;
        }
        if (r < 0)
                return bus_log_parse_error(r);

        r = sd_bus_message_exit_container(reply);
        if (r < 0)
                return bus_log_parse_error(r);

        return n;
}

struct socket_info {
        const char *machine;
        const char* id;

        char* type;
        char* path;

        /* Note: triggered is a list here, although it almost certainly
         * will always be one unit. Nevertheless, dbus API allows for multiple
         * values, so let's follow that. */
        char** triggered;

        /* The strv above is shared. free is set only in the first one. */
        bool own_triggered;
};

static int socket_info_compare(const struct socket_info *a, const struct socket_info *b) {
        int o;

        assert(a);
        assert(b);

        if (!a->machine && b->machine)
                return -1;
        if (a->machine && !b->machine)
                return 1;
        if (a->machine && b->machine) {
                o = strcasecmp(a->machine, b->machine);
                if (o != 0)
                        return o;
        }

        o = strcmp(a->path, b->path);
        if (o == 0)
                o = strcmp(a->type, b->type);

        return o;
}

static int output_sockets_list(struct socket_info *socket_infos, unsigned cs) {
        struct socket_info *s;
        unsigned pathlen = STRLEN("LISTEN"),
                typelen = STRLEN("TYPE") * arg_show_types,
                socklen = STRLEN("UNIT"),
                servlen = STRLEN("ACTIVATES");
        const char *on, *off;

        for (s = socket_infos; s < socket_infos + cs; s++) {
                unsigned tmp = 0;
                char **a;

                socklen = MAX(socklen, strlen(s->id));
                if (arg_show_types)
                        typelen = MAX(typelen, strlen(s->type));
                pathlen = MAX(pathlen, strlen(s->path) + (s->machine ? strlen(s->machine)+1 : 0));

                STRV_FOREACH(a, s->triggered)
                        tmp += strlen(*a) + 2*(a != s->triggered);
                servlen = MAX(servlen, tmp);
        }

        if (cs) {
                if (!arg_no_legend)
                        printf("%-*s %-*.*s%-*s %s\n",
                               pathlen, "LISTEN",
                               typelen + arg_show_types, typelen + arg_show_types, "TYPE ",
                               socklen, "UNIT",
                               "ACTIVATES");

                for (s = socket_infos; s < socket_infos + cs; s++) {
                        _cleanup_free_ char *j = NULL;
                        const char *path;
                        char **a;

                        if (s->machine) {
                                j = strjoin(s->machine, ":", s->path);
                                if (!j)
                                        return log_oom();
                                path = j;
                        } else
                                path = s->path;

                        if (arg_show_types)
                                printf("%-*s %-*s %-*s",
                                       pathlen, path, typelen, s->type, socklen, s->id);
                        else
                                printf("%-*s %-*s",
                                       pathlen, path, socklen, s->id);
                        STRV_FOREACH(a, s->triggered)
                                printf("%s %s",
                                       a == s->triggered ? "" : ",", *a);
                        printf("\n");
                }

                on = ansi_highlight();
                off = ansi_normal();
                if (!arg_no_legend)
                        printf("\n");
        } else {
                on = ansi_highlight_red();
                off = ansi_normal();
        }

        if (!arg_no_legend) {
                printf("%s%u sockets listed.%s\n", on, cs, off);
                if (!arg_all)
                        printf("Pass --all to see loaded but inactive sockets, too.\n");
        }

        return 0;
}

static int list_sockets(int argc, char *argv[], void *userdata) {
        _cleanup_(message_set_freep) Set *replies = NULL;
        _cleanup_strv_free_ char **machines = NULL;
        _cleanup_free_ UnitInfo *unit_infos = NULL;
        _cleanup_free_ struct socket_info *socket_infos = NULL;
        const UnitInfo *u;
        struct socket_info *s;
        unsigned cs = 0;
        size_t size = 0;
        int r = 0, n;
        sd_bus *bus;

        r = acquire_bus(BUS_MANAGER, &bus);
        if (r < 0)
                return r;

        pager_open(arg_no_pager, false);

        n = get_unit_list_recursive(bus, strv_skip(argv, 1), &unit_infos, &replies, &machines);
        if (n < 0)
                return n;

        for (u = unit_infos; u < unit_infos + n; u++) {
                _cleanup_strv_free_ char **listening = NULL, **triggered = NULL;
                int i, c;

                if (!endswith(u->id, ".socket"))
                        continue;

                r = get_triggered_units(bus, u->unit_path, &triggered);
                if (r < 0)
                        goto cleanup;

                c = get_listening(bus, u->unit_path, &listening);
                if (c < 0) {
                        r = c;
                        goto cleanup;
                }

                if (!GREEDY_REALLOC(socket_infos, size, cs + c)) {
                        r = log_oom();
                        goto cleanup;
                }

                for (i = 0; i < c; i++)
                        socket_infos[cs + i] = (struct socket_info) {
                                .machine = u->machine,
                                .id = u->id,
                                .type = listening[i*2],
                                .path = listening[i*2 + 1],
                                .triggered = triggered,
                                .own_triggered = i==0,
                        };

                /* from this point on we will cleanup those socket_infos */
                cs += c;
                free(listening);
                listening = triggered = NULL; /* avoid cleanup */
        }

        qsort_safe(socket_infos, cs, sizeof(struct socket_info),
                   (__compar_fn_t) socket_info_compare);

        output_sockets_list(socket_infos, cs);

 cleanup:
        assert(cs == 0 || socket_infos);
        for (s = socket_infos; s < socket_infos + cs; s++) {
                free(s->type);
                free(s->path);
                if (s->own_triggered)
                        strv_free(s->triggered);
        }

        return r;
}

static int get_next_elapse(
                sd_bus *bus,
                const char *path,
                dual_timestamp *next) {

        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        dual_timestamp t;
        int r;

        assert(bus);
        assert(path);
        assert(next);

        r = sd_bus_get_property_trivial(
                        bus,
                        "org.freedesktop.systemd1",
                        path,
                        "org.freedesktop.systemd1.Timer",
                        "NextElapseUSecMonotonic",
                        &error,
                        't',
                        &t.monotonic);
        if (r < 0)
                return log_error_errno(r, "Failed to get next elapse time: %s", bus_error_message(&error, r));

        r = sd_bus_get_property_trivial(
                        bus,
                        "org.freedesktop.systemd1",
                        path,
                        "org.freedesktop.systemd1.Timer",
                        "NextElapseUSecRealtime",
                        &error,
                        't',
                        &t.realtime);
        if (r < 0)
                return log_error_errno(r, "Failed to get next elapse time: %s", bus_error_message(&error, r));

        *next = t;
        return 0;
}

static int get_last_trigger(
                sd_bus *bus,
                const char *path,
                usec_t *last) {

        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        int r;

        assert(bus);
        assert(path);
        assert(last);

        r = sd_bus_get_property_trivial(
                        bus,
                        "org.freedesktop.systemd1",
                        path,
                        "org.freedesktop.systemd1.Timer",
                        "LastTriggerUSec",
                        &error,
                        't',
                        last);
        if (r < 0)
                return log_error_errno(r, "Failed to get last trigger time: %s", bus_error_message(&error, r));

        return 0;
}

struct timer_info {
        const char* machine;
        const char* id;
        usec_t next_elapse;
        usec_t last_trigger;
        char** triggered;
};

static int timer_info_compare(const struct timer_info *a, const struct timer_info *b) {
        int o;

        assert(a);
        assert(b);

        if (!a->machine && b->machine)
                return -1;
        if (a->machine && !b->machine)
                return 1;
        if (a->machine && b->machine) {
                o = strcasecmp(a->machine, b->machine);
                if (o != 0)
                        return o;
        }

        if (a->next_elapse < b->next_elapse)
                return -1;
        if (a->next_elapse > b->next_elapse)
                return 1;

        return strcmp(a->id, b->id);
}

static int output_timers_list(struct timer_info *timer_infos, unsigned n) {
        struct timer_info *t;
        unsigned
                nextlen = STRLEN("NEXT"),
                leftlen = STRLEN("LEFT"),
                lastlen = STRLEN("LAST"),
                passedlen = STRLEN("PASSED"),
                unitlen = STRLEN("UNIT"),
                activatelen = STRLEN("ACTIVATES");

        const char *on, *off;

        assert(timer_infos || n == 0);

        for (t = timer_infos; t < timer_infos + n; t++) {
                unsigned ul = 0;
                char **a;

                if (t->next_elapse > 0) {
                        char tstamp[FORMAT_TIMESTAMP_MAX] = "", trel[FORMAT_TIMESTAMP_RELATIVE_MAX] = "";

                        format_timestamp(tstamp, sizeof(tstamp), t->next_elapse);
                        nextlen = MAX(nextlen, strlen(tstamp) + 1);

                        format_timestamp_relative(trel, sizeof(trel), t->next_elapse);
                        leftlen = MAX(leftlen, strlen(trel));
                }

                if (t->last_trigger > 0) {
                        char tstamp[FORMAT_TIMESTAMP_MAX] = "", trel[FORMAT_TIMESTAMP_RELATIVE_MAX] = "";

                        format_timestamp(tstamp, sizeof(tstamp), t->last_trigger);
                        lastlen = MAX(lastlen, strlen(tstamp) + 1);

                        format_timestamp_relative(trel, sizeof(trel), t->last_trigger);
                        passedlen = MAX(passedlen, strlen(trel));
                }

                unitlen = MAX(unitlen, strlen(t->id) + (t->machine ? strlen(t->machine)+1 : 0));

                STRV_FOREACH(a, t->triggered)
                        ul += strlen(*a) + 2*(a != t->triggered);

                activatelen = MAX(activatelen, ul);
        }

        if (n > 0) {
                if (!arg_no_legend)
                        printf("%-*s %-*s %-*s %-*s %-*s %s\n",
                               nextlen,   "NEXT",
                               leftlen,   "LEFT",
                               lastlen,   "LAST",
                               passedlen, "PASSED",
                               unitlen,   "UNIT",
                                          "ACTIVATES");

                for (t = timer_infos; t < timer_infos + n; t++) {
                        _cleanup_free_ char *j = NULL;
                        const char *unit;
                        char tstamp1[FORMAT_TIMESTAMP_MAX] = "n/a", trel1[FORMAT_TIMESTAMP_RELATIVE_MAX] = "n/a";
                        char tstamp2[FORMAT_TIMESTAMP_MAX] = "n/a", trel2[FORMAT_TIMESTAMP_RELATIVE_MAX] = "n/a";
                        char **a;

                        format_timestamp(tstamp1, sizeof(tstamp1), t->next_elapse);
                        format_timestamp_relative(trel1, sizeof(trel1), t->next_elapse);

                        format_timestamp(tstamp2, sizeof(tstamp2), t->last_trigger);
                        format_timestamp_relative(trel2, sizeof(trel2), t->last_trigger);

                        if (t->machine) {
                                j = strjoin(t->machine, ":", t->id);
                                if (!j)
                                        return log_oom();
                                unit = j;
                        } else
                                unit = t->id;

                        printf("%-*s %-*s %-*s %-*s %-*s",
                               nextlen, tstamp1, leftlen, trel1, lastlen, tstamp2, passedlen, trel2, unitlen, unit);

                        STRV_FOREACH(a, t->triggered)
                                printf("%s %s",
                                       a == t->triggered ? "" : ",", *a);
                        printf("\n");
                }

                on = ansi_highlight();
                off = ansi_normal();
                if (!arg_no_legend)
                        printf("\n");
        } else {
                on = ansi_highlight_red();
                off = ansi_normal();
        }

        if (!arg_no_legend) {
                printf("%s%u timers listed.%s\n", on, n, off);
                if (!arg_all)
                        printf("Pass --all to see loaded but inactive timers, too.\n");
        }

        return 0;
}

static usec_t calc_next_elapse(dual_timestamp *nw, dual_timestamp *next) {
        usec_t next_elapse;

        assert(nw);
        assert(next);

        if (next->monotonic != USEC_INFINITY && next->monotonic > 0) {
                usec_t converted;

                if (next->monotonic > nw->monotonic)
                        converted = nw->realtime + (next->monotonic - nw->monotonic);
                else
                        converted = nw->realtime - (nw->monotonic - next->monotonic);

                if (next->realtime != USEC_INFINITY && next->realtime > 0)
                        next_elapse = MIN(converted, next->realtime);
                else
                        next_elapse = converted;

        } else
                next_elapse = next->realtime;

        return next_elapse;
}

static int list_timers(int argc, char *argv[], void *userdata) {
        _cleanup_(message_set_freep) Set *replies = NULL;
        _cleanup_strv_free_ char **machines = NULL;
        _cleanup_free_ struct timer_info *timer_infos = NULL;
        _cleanup_free_ UnitInfo *unit_infos = NULL;
        struct timer_info *t;
        const UnitInfo *u;
        size_t size = 0;
        int n, c = 0;
        dual_timestamp nw;
        sd_bus *bus;
        int r = 0;

        r = acquire_bus(BUS_MANAGER, &bus);
        if (r < 0)
                return r;

        pager_open(arg_no_pager, false);

        n = get_unit_list_recursive(bus, strv_skip(argv, 1), &unit_infos, &replies, &machines);
        if (n < 0)
                return n;

        dual_timestamp_get(&nw);

        for (u = unit_infos; u < unit_infos + n; u++) {
                _cleanup_strv_free_ char **triggered = NULL;
                dual_timestamp next = DUAL_TIMESTAMP_NULL;
                usec_t m, last = 0;

                if (!endswith(u->id, ".timer"))
                        continue;

                r = get_triggered_units(bus, u->unit_path, &triggered);
                if (r < 0)
                        goto cleanup;

                r = get_next_elapse(bus, u->unit_path, &next);
                if (r < 0)
                        goto cleanup;

                get_last_trigger(bus, u->unit_path, &last);

                if (!GREEDY_REALLOC(timer_infos, size, c+1)) {
                        r = log_oom();
                        goto cleanup;
                }

                m = calc_next_elapse(&nw, &next);

                timer_infos[c++] = (struct timer_info) {
                        .machine = u->machine,
                        .id = u->id,
                        .next_elapse = m,
                        .last_trigger = last,
                        .triggered = triggered,
                };

                triggered = NULL; /* avoid cleanup */
        }

        qsort_safe(timer_infos, c, sizeof(struct timer_info),
                   (__compar_fn_t) timer_info_compare);

        output_timers_list(timer_infos, c);

 cleanup:
        for (t = timer_infos; t < timer_infos + c; t++)
                strv_free(t->triggered);

        return r;
}

static int compare_unit_file_list(const void *a, const void *b) {
        const char *d1, *d2;
        const UnitFileList *u = a, *v = b;

        d1 = strrchr(u->path, '.');
        d2 = strrchr(v->path, '.');

        if (d1 && d2) {
                int r;

                r = strcasecmp(d1, d2);
                if (r != 0)
                        return r;
        }

        return strcasecmp(basename(u->path), basename(v->path));
}

static bool output_show_unit_file(const UnitFileList *u, char **states, char **patterns) {
        assert(u);

        if (!strv_fnmatch_or_empty(patterns, basename(u->path), FNM_NOESCAPE))
                return false;

        if (!strv_isempty(arg_types)) {
                const char *dot;

                dot = strrchr(u->path, '.');
                if (!dot)
                        return false;

                if (!strv_find(arg_types, dot+1))
                        return false;
        }

        if (!strv_isempty(states) &&
            !strv_find(states, unit_file_state_to_string(u->state)))
                return false;

        return true;
}

static void output_unit_file_list(const UnitFileList *units, unsigned c) {
        unsigned max_id_len, id_cols, state_cols;
        const UnitFileList *u;

        max_id_len = STRLEN("UNIT FILE");
        state_cols = STRLEN("STATE");

        for (u = units; u < units + c; u++) {
                max_id_len = MAX(max_id_len, strlen(basename(u->path)));
                state_cols = MAX(state_cols, strlen(unit_file_state_to_string(u->state)));
        }

        if (!arg_full) {
                unsigned basic_cols;

                id_cols = MIN(max_id_len, 25u);
                basic_cols = 1 + id_cols + state_cols;
                if (basic_cols < (unsigned) columns())
                        id_cols += MIN(columns() - basic_cols, max_id_len - id_cols);
        } else
                id_cols = max_id_len;

        if (!arg_no_legend && c > 0)
                printf("%s%-*s %-*s%s\n",
                       ansi_underline(),
                       id_cols, "UNIT FILE",
                       state_cols, "STATE",
                       ansi_normal());

        for (u = units; u < units + c; u++) {
                const char *on_underline = NULL, *on_color = NULL, *off = NULL, *id;
                _cleanup_free_ char *e = NULL;
                bool underline;

                underline = u + 1 < units + c &&
                        !streq(unit_type_suffix(u->path), unit_type_suffix((u + 1)->path));

                if (underline)
                        on_underline = ansi_underline();

                if (IN_SET(u->state,
                           UNIT_FILE_MASKED,
                           UNIT_FILE_MASKED_RUNTIME,
                           UNIT_FILE_DISABLED,
                           UNIT_FILE_BAD))
                        on_color = underline ? ansi_highlight_red_underline() : ansi_highlight_red();
                else if (u->state == UNIT_FILE_ENABLED)
                        on_color = underline ? ansi_highlight_green_underline() : ansi_highlight_green();

                if (on_underline || on_color)
                        off = ansi_normal();

                id = basename(u->path);

                e = arg_full ? NULL : ellipsize(id, id_cols, 33);

                printf("%s%-*s %s%-*s%s\n",
                       strempty(on_underline),
                       id_cols, e ? e : id,
                       strempty(on_color), state_cols, unit_file_state_to_string(u->state), strempty(off));
        }

        if (!arg_no_legend)
                printf("\n%u unit files listed.\n", c);
}

static int list_unit_files(int argc, char *argv[], void *userdata) {
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
        _cleanup_free_ UnitFileList *units = NULL;
        UnitFileList *unit;
        size_t size = 0;
        unsigned c = 0;
        const char *state;
        char *path;
        int r;
        bool fallback = false;

        if (install_client_side()) {
                Hashmap *h;
                UnitFileList *u;
                Iterator i;
                unsigned n_units;

                h = hashmap_new(&string_hash_ops);
                if (!h)
                        return log_oom();

                r = unit_file_get_list(arg_scope, arg_root, h, arg_states, strv_skip(argv, 1));
                if (r < 0) {
                        unit_file_list_free(h);
                        return log_error_errno(r, "Failed to get unit file list: %m");
                }

                n_units = hashmap_size(h);

                units = new(UnitFileList, n_units ?: 1); /* avoid malloc(0) */
                if (!units) {
                        unit_file_list_free(h);
                        return log_oom();
                }

                HASHMAP_FOREACH(u, h, i) {
                        if (!output_show_unit_file(u, NULL, NULL))
                                continue;

                        units[c++] = *u;
                        free(u);
                }

                assert(c <= n_units);
                hashmap_free(h);

                r = 0;
        } else {
                _cleanup_(sd_bus_message_unrefp) sd_bus_message *m = NULL;
                _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
                sd_bus *bus;

                r = acquire_bus(BUS_MANAGER, &bus);
                if (r < 0)
                        return r;

                r = sd_bus_message_new_method_call(
                                bus,
                                &m,
                                "org.freedesktop.systemd1",
                                "/org/freedesktop/systemd1",
                                "org.freedesktop.systemd1.Manager",
                                "ListUnitFilesByPatterns");
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_append_strv(m, arg_states);
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_append_strv(m, strv_skip(argv, 1));
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_call(bus, m, 0, &error, &reply);
                if (r < 0 && sd_bus_error_has_name(&error, SD_BUS_ERROR_UNKNOWN_METHOD)) {
                        /* Fallback to legacy ListUnitFiles method */
                        fallback = true;
                        log_debug_errno(r, "Failed to list unit files: %s Falling back to ListUnitsFiles method.", bus_error_message(&error, r));
                        m = sd_bus_message_unref(m);
                        sd_bus_error_free(&error);

                        r = sd_bus_message_new_method_call(
                                        bus,
                                        &m,
                                        "org.freedesktop.systemd1",
                                        "/org/freedesktop/systemd1",
                                        "org.freedesktop.systemd1.Manager",
                                        "ListUnitFiles");
                        if (r < 0)
                                return bus_log_create_error(r);

                        r = sd_bus_call(bus, m, 0, &error, &reply);
                }
                if (r < 0)
                        return log_error_errno(r, "Failed to list unit files: %s", bus_error_message(&error, r));

                r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "(ss)");
                if (r < 0)
                        return bus_log_parse_error(r);

                while ((r = sd_bus_message_read(reply, "(ss)", &path, &state)) > 0) {

                        if (!GREEDY_REALLOC(units, size, c + 1))
                                return log_oom();

                        units[c] = (struct UnitFileList) {
                                path,
                                unit_file_state_from_string(state)
                        };

                        if (output_show_unit_file(&units[c],
                            fallback ? arg_states : NULL,
                            fallback ? strv_skip(argv, 1) : NULL))
                                c++;

                }
                if (r < 0)
                        return bus_log_parse_error(r);

                r = sd_bus_message_exit_container(reply);
                if (r < 0)
                        return bus_log_parse_error(r);
        }

        pager_open(arg_no_pager, false);

        qsort_safe(units, c, sizeof(UnitFileList), compare_unit_file_list);
        output_unit_file_list(units, c);

        if (install_client_side())
                for (unit = units; unit < units + c; unit++)
                        free(unit->path);

        return 0;
}

static int list_dependencies_print(const char *name, int level, unsigned int branches, bool last) {
        _cleanup_free_ char *n = NULL;
        size_t max_len = MAX(columns(),20u);
        size_t len = 0;
        int i;

        if (!arg_plain) {

                for (i = level - 1; i >= 0; i--) {
                        len += 2;
                        if (len > max_len - 3 && !arg_full) {
                                printf("%s...\n",max_len % 2 ? "" : " ");
                                return 0;
                        }
                        printf("%s", special_glyph(branches & (1 << i) ? TREE_VERTICAL : TREE_SPACE));
                }
                len += 2;

                if (len > max_len - 3 && !arg_full) {
                        printf("%s...\n",max_len % 2 ? "" : " ");
                        return 0;
                }

                printf("%s", special_glyph(last ? TREE_RIGHT : TREE_BRANCH));
        }

        if (arg_full) {
                printf("%s\n", name);
                return 0;
        }

        n = ellipsize(name, max_len-len, 100);
        if (!n)
                return log_oom();

        printf("%s\n", n);
        return 0;
}

static int list_dependencies_get_dependencies(sd_bus *bus, const char *name, char ***deps) {

        static const char *dependencies[_DEPENDENCY_MAX] = {
                [DEPENDENCY_FORWARD] = "Requires\0"
                                       "Requisite\0"
                                       "Wants\0"
                                       "ConsistsOf\0"
                                       "BindsTo\0",
                [DEPENDENCY_REVERSE] = "RequiredBy\0"
                                       "RequisiteOf\0"
                                       "WantedBy\0"
                                       "PartOf\0"
                                       "BoundBy\0",
                [DEPENDENCY_AFTER]   = "After\0",
                [DEPENDENCY_BEFORE]  = "Before\0",
        };

        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
        _cleanup_strv_free_ char **ret = NULL;
        _cleanup_free_ char *path = NULL;
        int r;

        assert(bus);
        assert(name);
        assert(deps);
        assert_cc(ELEMENTSOF(dependencies) == _DEPENDENCY_MAX);

        path = unit_dbus_path_from_name(name);
        if (!path)
                return log_oom();

        r = sd_bus_call_method(
                        bus,
                        "org.freedesktop.systemd1",
                        path,
                        "org.freedesktop.DBus.Properties",
                        "GetAll",
                        &error,
                        &reply,
                        "s", "org.freedesktop.systemd1.Unit");
        if (r < 0)
                return log_error_errno(r, "Failed to get properties of %s: %s", name, bus_error_message(&error, r));

        r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "{sv}");
        if (r < 0)
                return bus_log_parse_error(r);

        while ((r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_DICT_ENTRY, "sv")) > 0) {
                const char *prop;

                r = sd_bus_message_read(reply, "s", &prop);
                if (r < 0)
                        return bus_log_parse_error(r);

                if (!nulstr_contains(dependencies[arg_dependency], prop)) {
                        r = sd_bus_message_skip(reply, "v");
                        if (r < 0)
                                return bus_log_parse_error(r);
                } else {

                        r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_VARIANT, "as");
                        if (r < 0)
                                return bus_log_parse_error(r);

                        r = bus_message_read_strv_extend(reply, &ret);
                        if (r < 0)
                                return bus_log_parse_error(r);

                        r = sd_bus_message_exit_container(reply);
                        if (r < 0)
                                return bus_log_parse_error(r);
                }

                r = sd_bus_message_exit_container(reply);
                if (r < 0)
                        return bus_log_parse_error(r);

        }
        if (r < 0)
                return bus_log_parse_error(r);

        r = sd_bus_message_exit_container(reply);
        if (r < 0)
                return bus_log_parse_error(r);

        *deps = strv_uniq(ret);
        ret = NULL;

        return 0;
}

static int list_dependencies_compare(const void *_a, const void *_b) {
        const char **a = (const char**) _a, **b = (const char**) _b;

        if (unit_name_to_type(*a) == UNIT_TARGET && unit_name_to_type(*b) != UNIT_TARGET)
                return 1;
        if (unit_name_to_type(*a) != UNIT_TARGET && unit_name_to_type(*b) == UNIT_TARGET)
                return -1;

        return strcasecmp(*a, *b);
}

static int list_dependencies_one(
                sd_bus *bus,
                const char *name,
                int level,
                char ***units,
                unsigned int branches) {

        _cleanup_strv_free_ char **deps = NULL;
        char **c;
        int r = 0;

        assert(bus);
        assert(name);
        assert(units);

        r = strv_extend(units, name);
        if (r < 0)
                return log_oom();

        r = list_dependencies_get_dependencies(bus, name, &deps);
        if (r < 0)
                return r;

        qsort_safe(deps, strv_length(deps), sizeof (char*), list_dependencies_compare);

        STRV_FOREACH(c, deps) {
                if (strv_contains(*units, *c)) {
                        if (!arg_plain) {
                                printf("  ");
                                r = list_dependencies_print("...", level + 1, (branches << 1) | (c[1] == NULL ? 0 : 1), 1);
                                if (r < 0)
                                        return r;
                        }
                        continue;
                }

                if (arg_plain)
                        printf("  ");
                else {
                        UnitActiveState active_state = _UNIT_ACTIVE_STATE_INVALID;
                        const char *on;

                        (void) get_state_one_unit(bus, *c, &active_state);

                        switch (active_state) {
                        case UNIT_ACTIVE:
                        case UNIT_RELOADING:
                        case UNIT_ACTIVATING:
                                on = ansi_highlight_green();
                                break;

                        case UNIT_INACTIVE:
                        case UNIT_DEACTIVATING:
                                on = ansi_normal();
                                break;

                        default:
                                on = ansi_highlight_red();
                                break;
                        }

                        printf("%s%s%s ", on, special_glyph(BLACK_CIRCLE), ansi_normal());
                }

                r = list_dependencies_print(*c, level, branches, c[1] == NULL);
                if (r < 0)
                        return r;

                if (arg_all || unit_name_to_type(*c) == UNIT_TARGET) {
                       r = list_dependencies_one(bus, *c, level + 1, units, (branches << 1) | (c[1] == NULL ? 0 : 1));
                       if (r < 0)
                               return r;
                }
        }

        if (!arg_plain)
                strv_remove(*units, name);

        return 0;
}

static int list_dependencies(int argc, char *argv[], void *userdata) {
        _cleanup_strv_free_ char **units = NULL;
        _cleanup_free_ char *unit = NULL;
        const char *u;
        sd_bus *bus;
        int r;

        if (argv[1]) {
                r = unit_name_mangle(argv[1], UNIT_NAME_NOGLOB, &unit);
                if (r < 0)
                        return log_error_errno(r, "Failed to mangle unit name: %m");

                u = unit;
        } else
                u = SPECIAL_DEFAULT_TARGET;

        r = acquire_bus(BUS_MANAGER, &bus);
        if (r < 0)
                return r;

        pager_open(arg_no_pager, false);

        puts(u);

        return list_dependencies_one(bus, u, 0, &units, 0);
}

struct machine_info {
        bool is_host;
        char *name;
        char *state;
        char *control_group;
        uint32_t n_failed_units;
        uint32_t n_jobs;
        usec_t timestamp;
};

static const struct bus_properties_map machine_info_property_map[] = {
        { "SystemState",        "s", NULL, offsetof(struct machine_info, state)          },
        { "NJobs",              "u", NULL, offsetof(struct machine_info, n_jobs)         },
        { "NFailedUnits",       "u", NULL, offsetof(struct machine_info, n_failed_units) },
        { "ControlGroup",       "s", NULL, offsetof(struct machine_info, control_group)  },
        { "UserspaceTimestamp", "t", NULL, offsetof(struct machine_info, timestamp)      },
        {}
};

static void machine_info_clear(struct machine_info *info) {
        assert(info);

        free(info->name);
        free(info->state);
        free(info->control_group);
        zero(*info);
}

static void free_machines_list(struct machine_info *machine_infos, int n) {
        int i;

        if (!machine_infos)
                return;

        for (i = 0; i < n; i++)
                machine_info_clear(&machine_infos[i]);

        free(machine_infos);
}

static int compare_machine_info(const void *a, const void *b) {
        const struct machine_info *u = a, *v = b;

        if (u->is_host != v->is_host)
                return u->is_host > v->is_host ? -1 : 1;

        return strcasecmp(u->name, v->name);
}

static int get_machine_properties(sd_bus *bus, struct machine_info *mi) {
        _cleanup_(sd_bus_flush_close_unrefp) sd_bus *container = NULL;
        int r;

        assert(mi);

        if (!bus) {
                r = sd_bus_open_system_machine(&container, mi->name);
                if (r < 0)
                        return r;

                bus = container;
        }

        r = bus_map_all_properties(bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1", machine_info_property_map, NULL, mi);
        if (r < 0)
                return r;

        return 0;
}

static bool output_show_machine(const char *name, char **patterns) {
        return strv_fnmatch_or_empty(patterns, name, FNM_NOESCAPE);
}

static int get_machine_list(
                sd_bus *bus,
                struct machine_info **_machine_infos,
                char **patterns) {

        struct machine_info *machine_infos = NULL;
        _cleanup_strv_free_ char **m = NULL;
        _cleanup_free_ char *hn = NULL;
        size_t sz = 0;
        char **i;
        int c = 0, r;

        hn = gethostname_malloc();
        if (!hn)
                return log_oom();

        if (output_show_machine(hn, patterns)) {
                if (!GREEDY_REALLOC0(machine_infos, sz, c+1))
                        return log_oom();

                machine_infos[c].is_host = true;
                machine_infos[c].name = hn;
                hn = NULL;

                (void) get_machine_properties(bus, &machine_infos[c]);
                c++;
        }

        r = sd_get_machine_names(&m);
        if (r < 0)
                return log_error_errno(r, "Failed to get machine list: %m");

        STRV_FOREACH(i, m) {
                _cleanup_free_ char *class = NULL;

                if (!output_show_machine(*i, patterns))
                        continue;

                sd_machine_get_class(*i, &class);
                if (!streq_ptr(class, "container"))
                        continue;

                if (!GREEDY_REALLOC0(machine_infos, sz, c+1)) {
                        free_machines_list(machine_infos, c);
                        return log_oom();
                }

                machine_infos[c].is_host = false;
                machine_infos[c].name = strdup(*i);
                if (!machine_infos[c].name) {
                        free_machines_list(machine_infos, c);
                        return log_oom();
                }

                (void) get_machine_properties(NULL, &machine_infos[c]);
                c++;
        }

        *_machine_infos = machine_infos;
        return c;
}

static void output_machines_list(struct machine_info *machine_infos, unsigned n) {
        struct machine_info *m;
        unsigned
                circle_len = 0,
                namelen = STRLEN("NAME"),
                statelen = STRLEN("STATE"),
                failedlen = STRLEN("FAILED"),
                jobslen = STRLEN("JOBS");

        assert(machine_infos || n == 0);

        for (m = machine_infos; m < machine_infos + n; m++) {
                namelen = MAX(namelen,
                              strlen(m->name) + (m->is_host ? STRLEN(" (host)") : 0));
                statelen = MAX(statelen, strlen_ptr(m->state));
                failedlen = MAX(failedlen, DECIMAL_STR_WIDTH(m->n_failed_units));
                jobslen = MAX(jobslen, DECIMAL_STR_WIDTH(m->n_jobs));

                if (!arg_plain && !streq_ptr(m->state, "running"))
                        circle_len = 2;
        }

        if (!arg_no_legend) {
                if (circle_len > 0)
                        fputs("  ", stdout);

                printf("%-*s %-*s %-*s %-*s\n",
                         namelen, "NAME",
                        statelen, "STATE",
                       failedlen, "FAILED",
                         jobslen, "JOBS");
        }

        for (m = machine_infos; m < machine_infos + n; m++) {
                const char *on_state = "", *off_state = "";
                const char *on_failed = "", *off_failed = "";
                bool circle = false;

                if (streq_ptr(m->state, "degraded")) {
                        on_state = ansi_highlight_red();
                        off_state = ansi_normal();
                        circle = true;
                } else if (!streq_ptr(m->state, "running")) {
                        on_state = ansi_highlight_yellow();
                        off_state = ansi_normal();
                        circle = true;
                }

                if (m->n_failed_units > 0) {
                        on_failed = ansi_highlight_red();
                        off_failed = ansi_normal();
                } else
                        on_failed = off_failed = "";

                if (circle_len > 0)
                        printf("%s%s%s ", on_state, circle ? special_glyph(BLACK_CIRCLE) : " ", off_state);

                if (m->is_host)
                        printf("%-*s (host) %s%-*s%s %s%*" PRIu32 "%s %*" PRIu32 "\n",
                               (int) (namelen - (STRLEN(" (host)"))),
                               strna(m->name),
                               on_state, statelen, strna(m->state), off_state,
                               on_failed, failedlen, m->n_failed_units, off_failed,
                               jobslen, m->n_jobs);
                else
                        printf("%-*s %s%-*s%s %s%*" PRIu32 "%s %*" PRIu32 "\n",
                               namelen, strna(m->name),
                               on_state, statelen, strna(m->state), off_state,
                               on_failed, failedlen, m->n_failed_units, off_failed,
                               jobslen, m->n_jobs);
        }

        if (!arg_no_legend)
                printf("\n%u machines listed.\n", n);
}

static int list_machines(int argc, char *argv[], void *userdata) {
        struct machine_info *machine_infos = NULL;
        sd_bus *bus;
        int r;

        r = acquire_bus(BUS_MANAGER, &bus);
        if (r < 0)
                return r;

        r = get_machine_list(bus, &machine_infos, strv_skip(argv, 1));
        if (r < 0)
                return r;

        pager_open(arg_no_pager, false);

        qsort_safe(machine_infos, r, sizeof(struct machine_info), compare_machine_info);
        output_machines_list(machine_infos, r);
        free_machines_list(machine_infos, r);

        return 0;
}

static int get_default(int argc, char *argv[], void *userdata) {
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
        _cleanup_free_ char *_path = NULL;
        const char *path;
        int r;

        if (install_client_side()) {
                r = unit_file_get_default(arg_scope, arg_root, &_path);
                if (r < 0)
                        return log_error_errno(r, "Failed to get default target: %m");
                path = _path;

                r = 0;
        } else {
                _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
                sd_bus *bus;

                r = acquire_bus(BUS_MANAGER, &bus);
                if (r < 0)
                        return r;

                r = sd_bus_call_method(
                                bus,
                                "org.freedesktop.systemd1",
                                "/org/freedesktop/systemd1",
                                "org.freedesktop.systemd1.Manager",
                                "GetDefaultTarget",
                                &error,
                                &reply,
                                NULL);
                if (r < 0)
                        return log_error_errno(r, "Failed to get default target: %s", bus_error_message(&error, r));

                r = sd_bus_message_read(reply, "s", &path);
                if (r < 0)
                        return bus_log_parse_error(r);
        }

        if (path)
                printf("%s\n", path);

        return 0;
}

static int set_default(int argc, char *argv[], void *userdata) {
        _cleanup_free_ char *unit = NULL;
        UnitFileChange *changes = NULL;
        unsigned n_changes = 0;
        int r;

        assert(argc >= 2);
        assert(argv);

        r = unit_name_mangle_with_suffix(argv[1], UNIT_NAME_NOGLOB, ".target", &unit);
        if (r < 0)
                return log_error_errno(r, "Failed to mangle unit name: %m");

        if (install_client_side()) {
                r = unit_file_set_default(arg_scope, UNIT_FILE_FORCE, arg_root, unit, &changes, &n_changes);
                unit_file_dump_changes(r, "set default", changes, n_changes, arg_quiet);

                if (r > 0)
                        r = 0;
        } else {
                _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
                _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
                sd_bus *bus;

                polkit_agent_open_maybe();

                r = acquire_bus(BUS_MANAGER, &bus);
                if (r < 0)
                        return r;

                r = sd_bus_call_method(
                                bus,
                                "org.freedesktop.systemd1",
                                "/org/freedesktop/systemd1",
                                "org.freedesktop.systemd1.Manager",
                                "SetDefaultTarget",
                                &error,
                                &reply,
                                "sb", unit, 1);
                if (r < 0)
                        return log_error_errno(r, "Failed to set default target: %s", bus_error_message(&error, r));

                r = bus_deserialize_and_dump_unit_file_changes(reply, arg_quiet, &changes, &n_changes);
                if (r < 0)
                        goto finish;

                /* Try to reload if enabled */
                if (!arg_no_reload)
                        r = daemon_reload(argc, argv, userdata);
                else
                        r = 0;
        }

finish:
        unit_file_changes_free(changes, n_changes);

        return r;
}

static int output_waiting_jobs(sd_bus *bus, uint32_t id, const char *method, const char *prefix) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
        const char *name, *type, *state, *job_path, *unit_path;
        uint32_t other_id;
        int r;

        assert(bus);

        r = sd_bus_call_method(
                        bus,
                        "org.freedesktop.systemd1",
                        "/org/freedesktop/systemd1",
                        "org.freedesktop.systemd1.Manager",
                        method,
                        &error,
                        &reply,
                        "u", id);
        if (r < 0)
                return log_debug_errno(r, "Failed to get waiting jobs for job %" PRIu32, id);

        r = sd_bus_message_enter_container(reply, 'a', "(usssoo)");
        if (r < 0)
                return bus_log_parse_error(r);

        while ((r = sd_bus_message_read(reply, "(usssoo)", &other_id, &name, &type, &state, &job_path, &unit_path)) > 0)
                printf("%s %u (%s/%s)\n", prefix, other_id, name, type);
        if (r < 0)
                return bus_log_parse_error(r);

        r = sd_bus_message_exit_container(reply);
        if (r < 0)
                return bus_log_parse_error(r);

        return 0;
}

struct job_info {
        uint32_t id;
        const char *name, *type, *state;
};

static void output_jobs_list(sd_bus *bus, const struct job_info* jobs, unsigned n, bool skipped) {
        unsigned id_len, unit_len, type_len, state_len;
        const struct job_info *j;
        const char *on, *off;
        bool shorten = false;

        assert(n == 0 || jobs);

        if (n == 0) {
                if (!arg_no_legend) {
                        on = ansi_highlight_green();
                        off = ansi_normal();

                        printf("%sNo jobs %s.%s\n", on, skipped ? "listed" : "running", off);
                }
                return;
        }

        pager_open(arg_no_pager, false);

        id_len = STRLEN("JOB");
        unit_len = STRLEN("UNIT");
        type_len = STRLEN("TYPE");
        state_len = STRLEN("STATE");

        for (j = jobs; j < jobs + n; j++) {
                uint32_t id = j->id;
                assert(j->name && j->type && j->state);

                id_len = MAX(id_len, DECIMAL_STR_WIDTH(id));
                unit_len = MAX(unit_len, strlen(j->name));
                type_len = MAX(type_len, strlen(j->type));
                state_len = MAX(state_len, strlen(j->state));
        }

        if (!arg_full && id_len + 1 + unit_len + type_len + 1 + state_len > columns()) {
                unit_len = MAX(33u, columns() - id_len - type_len - state_len - 3);
                shorten = true;
        }

        if (!arg_no_legend)
                printf("%*s %-*s %-*s %-*s\n",
                       id_len, "JOB",
                       unit_len, "UNIT",
                       type_len, "TYPE",
                       state_len, "STATE");

        for (j = jobs; j < jobs + n; j++) {
                _cleanup_free_ char *e = NULL;

                if (streq(j->state, "running")) {
                        on = ansi_highlight();
                        off = ansi_normal();
                } else
                        on = off = "";

                e = shorten ? ellipsize(j->name, unit_len, 33) : NULL;
                printf("%*u %s%-*s%s %-*s %s%-*s%s\n",
                       id_len, j->id,
                       on, unit_len, e ? e : j->name, off,
                       type_len, j->type,
                       on, state_len, j->state, off);

                if (arg_jobs_after)
                        output_waiting_jobs(bus, j->id, "GetJobAfter", "\twaiting for job");
                if (arg_jobs_before)
                        output_waiting_jobs(bus, j->id, "GetJobBefore", "\tblocking job");
        }

        if (!arg_no_legend) {
                on = ansi_highlight();
                off = ansi_normal();

                printf("\n%s%u jobs listed%s.\n", on, n, off);
        }
}

static bool output_show_job(struct job_info *job, char **patterns) {
        return strv_fnmatch_or_empty(patterns, job->name, FNM_NOESCAPE);
}

static int list_jobs(int argc, char *argv[], void *userdata) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
        const char *name, *type, *state, *job_path, *unit_path;
        _cleanup_free_ struct job_info *jobs = NULL;
        size_t size = 0;
        unsigned c = 0;
        sd_bus *bus;
        uint32_t id;
        int r;
        bool skipped = false;

        r = acquire_bus(BUS_MANAGER, &bus);
        if (r < 0)
                return r;

        r = sd_bus_call_method(
                        bus,
                        "org.freedesktop.systemd1",
                        "/org/freedesktop/systemd1",
                        "org.freedesktop.systemd1.Manager",
                        "ListJobs",
                        &error,
                        &reply,
                        NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to list jobs: %s", bus_error_message(&error, r));

        r = sd_bus_message_enter_container(reply, 'a', "(usssoo)");
        if (r < 0)
                return bus_log_parse_error(r);

        while ((r = sd_bus_message_read(reply, "(usssoo)", &id, &name, &type, &state, &job_path, &unit_path)) > 0) {
                struct job_info job = { id, name, type, state };

                if (!output_show_job(&job, strv_skip(argv, 1))) {
                        skipped = true;
                        continue;
                }

                if (!GREEDY_REALLOC(jobs, size, c + 1))
                        return log_oom();

                jobs[c++] = job;
        }
        if (r < 0)
                return bus_log_parse_error(r);

        r = sd_bus_message_exit_container(reply);
        if (r < 0)
                return bus_log_parse_error(r);

        pager_open(arg_no_pager, false);

        output_jobs_list(bus, jobs, c, skipped);
        return 0;
}

static int cancel_job(int argc, char *argv[], void *userdata) {
        sd_bus *bus;
        char **name;
        int r = 0;

        if (argc <= 1)
                return trivial_method(argc, argv, userdata);

        r = acquire_bus(BUS_MANAGER, &bus);
        if (r < 0)
                return r;

        polkit_agent_open_maybe();

        STRV_FOREACH(name, strv_skip(argv, 1)) {
                _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
                uint32_t id;
                int q;

                q = safe_atou32(*name, &id);
                if (q < 0)
                        return log_error_errno(q, "Failed to parse job id \"%s\": %m", *name);

                q = sd_bus_call_method(
                                bus,
                                "org.freedesktop.systemd1",
                                "/org/freedesktop/systemd1",
                                "org.freedesktop.systemd1.Manager",
                                "CancelJob",
                                &error,
                                NULL,
                                "u", id);
                if (q < 0) {
                        log_error_errno(q, "Failed to cancel job %"PRIu32": %s", id, bus_error_message(&error, q));
                        if (r == 0)
                                r = q;
                }
        }

        return r;
}

static int need_daemon_reload(sd_bus *bus, const char *unit) {
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
        const char *path;
        int b, r;

        /* We ignore all errors here, since this is used to show a
         * warning only */

        /* We don't use unit_dbus_path_from_name() directly since we
         * don't want to load the unit if it isn't loaded. */

        r = sd_bus_call_method(
                        bus,
                        "org.freedesktop.systemd1",
                        "/org/freedesktop/systemd1",
                        "org.freedesktop.systemd1.Manager",
                        "GetUnit",
                        NULL,
                        &reply,
                        "s", unit);
        if (r < 0)
                return r;

        r = sd_bus_message_read(reply, "o", &path);
        if (r < 0)
                return r;

        r = sd_bus_get_property_trivial(
                        bus,
                        "org.freedesktop.systemd1",
                        path,
                        "org.freedesktop.systemd1.Unit",
                        "NeedDaemonReload",
                        NULL,
                        'b', &b);
        if (r < 0)
                return r;

        return b;
}

static void warn_unit_file_changed(const char *name) {
        assert(name);

        log_warning("%sWarning:%s The unit file, source configuration file or drop-ins of %s changed on disk. Run 'systemctl%s daemon-reload' to reload units.",
                    ansi_highlight_red(),
                    ansi_normal(),
                    name,
                    arg_scope == UNIT_FILE_SYSTEM ? "" : " --user");
}

static int unit_file_find_path(LookupPaths *lp, const char *unit_name, char **unit_path) {
        char **p;

        assert(lp);
        assert(unit_name);

        STRV_FOREACH(p, lp->search_path) {
                _cleanup_free_ char *path = NULL, *lpath = NULL;
                int r;

                path = path_join(NULL, *p, unit_name);
                if (!path)
                        return log_oom();

                r = chase_symlinks(path, arg_root, 0, &lpath);
                if (r == -ENOENT)
                        continue;
                if (r == -ENOMEM)
                        return log_oom();
                if (r < 0)
                        return log_error_errno(r, "Failed to access path '%s': %m", path);

                if (unit_path) {
                        *unit_path = lpath;
                        lpath = NULL;
                }
                return 1;
        }

        return 0;
}

static int unit_find_template_path(
                const char *unit_name,
                LookupPaths *lp,
                char **fragment_path,
                char **template) {

        _cleanup_free_ char *_template = NULL;
        int r;

        /* Returns 1 if a fragment was found, 0 if not found, negative on error. */

        r = unit_file_find_path(lp, unit_name, fragment_path);
        if (r != 0)
                return r; /* error or found a real unit */

        r = unit_name_template(unit_name, &_template);
        if (r == -EINVAL)
                return 0; /* not a template, does not exist */
        if (r < 0)
                return log_error_errno(r, "Failed to determine template name: %m");

        r = unit_file_find_path(lp, _template, fragment_path);
        if (r < 0)
                return r;

        if (template) {
                *template = _template;
                _template = NULL;
        }
        return r;
}

static int unit_find_paths(
                sd_bus *bus,
                const char *unit_name,
                LookupPaths *lp,
                char **fragment_path,
                char ***dropin_paths) {

        _cleanup_free_ char *path = NULL;
        _cleanup_strv_free_ char **dropins = NULL;
        int r;

        /**
         * Finds where the unit is defined on disk. Returns 0 if the unit
         * is not found. Returns 1 if it is found, and sets
         * - the path to the unit in *path, if it exists on disk,
         * - and a strv of existing drop-ins in *dropins,
         *   if the arg is not NULL and any dropins were found.
         */

        assert(unit_name);
        assert(fragment_path);
        assert(lp);

        if (!install_client_side() && !unit_name_is_valid(unit_name, UNIT_NAME_TEMPLATE)) {
                _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
                _cleanup_free_ char *unit = NULL;

                unit = unit_dbus_path_from_name(unit_name);
                if (!unit)
                        return log_oom();

                r = sd_bus_get_property_string(
                                bus,
                                "org.freedesktop.systemd1",
                                unit,
                                "org.freedesktop.systemd1.Unit",
                                "FragmentPath",
                                &error,
                                &path);
                if (r < 0)
                        return log_error_errno(r, "Failed to get FragmentPath: %s", bus_error_message(&error, r));

                if (dropin_paths) {
                        r = sd_bus_get_property_strv(
                                        bus,
                                        "org.freedesktop.systemd1",
                                        unit,
                                        "org.freedesktop.systemd1.Unit",
                                        "DropInPaths",
                                        &error,
                                        &dropins);
                        if (r < 0)
                                return log_error_errno(r, "Failed to get DropInPaths: %s", bus_error_message(&error, r));
                }
        } else {
                _cleanup_set_free_ Set *names = NULL;
                _cleanup_free_ char *template = NULL;

                names = set_new(NULL);
                if (!names)
                        return log_oom();

                r = unit_find_template_path(unit_name, lp, &path, &template);
                if (r < 0)
                        return r;

                if (r > 0)
                        /* We found the unit file. If we followed symlinks, this name might be
                         * different then the unit_name with started with. Look for dropins matching
                         * that "final" name. */
                        r = set_put(names, basename(path));
                else if (!template)
                        /* No unit file, let's look for dropins matching the original name.
                         * systemd has fairly complicated rules (based on unit type and provenience),
                         * which units are allowed not to have the main unit file. We err on the
                         * side of including too many files, and always try to load dropins. */
                        r = set_put(names, unit_name);
                else
                        /* The cases where we allow a unit to exist without the main file are
                         * never valid for templates. Don't try to load dropins in this case. */
                        goto not_found;

                if (r < 0)
                        return log_error_errno(r, "Failed to add unit name: %m");

                if (dropin_paths) {
                        r = unit_file_find_dropin_conf_paths(arg_root, lp->search_path,
                                                             NULL, names, &dropins);
                        if (r < 0)
                                return r;
                }
        }

        r = 0;

        if (!isempty(path)) {
                *fragment_path = path;
                path = NULL;
                r = 1;
        }

        if (dropin_paths && !strv_isempty(dropins)) {
                *dropin_paths = dropins;
                dropins = NULL;
                r = 1;
        }
 not_found:
        if (r == 0 && !arg_force)
                log_error("No files found for %s.", unit_name);

        return r;
}

static int get_state_one_unit(sd_bus *bus, const char *name, UnitActiveState *active_state) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_free_ char *buf = NULL, *path = NULL;
        UnitActiveState state;
        int r;

        assert(name);
        assert(active_state);

        path = unit_dbus_path_from_name(name);
        if (!path)
                return log_oom();

        r = sd_bus_get_property_string(
                        bus,
                        "org.freedesktop.systemd1",
                        path,
                        "org.freedesktop.systemd1.Unit",
                        "ActiveState",
                        &error,
                        &buf);
        if (r < 0)
                return log_error_errno(r, "Failed to retrieve unit state: %s", bus_error_message(&error, r));

        state = unit_active_state_from_string(buf);
        if (state == _UNIT_ACTIVE_STATE_INVALID) {
                log_error("Invalid unit state '%s' for: %s", buf, name);
                return -EINVAL;
        }

        *active_state = state;
        return 0;
}

static int check_triggering_units(
                sd_bus *bus,
                const char *name) {

        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_free_ char *path = NULL, *n = NULL, *load_state = NULL;
        _cleanup_strv_free_ char **triggered_by = NULL;
        bool print_warning_label = true;
        UnitActiveState active_state;
        char **i;
        int r;

        r = unit_name_mangle(name, UNIT_NAME_NOGLOB, &n);
        if (r < 0)
                return log_error_errno(r, "Failed to mangle unit name: %m");

        path = unit_dbus_path_from_name(n);
        if (!path)
                return log_oom();

        r = sd_bus_get_property_string(
                        bus,
                        "org.freedesktop.systemd1",
                        path,
                        "org.freedesktop.systemd1.Unit",
                        "LoadState",
                        &error,
                        &load_state);
        if (r < 0)
                return log_error_errno(r, "Failed to get load state of %s: %s", n, bus_error_message(&error, r));

        if (streq(load_state, "masked"))
                return 0;

        r = sd_bus_get_property_strv(
                        bus,
                        "org.freedesktop.systemd1",
                        path,
                        "org.freedesktop.systemd1.Unit",
                        "TriggeredBy",
                        &error,
                        &triggered_by);
        if (r < 0)
                return log_error_errno(r, "Failed to get triggered by array of %s: %s", n, bus_error_message(&error, r));

        STRV_FOREACH(i, triggered_by) {
                r = get_state_one_unit(bus, *i, &active_state);
                if (r < 0)
                        return r;

                if (!IN_SET(active_state, UNIT_ACTIVE, UNIT_RELOADING))
                        continue;

                if (print_warning_label) {
                        log_warning("Warning: Stopping %s, but it can still be activated by:", n);
                        print_warning_label = false;
                }

                log_warning("  %s", *i);
        }

        return 0;
}

static const struct {
        const char *verb;
        const char *method;
} unit_actions[] = {
        { "start",                 "StartUnit" },
        { "stop",                  "StopUnit" },
        { "condstop",              "StopUnit" },
        { "reload",                "ReloadUnit" },
        { "restart",               "RestartUnit" },
        { "try-restart",           "TryRestartUnit" },
        { "condrestart",           "TryRestartUnit" },
        { "reload-or-restart",     "ReloadOrRestartUnit" },
        { "try-reload-or-restart", "ReloadOrTryRestartUnit" },
        { "reload-or-try-restart", "ReloadOrTryRestartUnit" },
        { "condreload",            "ReloadOrTryRestartUnit" },
        { "force-reload",          "ReloadOrTryRestartUnit" }
};

static const char *verb_to_method(const char *verb) {
       uint i;

       for (i = 0; i < ELEMENTSOF(unit_actions); i++)
                if (streq_ptr(unit_actions[i].verb, verb))
                        return unit_actions[i].method;

       return "StartUnit";
}

static const char *method_to_verb(const char *method) {
       uint i;

       for (i = 0; i < ELEMENTSOF(unit_actions); i++)
                if (streq_ptr(unit_actions[i].method, method))
                        return unit_actions[i].verb;

       return "n/a";
}

typedef struct {
        sd_bus_slot *match;
        sd_event *event;
        Set *unit_paths;
        bool any_failed;
} WaitContext;

static void wait_context_free(WaitContext *c) {
        c->match = sd_bus_slot_unref(c->match);
        c->event = sd_event_unref(c->event);
        c->unit_paths = set_free_free(c->unit_paths);
}

static int on_properties_changed(sd_bus_message *m, void *userdata, sd_bus_error *error) {
        WaitContext *c = userdata;
        const char *path;
        int r;

        path = sd_bus_message_get_path(m);
        if (!set_contains(c->unit_paths, path))
                return 0;

        /* Check if ActiveState changed to inactive/failed */
        /* (s interface, a{sv} changed_properties, as invalidated_properties) */
        r = sd_bus_message_skip(m, "s");
        if (r < 0)
                return bus_log_parse_error(r);

        r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "{sv}");
        if (r < 0)
                return bus_log_parse_error(r);

        while ((r = sd_bus_message_enter_container(m, SD_BUS_TYPE_DICT_ENTRY, "sv")) > 0) {
                const char *s;

                r = sd_bus_message_read(m, "s", &s);
                if (r < 0)
                        return bus_log_parse_error(r);

                if (streq(s, "ActiveState")) {
                        bool is_failed;

                        r = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "s");
                        if (r < 0)
                                return bus_log_parse_error(r);

                        r = sd_bus_message_read(m, "s", &s);
                        if (r < 0)
                                return bus_log_parse_error(r);

                        is_failed = streq(s, "failed");
                        if (streq(s, "inactive") || is_failed) {
                                log_debug("%s became %s, dropping from --wait tracking", path, s);
                                free(set_remove(c->unit_paths, path));
                                c->any_failed = c->any_failed || is_failed;
                        } else
                                log_debug("ActiveState on %s changed to %s", path, s);

                        break; /* no need to dissect the rest of the message */
                } else {
                        /* other property */
                        r = sd_bus_message_skip(m, "v");
                        if (r < 0)
                                return bus_log_parse_error(r);
                }
                r = sd_bus_message_exit_container(m);
                if (r < 0)
                        return bus_log_parse_error(r);
        }
        if (r < 0)
                return bus_log_parse_error(r);

        if (set_isempty(c->unit_paths))
                sd_event_exit(c->event, EXIT_SUCCESS);

        return 0;
}

static int start_unit_one(
                sd_bus *bus,
                const char *method,
                const char *name,
                const char *mode,
                sd_bus_error *error,
                BusWaitForJobs *w,
                WaitContext *wait_context) {

        _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
        const char *path;
        int r;

        assert(method);
        assert(name);
        assert(mode);
        assert(error);

        if (wait_context) {
                _cleanup_free_ char *unit_path = NULL;

                log_debug("Watching for property changes of %s", name);
                r = sd_bus_call_method(
                                bus,
                                "org.freedesktop.systemd1",
                                "/org/freedesktop/systemd1",
                                "org.freedesktop.systemd1.Manager",
                                "RefUnit",
                                error,
                                NULL,
                                "s", name);
                if (r < 0)
                        return log_error_errno(r, "Failed to RefUnit %s: %s", name, bus_error_message(error, r));

                unit_path = unit_dbus_path_from_name(name);
                if (!unit_path)
                        return log_oom();

                r = set_put_strdup(wait_context->unit_paths, unit_path);
                if (r < 0)
                        return log_error_errno(r, "Failed to add unit path %s to set: %m", unit_path);

                r = sd_bus_match_signal_async(bus,
                                              &wait_context->match,
                                              NULL,
                                              unit_path,
                                              "org.freedesktop.DBus.Properties",
                                              "PropertiesChanged",
                                              on_properties_changed, NULL, wait_context);
                if (r < 0)
                        return log_error_errno(r, "Failed to request match for PropertiesChanged signal: %m");
        }

        log_debug("%s dbus call org.freedesktop.systemd1.Manager %s(%s, %s)",
                  arg_dry_run ? "Would execute" : "Executing",
                  method, name, mode);
        if (arg_dry_run)
                return 0;

        r = sd_bus_call_method(
                        bus,
                        "org.freedesktop.systemd1",
                        "/org/freedesktop/systemd1",
                        "org.freedesktop.systemd1.Manager",
                        method,
                        error,
                        &reply,
                        "ss", name, mode);
        if (r < 0) {
                const char *verb;

                /* There's always a fallback possible for legacy actions. */
                if (arg_action != ACTION_SYSTEMCTL)
                        return r;

                verb = method_to_verb(method);

                log_error("Failed to %s %s: %s", verb, name, bus_error_message(error, r));

                if (!sd_bus_error_has_name(error, BUS_ERROR_NO_SUCH_UNIT) &&
                    !sd_bus_error_has_name(error, BUS_ERROR_UNIT_MASKED) &&
                    !sd_bus_error_has_name(error, BUS_ERROR_JOB_TYPE_NOT_APPLICABLE))
                        log_error("See %s logs and 'systemctl%s status%s %s' for details.",
                                   arg_scope == UNIT_FILE_SYSTEM ? "system" : "user",
                                   arg_scope == UNIT_FILE_SYSTEM ? "" : " --user",
                                   name[0] == '-' ? " --" : "",
                                   name);

                return r;
        }

        r = sd_bus_message_read(reply, "o", &path);
        if (r < 0)
                return bus_log_parse_error(r);

        if (need_daemon_reload(bus, name) > 0)
                warn_unit_file_changed(name);

        if (w) {
                log_debug("Adding %s to the set", path);
                r = bus_wait_for_jobs_add(w, path);
                if (r < 0)
                        return log_oom();
        }

        return 0;
}

static int expand_names(sd_bus *bus, char **names, const char* suffix, char ***ret) {
        _cleanup_strv_free_ char **mangled = NULL, **globs = NULL;
        char **name;
        int r, i;

        assert(bus);
        assert(ret);

        STRV_FOREACH(name, names) {
                char *t;

                if (suffix)
                        r = unit_name_mangle_with_suffix(*name, UNIT_NAME_GLOB, suffix, &t);
                else
                        r = unit_name_mangle(*name, UNIT_NAME_GLOB, &t);
                if (r < 0)
                        return log_error_errno(r, "Failed to mangle name: %m");

                if (string_is_glob(t))
                        r = strv_consume(&globs, t);
                else
                        r = strv_consume(&mangled, t);
                if (r < 0)
                        return log_oom();
        }

        /* Query the manager only if any of the names are a glob, since
         * this is fairly expensive */
        if (!strv_isempty(globs)) {
                _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
                _cleanup_free_ UnitInfo *unit_infos = NULL;
                size_t allocated, n;

                r = get_unit_list(bus, NULL, globs, &unit_infos, 0, &reply);
                if (r < 0)
                        return r;

                n = strv_length(mangled);
                allocated = n + 1;

                for (i = 0; i < r; i++) {
                        if (!GREEDY_REALLOC(mangled, allocated, n+2))
                                return log_oom();

                        mangled[n] = strdup(unit_infos[i].id);
                        if (!mangled[n])
                                return log_oom();

                        mangled[++n] = NULL;
                }
        }

        *ret = mangled;
        mangled = NULL; /* do not free */

        return 0;
}

static const struct {
        const char *target;
        const char *verb;
        const char *mode;
} action_table[_ACTION_MAX] = {
        [ACTION_HALT]         = { SPECIAL_HALT_TARGET,         "halt",         "replace-irreversibly" },
        [ACTION_POWEROFF]     = { SPECIAL_POWEROFF_TARGET,     "poweroff",     "replace-irreversibly" },
        [ACTION_REBOOT]       = { SPECIAL_REBOOT_TARGET,       "reboot",       "replace-irreversibly" },
        [ACTION_KEXEC]        = { SPECIAL_KEXEC_TARGET,        "kexec",        "replace-irreversibly" },
        [ACTION_RUNLEVEL2]    = { SPECIAL_MULTI_USER_TARGET,   NULL,           "isolate" },
        [ACTION_RUNLEVEL3]    = { SPECIAL_MULTI_USER_TARGET,   NULL,           "isolate" },
        [ACTION_RUNLEVEL4]    = { SPECIAL_MULTI_USER_TARGET,   NULL,           "isolate" },
        [ACTION_RUNLEVEL5]    = { SPECIAL_GRAPHICAL_TARGET,    NULL,           "isolate" },
        [ACTION_RESCUE]       = { SPECIAL_RESCUE_TARGET,       "rescue",       "isolate" },
        [ACTION_EMERGENCY]    = { SPECIAL_EMERGENCY_TARGET,    "emergency",    "isolate" },
        [ACTION_DEFAULT]      = { SPECIAL_DEFAULT_TARGET,      "default",      "isolate" },
        [ACTION_EXIT]         = { SPECIAL_EXIT_TARGET,         "exit",         "replace-irreversibly" },
        [ACTION_SUSPEND]      = { SPECIAL_SUSPEND_TARGET,      "suspend",      "replace-irreversibly" },
        [ACTION_HIBERNATE]    = { SPECIAL_HIBERNATE_TARGET,    "hibernate",    "replace-irreversibly" },
        [ACTION_HYBRID_SLEEP] = { SPECIAL_HYBRID_SLEEP_TARGET, "hybrid-sleep", "replace-irreversibly" },
};

static enum action verb_to_action(const char *verb) {
        enum action i;

        for (i = 0; i < _ACTION_MAX; i++)
                if (streq_ptr(action_table[i].verb, verb))
                        return i;

        return _ACTION_INVALID;
}

static int start_unit(int argc, char *argv[], void *userdata) {
        _cleanup_(bus_wait_for_jobs_freep) BusWaitForJobs *w = NULL;
        const char *method, *mode, *one_name, *suffix = NULL;
        _cleanup_strv_free_ char **names = NULL;
        sd_bus *bus;
        _cleanup_(wait_context_free) WaitContext wait_context = {};
        char **name;
        int r = 0;

        if (arg_wait && !STR_IN_SET(argv[0], "start", "restart")) {
                log_error("--wait may only be used with the 'start' or 'restart' commands.");
                return -EINVAL;
        }

        /* we cannot do sender tracking on the private bus, so we need the full
         * one for RefUnit to implement --wait */
        r = acquire_bus(arg_wait ? BUS_FULL : BUS_MANAGER, &bus);
        if (r < 0)
                return r;

        ask_password_agent_open_if_enabled();
        polkit_agent_open_maybe();

        if (arg_action == ACTION_SYSTEMCTL) {
                enum action action;

                action = verb_to_action(argv[0]);

                if (action != _ACTION_INVALID) {
                        method = "StartUnit";
                        mode = action_table[action].mode;
                        one_name = action_table[action].target;
                } else {
                        if (streq(argv[0], "isolate")) {
                                method = "StartUnit";
                                mode = "isolate";

                                suffix = ".target";
                        } else {
                                method = verb_to_method(argv[0]);
                                mode = arg_job_mode;
                        }
                        one_name = NULL;
                }
        } else {
                assert(arg_action >= 0 && arg_action < _ACTION_MAX);
                assert(action_table[arg_action].target);
                assert(action_table[arg_action].mode);

                method = "StartUnit";
                mode = action_table[arg_action].mode;
                one_name = action_table[arg_action].target;
        }

        if (one_name)
                names = strv_new(one_name, NULL);
        else {
                r = expand_names(bus, strv_skip(argv, 1), suffix, &names);
                if (r < 0)
                        return log_error_errno(r, "Failed to expand names: %m");
        }

        if (!arg_no_block) {
                r = bus_wait_for_jobs_new(bus, &w);
                if (r < 0)
                        return log_error_errno(r, "Could not watch jobs: %m");
        }

        if (arg_wait) {
                wait_context.unit_paths = set_new(&string_hash_ops);
                if (!wait_context.unit_paths)
                        return log_oom();

                r = sd_bus_call_method_async(
                                bus,
                                NULL,
                                "org.freedesktop.systemd1",
                                "/org/freedesktop/systemd1",
                                "org.freedesktop.systemd1.Manager",
                                "Subscribe",
                                NULL, NULL,
                                NULL);
                if (r < 0)
                        return log_error_errno(r, "Failed to enable subscription: %m");
                r = sd_event_default(&wait_context.event);
                if (r < 0)
                        return log_error_errno(r, "Failed to allocate event loop: %m");
                r = sd_bus_attach_event(bus, wait_context.event, 0);
                if (r < 0)
                        return log_error_errno(r, "Failed to attach bus to event loop: %m");
        }

        STRV_FOREACH(name, names) {
                _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
                int q;

                q = start_unit_one(bus, method, *name, mode, &error, w, arg_wait ? &wait_context : NULL);
                if (r >= 0 && q < 0)
                        r = translate_bus_error_to_exit_status(q, &error);
        }

        if (!arg_no_block) {
                int q, arg_count = 0;
                const char* extra_args[4] = {};

                if (arg_scope != UNIT_FILE_SYSTEM)
                        extra_args[arg_count++] = "--user";

                assert(IN_SET(arg_transport, BUS_TRANSPORT_LOCAL, BUS_TRANSPORT_REMOTE, BUS_TRANSPORT_MACHINE));
                if (arg_transport == BUS_TRANSPORT_REMOTE) {
                        extra_args[arg_count++] = "-H";
                        extra_args[arg_count++] = arg_host;
                } else if (arg_transport == BUS_TRANSPORT_MACHINE) {
                        extra_args[arg_count++] = "-M";
                        extra_args[arg_count++] = arg_host;
                }

                q = bus_wait_for_jobs(w, arg_quiet, extra_args);
                if (q < 0)
                        return q;

                /* When stopping units, warn if they can still be triggered by
                 * another active unit (socket, path, timer) */
                if (!arg_quiet && streq(method, "StopUnit"))
                        STRV_FOREACH(name, names)
                                check_triggering_units(bus, *name);
        }

        if (r >= 0 && arg_wait) {
                int q;
                q = sd_event_loop(wait_context.event);
                if (q < 0)
                        return log_error_errno(q, "Failed to run event loop: %m");
                if (wait_context.any_failed)
                        r = EXIT_FAILURE;
        }

        return r;
}

#if ENABLE_LOGIND
static int logind_set_wall_message(void) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus *bus;
        _cleanup_free_ char *m = NULL;
        int r;

        r = acquire_bus(BUS_FULL, &bus);
        if (r < 0)
                return r;

        m = strv_join(arg_wall, " ");
        if (!m)
                return log_oom();

        log_debug("%s wall message \"%s\".", arg_dry_run ? "Would set" : "Setting", m);
        if (arg_dry_run)
                return 0;

        r = sd_bus_call_method(
                        bus,
                        "org.freedesktop.login1",
                        "/org/freedesktop/login1",
                        "org.freedesktop.login1.Manager",
                        "SetWallMessage",
                        &error,
                        NULL,
                        "sb",
                        m,
                        !arg_no_wall);

        if (r < 0)
                return log_warning_errno(r, "Failed to set wall message, ignoring: %s", bus_error_message(&error, r));
        return 0;
}
#endif

/* Ask systemd-logind, which might grant access to unprivileged users
 * through PolicyKit */
static int logind_reboot(enum action a) {
#if ENABLE_LOGIND
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        const char *method, *description;
        sd_bus *bus;
        int r;

        r = acquire_bus(BUS_FULL, &bus);
        if (r < 0)
                return r;

        switch (a) {

        case ACTION_POWEROFF:
                method = "PowerOff";
                description = "power off system";
                break;

        case ACTION_REBOOT:
                method = "Reboot";
                description = "reboot system";
                break;

        case ACTION_HALT:
                method = "Halt";
                description = "halt system";
                break;

        case ACTION_SUSPEND:
                method = "Suspend";
                description = "suspend system";
                break;

        case ACTION_HIBERNATE:
                method = "Hibernate";
                description = "hibernate system";
                break;

        case ACTION_HYBRID_SLEEP:
                method = "HybridSleep";
                description = "put system into hybrid sleep";
                break;

        default:
                return -EINVAL;
        }

        polkit_agent_open_maybe();
        (void) logind_set_wall_message();

        log_debug("%s org.freedesktop.login1.Manager %s dbus call.", arg_dry_run ? "Would execute" : "Executing", method);
        if (arg_dry_run)
                return 0;

        r = sd_bus_call_method(
                        bus,
                        "org.freedesktop.login1",
                        "/org/freedesktop/login1",
                        "org.freedesktop.login1.Manager",
                        method,
                        &error,
                        NULL,
                        "b", arg_ask_password);
        if (r < 0)
                return log_error_errno(r, "Failed to %s via logind: %s", description, bus_error_message(&error, r));

        return 0;
#else
        return -ENOSYS;
#endif
}

static int logind_check_inhibitors(enum action a) {
#if ENABLE_LOGIND
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
        _cleanup_strv_free_ char **sessions = NULL;
        const char *what, *who, *why, *mode;
        uint32_t uid, pid;
        sd_bus *bus;
        unsigned c = 0;
        char **s;
        int r;

        if (arg_ignore_inhibitors || arg_force > 0)
                return 0;

        if (arg_when > 0)
                return 0;

        if (geteuid() == 0)
                return 0;

        if (!on_tty())
                return 0;

        if (arg_transport != BUS_TRANSPORT_LOCAL)
                return 0;

        r = acquire_bus(BUS_FULL, &bus);
        if (r < 0)
                return r;

        r = sd_bus_call_method(
                        bus,
                        "org.freedesktop.login1",
                        "/org/freedesktop/login1",
                        "org.freedesktop.login1.Manager",
                        "ListInhibitors",
                        NULL,
                        &reply,
                        NULL);
        if (r < 0)
                /* If logind is not around, then there are no inhibitors... */
                return 0;

        r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "(ssssuu)");
        if (r < 0)
                return bus_log_parse_error(r);

        while ((r = sd_bus_message_read(reply, "(ssssuu)", &what, &who, &why, &mode, &uid, &pid)) > 0) {
                _cleanup_free_ char *comm = NULL, *user = NULL;
                _cleanup_strv_free_ char **sv = NULL;

                if (!streq(mode, "block"))
                        continue;

                sv = strv_split(what, ":");
                if (!sv)
                        return log_oom();

                if (!pid_is_valid((pid_t) pid)) {
                        log_error("Invalid PID "PID_FMT".", (pid_t) pid);
                        return -ERANGE;
                }

                if (!strv_contains(sv,
                                   IN_SET(a,
                                          ACTION_HALT,
                                          ACTION_POWEROFF,
                                          ACTION_REBOOT,
                                          ACTION_KEXEC) ? "shutdown" : "sleep"))
                        continue;

                get_process_comm(pid, &comm);
                user = uid_to_name(uid);

                log_warning("Operation inhibited by \"%s\" (PID "PID_FMT" \"%s\", user %s), reason is \"%s\".",
                            who, (pid_t) pid, strna(comm), strna(user), why);

                c++;
        }
        if (r < 0)
                return bus_log_parse_error(r);

        r = sd_bus_message_exit_container(reply);
        if (r < 0)
                return bus_log_parse_error(r);

        /* Check for current sessions */
        sd_get_sessions(&sessions);
        STRV_FOREACH(s, sessions) {
                _cleanup_free_ char *type = NULL, *tty = NULL, *seat = NULL, *user = NULL, *service = NULL, *class = NULL;

                if (sd_session_get_uid(*s, &uid) < 0 || uid == getuid())
                        continue;

                if (sd_session_get_class(*s, &class) < 0 || !streq(class, "user"))
                        continue;

                if (sd_session_get_type(*s, &type) < 0 || !STR_IN_SET(type, "x11", "wayland", "tty", "mir"))
                        continue;

                sd_session_get_tty(*s, &tty);
                sd_session_get_seat(*s, &seat);
                sd_session_get_service(*s, &service);
                user = uid_to_name(uid);

                log_warning("User %s is logged in on %s.", strna(user), isempty(tty) ? (isempty(seat) ? strna(service) : seat) : tty);
                c++;
        }

        if (c <= 0)
                return 0;

        log_error("Please retry operation after closing inhibitors and logging out other users.\nAlternatively, ignore inhibitors and users with 'systemctl %s -i'.",
                  action_table[a].verb);

        return -EPERM;
#else
        return 0;
#endif
}

static int logind_prepare_firmware_setup(void) {
#if ENABLE_LOGIND
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus *bus;
        int r;

        r = acquire_bus(BUS_FULL, &bus);
        if (r < 0)
                return r;

        r = sd_bus_call_method(
                        bus,
                        "org.freedesktop.login1",
                        "/org/freedesktop/login1",
                        "org.freedesktop.login1.Manager",
                        "SetRebootToFirmwareSetup",
                        &error,
                        NULL,
                        "b", true);
        if (r < 0)
                return log_error_errno(r, "Cannot indicate to EFI to boot into setup mode: %s", bus_error_message(&error, r));

        return 0;
#else
        log_error("Cannot remotely indicate to EFI to boot into setup mode.");
        return -ENOSYS;
#endif
}

static int prepare_firmware_setup(void) {
        int r;

        if (!arg_firmware_setup)
                return 0;

        if (arg_transport == BUS_TRANSPORT_LOCAL) {

                r = efi_set_reboot_to_firmware(true);
                if (r < 0)
                        log_debug_errno(r, "Cannot indicate to EFI to boot into setup mode, will retry via logind: %m");
                else
                        return r;
        }

        return logind_prepare_firmware_setup();
}

static int load_kexec_kernel(void) {
        _cleanup_(boot_config_free) BootConfig config = {};
        _cleanup_free_ char *where = NULL, *kernel = NULL, *initrd = NULL, *options = NULL;
        const BootEntry *e;
        pid_t pid;
        int r;

        if (kexec_loaded()) {
                log_debug("Kexec kernel already loaded.");
                return 0;
        }

        if (access(KEXEC, X_OK) < 0)
                return log_error_errno(errno, KEXEC" is not available: %m");

        r = find_esp_and_warn(arg_esp_path, false, &where, NULL, NULL, NULL, NULL);
        if (r == -ENOKEY) /* find_esp_and_warn() doesn't warn about this case */
                return log_error_errno(r, "Cannot find the ESP partition mount point.");
        if (r < 0) /* But it logs about all these cases, hence don't log here again */
                return r;

        r = boot_entries_load_config(where, &config);
        if (r < 0)
                return log_error_errno(r, "Failed to load bootspec config from \"%s/loader\": %m", where);

        if (config.default_entry < 0) {
                log_error("No entry suitable as default, refusing to guess.");
                return -ENOENT;
        }
        e = &config.entries[config.default_entry];

        if (strv_length(e->initrd) > 1) {
                log_error("Boot entry specifies multiple initrds, which is not supported currently.");
                return -EINVAL;
        }

        kernel = path_join(NULL, where, e->kernel);
        if (!strv_isempty(e->initrd))
                initrd = path_join(NULL, where, *e->initrd);
        options = strv_join(e->options, " ");
        if (!options)
                return log_oom();

        log_full(arg_quiet ? LOG_DEBUG : LOG_INFO,
                 "%s "KEXEC" --load \"%s\" --append \"%s\"%s%s%s",
                 arg_dry_run ? "Would run" : "Running",
                 kernel,
                 options,
                 initrd ? " --initrd \"" : NULL, strempty(initrd), initrd ? "\"" : "");
        if (arg_dry_run)
                return 0;

        r = safe_fork("(kexec)", FORK_RESET_SIGNALS|FORK_DEATHSIG|FORK_LOG, &pid);
        if (r < 0)
                return r;
        if (r == 0) {
                const char* const args[] = {
                        KEXEC,
                        "--load", kernel,
                        "--append", options,
                        initrd ? "--initrd" : NULL, initrd,
                        NULL };

                /* Child */
                execv(args[0], (char * const *) args);
                _exit(EXIT_FAILURE);
        }

        r = wait_for_terminate_and_check("kexec", pid, WAIT_LOG);
        if (r < 0)
                return r;
        if (r > 0)
                /* Command failed */
                return -EPROTO;
        return 0;
}

static int set_exit_code(uint8_t code) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus *bus;
        int r;

        r = acquire_bus(BUS_MANAGER, &bus);
        if (r < 0)
                return r;

        r = sd_bus_call_method(
                        bus,
                        "org.freedesktop.systemd1",
                        "/org/freedesktop/systemd1",
                        "org.freedesktop.systemd1.Manager",
                        "SetExitCode",
                        &error,
                        NULL,
                        "y", code);
        if (r < 0)
                return log_error_errno(r, "Failed to set exit code: %s", bus_error_message(&error, r));

        return 0;
}

static int start_special(int argc, char *argv[], void *userdata) {
        enum action a;
        int r;
        bool termination_action; /* an action that terminates the manager,
                                  * can be performed also by signal. */

        assert(argv);

        a = verb_to_action(argv[0]);

        r = logind_check_inhibitors(a);
        if (r < 0)
                return r;

        if (arg_force >= 2) {
                r = must_be_root();
                if (r < 0)
                        return r;
        }

        r = prepare_firmware_setup();
        if (r < 0)
                return r;

        if (a == ACTION_REBOOT && argc > 1) {
                r = update_reboot_parameter_and_warn(argv[1]);
                if (r < 0)
                        return r;

        } else if (a == ACTION_KEXEC) {
                r = load_kexec_kernel();
                if (r < 0 && arg_force >= 1)
                        log_notice("Failed to load kexec kernel, continuing without.");
                else if (r < 0)
                        return r;

        } else if (a == ACTION_EXIT && argc > 1) {
                uint8_t code;

                /* If the exit code is not given on the command line,
                 * don't reset it to zero: just keep it as it might
                 * have been set previously. */

                r = safe_atou8(argv[1], &code);
                if (r < 0)
                        return log_error_errno(r, "Invalid exit code.");

                r = set_exit_code(code);
                if (r < 0)
                        return r;
        }

        termination_action = IN_SET(a,
                                    ACTION_HALT,
                                    ACTION_POWEROFF,
                                    ACTION_REBOOT);
        if (termination_action && arg_force >= 2)
                return halt_now(a);

        if (arg_force >= 1 &&
            (termination_action || IN_SET(a, ACTION_KEXEC, ACTION_EXIT)))
                r = trivial_method(argc, argv, userdata);
        else {
                /* First try logind, to allow authentication with polkit */
                if (IN_SET(a,
                           ACTION_POWEROFF,
                           ACTION_REBOOT,
                           ACTION_HALT,
                           ACTION_SUSPEND,
                           ACTION_HIBERNATE,
                           ACTION_HYBRID_SLEEP)) {

                        r = logind_reboot(a);
                        if (r >= 0)
                                return r;
                        if (IN_SET(r, -EOPNOTSUPP, -EINPROGRESS))
                                /* requested operation is not supported or already in progress */
                                return r;

                        /* On all other errors, try low-level operation. In order to minimize the difference between
                         * operation with and without logind, we explicitly enable non-blocking mode for this, as
                         * logind's shutdown operations are always non-blocking. */

                        arg_no_block = true;

                } else if (IN_SET(a, ACTION_EXIT, ACTION_KEXEC))
                        /* Since exit/kexec are so close in behaviour to power-off/reboot, let's also make them
                         * asynchronous, in order to not confuse the user needlessly with unexpected behaviour. */
                        arg_no_block = true;

                r = start_unit(argc, argv, userdata);
        }

        if (termination_action && arg_force < 2 &&
            IN_SET(r, -ENOENT, -ETIMEDOUT))
                log_notice("It is possible to perform action directly, see discussion of --force --force in man:systemctl(1).");

        return r;
}

static int start_system_special(int argc, char *argv[], void *userdata) {
        /* Like start_special above, but raises an error when running in user mode */

        if (arg_scope != UNIT_FILE_SYSTEM) {
                log_error("Bad action for %s mode.",
                          arg_scope == UNIT_FILE_GLOBAL ? "--global" : "--user");
                return -EINVAL;
        }

        return start_special(argc, argv, userdata);
}

static int check_unit_generic(int code, const UnitActiveState good_states[], int nb_states, char **args) {
        _cleanup_strv_free_ char **names = NULL;
        UnitActiveState active_state;
        sd_bus *bus;
        char **name;
        int r, i;
        bool found = false;

        r = acquire_bus(BUS_MANAGER, &bus);
        if (r < 0)
                return r;

        r = expand_names(bus, args, NULL, &names);
        if (r < 0)
                return log_error_errno(r, "Failed to expand names: %m");

        STRV_FOREACH(name, names) {
                r = get_state_one_unit(bus, *name, &active_state);
                if (r < 0)
                        return r;

                if (!arg_quiet)
                        puts(unit_active_state_to_string(active_state));

                for (i = 0; i < nb_states; ++i)
                        if (good_states[i] == active_state)
                                found = true;
        }

        /* use the given return code for the case that we won't find
         * any unit which matches the list */
        return found ? 0 : code;
}

static int check_unit_active(int argc, char *argv[], void *userdata) {
        const UnitActiveState states[] = { UNIT_ACTIVE, UNIT_RELOADING };
        /* According to LSB: 3, "program is not running" */
        return check_unit_generic(EXIT_PROGRAM_NOT_RUNNING, states, ELEMENTSOF(states), strv_skip(argv, 1));
}

static int check_unit_failed(int argc, char *argv[], void *userdata) {
        const UnitActiveState states[] = { UNIT_FAILED };
        return check_unit_generic(EXIT_PROGRAM_DEAD_AND_PID_EXISTS, states, ELEMENTSOF(states), strv_skip(argv, 1));
}

static int kill_unit(int argc, char *argv[], void *userdata) {
        _cleanup_strv_free_ char **names = NULL;
        char *kill_who = NULL, **name;
        sd_bus *bus;
        int r, q;

        r = acquire_bus(BUS_MANAGER, &bus);
        if (r < 0)
                return r;

        polkit_agent_open_maybe();

        if (!arg_kill_who)
                arg_kill_who = "all";

        /* --fail was specified */
        if (streq(arg_job_mode, "fail"))
                kill_who = strjoina(arg_kill_who, "-fail");

        r = expand_names(bus, strv_skip(argv, 1), NULL, &names);
        if (r < 0)
                return log_error_errno(r, "Failed to expand names: %m");

        STRV_FOREACH(name, names) {
                _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;

                q = sd_bus_call_method(
                                bus,
                                "org.freedesktop.systemd1",
                                "/org/freedesktop/systemd1",
                                "org.freedesktop.systemd1.Manager",
                                "KillUnit",
                                &error,
                                NULL,
                                "ssi", *name, kill_who ? kill_who : arg_kill_who, arg_signal);
                if (q < 0) {
                        log_error_errno(q, "Failed to kill unit %s: %s", *name, bus_error_message(&error, q));
                        if (r == 0)
                                r = q;
                }
        }

        return r;
}

typedef struct ExecStatusInfo {
        char *name;

        char *path;
        char **argv;

        bool ignore;

        usec_t start_timestamp;
        usec_t exit_timestamp;
        pid_t pid;
        int code;
        int status;

        LIST_FIELDS(struct ExecStatusInfo, exec);
} ExecStatusInfo;

static void exec_status_info_free(ExecStatusInfo *i) {
        assert(i);

        free(i->name);
        free(i->path);
        strv_free(i->argv);
        free(i);
}

static int exec_status_info_deserialize(sd_bus_message *m, ExecStatusInfo *i) {
        uint64_t start_timestamp, exit_timestamp, start_timestamp_monotonic, exit_timestamp_monotonic;
        const char *path;
        uint32_t pid;
        int32_t code, status;
        int ignore, r;

        assert(m);
        assert(i);

        r = sd_bus_message_enter_container(m, SD_BUS_TYPE_STRUCT, "sasbttttuii");
        if (r < 0)
                return bus_log_parse_error(r);
        else if (r == 0)
                return 0;

        r = sd_bus_message_read(m, "s", &path);
        if (r < 0)
                return bus_log_parse_error(r);

        i->path = strdup(path);
        if (!i->path)
                return log_oom();

        r = sd_bus_message_read_strv(m, &i->argv);
        if (r < 0)
                return bus_log_parse_error(r);

        r = sd_bus_message_read(m,
                                "bttttuii",
                                &ignore,
                                &start_timestamp, &start_timestamp_monotonic,
                                &exit_timestamp, &exit_timestamp_monotonic,
                                &pid,
                                &code, &status);
        if (r < 0)
                return bus_log_parse_error(r);

        i->ignore = ignore;
        i->start_timestamp = (usec_t) start_timestamp;
        i->exit_timestamp = (usec_t) exit_timestamp;
        i->pid = (pid_t) pid;
        i->code = code;
        i->status = status;

        r = sd_bus_message_exit_container(m);
        if (r < 0)
                return bus_log_parse_error(r);

        return 1;
}

typedef struct UnitCondition {
        char *name;
        char *param;
        bool trigger;
        bool negate;
        int tristate;

        LIST_FIELDS(struct UnitCondition, conditions);
} UnitCondition;

static void unit_condition_free(UnitCondition *c) {
        if (!c)
                return;

        free(c->name);
        free(c->param);
        free(c);
}

DEFINE_TRIVIAL_CLEANUP_FUNC(UnitCondition*, unit_condition_free);

typedef struct UnitStatusInfo {
        const char *id;
        const char *load_state;
        const char *active_state;
        const char *sub_state;
        const char *unit_file_state;
        const char *unit_file_preset;

        const char *description;
        const char *following;

        char **documentation;

        const char *fragment_path;
        const char *source_path;
        const char *control_group;

        char **dropin_paths;

        const char *load_error;
        const char *result;

        usec_t inactive_exit_timestamp;
        usec_t inactive_exit_timestamp_monotonic;
        usec_t active_enter_timestamp;
        usec_t active_exit_timestamp;
        usec_t inactive_enter_timestamp;

        bool need_daemon_reload;
        bool transient;

        /* Service */
        pid_t main_pid;
        pid_t control_pid;
        const char *status_text;
        const char *pid_file;
        bool running:1;
        int status_errno;

        usec_t start_timestamp;
        usec_t exit_timestamp;

        int exit_code, exit_status;

        usec_t condition_timestamp;
        bool condition_result;
        LIST_HEAD(UnitCondition, conditions);

        usec_t assert_timestamp;
        bool assert_result;
        bool failed_assert_trigger;
        bool failed_assert_negate;
        const char *failed_assert;
        const char *failed_assert_parameter;
        usec_t next_elapse_real;
        usec_t next_elapse_monotonic;

        /* Socket */
        unsigned n_accepted;
        unsigned n_connections;
        bool accept;

        /* Pairs of type, path */
        char **listen;

        /* Device */
        const char *sysfs_path;

        /* Mount, Automount */
        const char *where;

        /* Swap */
        const char *what;

        /* CGroup */
        uint64_t memory_current;
        uint64_t memory_low;
        uint64_t memory_high;
        uint64_t memory_max;
        uint64_t memory_swap_max;
        uint64_t memory_limit;
        uint64_t cpu_usage_nsec;
        uint64_t tasks_current;
        uint64_t tasks_max;

        uint64_t ip_ingress_bytes;
        uint64_t ip_egress_bytes;

        LIST_HEAD(ExecStatusInfo, exec);
} UnitStatusInfo;

static void unit_status_info_free(UnitStatusInfo *info) {
        ExecStatusInfo *p;
        UnitCondition *c;

        strv_free(info->documentation);
        strv_free(info->dropin_paths);
        strv_free(info->listen);

        while ((c = info->conditions)) {
                LIST_REMOVE(conditions, info->conditions, c);
                unit_condition_free(c);
        }

        while ((p = info->exec)) {
                LIST_REMOVE(exec, info->exec, p);
                exec_status_info_free(p);
        }
}

static void print_status_info(
                sd_bus *bus,
                UnitStatusInfo *i,
                bool *ellipsized) {

        ExecStatusInfo *p;
        const char *active_on, *active_off, *on, *off, *ss;
        usec_t timestamp;
        char since1[FORMAT_TIMESTAMP_RELATIVE_MAX], *s1;
        char since2[FORMAT_TIMESTAMP_MAX], *s2;
        const char *path;
        char **t, **t2;
        int r;

        assert(i);

        /* This shows pretty information about a unit. See
         * print_property() for a low-level property printer */

        if (streq_ptr(i->active_state, "failed")) {
                active_on = ansi_highlight_red();
                active_off = ansi_normal();
        } else if (STRPTR_IN_SET(i->active_state, "active", "reloading")) {
                active_on = ansi_highlight_green();
                active_off = ansi_normal();
        } else
                active_on = active_off = "";

        printf("%s%s%s %s", active_on, special_glyph(BLACK_CIRCLE), active_off, strna(i->id));

        if (i->description && !streq_ptr(i->id, i->description))
                printf(" - %s", i->description);

        printf("\n");

        if (i->following)
                printf("   Follow: unit currently follows state of %s\n", i->following);

        if (streq_ptr(i->load_state, "error")) {
                on = ansi_highlight_red();
                off = ansi_normal();
        } else
                on = off = "";

        path = i->source_path ? i->source_path : i->fragment_path;

        if (i->load_error != 0)
                printf("   Loaded: %s%s%s (Reason: %s)\n",
                       on, strna(i->load_state), off, i->load_error);
        else if (path && !isempty(i->unit_file_state) && !isempty(i->unit_file_preset) &&
                 !STR_IN_SET(i->unit_file_state, "generated", "transient"))
                printf("   Loaded: %s%s%s (%s; %s; vendor preset: %s)\n",
                       on, strna(i->load_state), off, path, i->unit_file_state, i->unit_file_preset);
        else if (path && !isempty(i->unit_file_state))
                printf("   Loaded: %s%s%s (%s; %s)\n",
                       on, strna(i->load_state), off, path, i->unit_file_state);
        else if (path)
                printf("   Loaded: %s%s%s (%s)\n",
                       on, strna(i->load_state), off, path);
        else
                printf("   Loaded: %s%s%s\n",
                       on, strna(i->load_state), off);

        if (i->transient)
                printf("Transient: yes\n");

        if (!strv_isempty(i->dropin_paths)) {
                _cleanup_free_ char *dir = NULL;
                bool last = false;
                char ** dropin;

                STRV_FOREACH(dropin, i->dropin_paths) {
                        if (!dir || last) {
                                printf(dir ? "           " :
                                             "  Drop-In: ");

                                dir = mfree(dir);

                                dir = dirname_malloc(*dropin);
                                if (!dir) {
                                        log_oom();
                                        return;
                                }

                                printf("%s\n"
                                       "           %s", dir,
                                       special_glyph(TREE_RIGHT));
                        }

                        last = ! (*(dropin + 1) && startswith(*(dropin + 1), dir));

                        printf("%s%s", basename(*dropin), last ? "\n" : ", ");
                }
        }

        ss = streq_ptr(i->active_state, i->sub_state) ? NULL : i->sub_state;
        if (ss)
                printf("   Active: %s%s (%s)%s",
                       active_on, strna(i->active_state), ss, active_off);
        else
                printf("   Active: %s%s%s",
                       active_on, strna(i->active_state), active_off);

        if (!isempty(i->result) && !streq(i->result, "success"))
                printf(" (Result: %s)", i->result);

        timestamp = STRPTR_IN_SET(i->active_state, "active", "reloading") ? i->active_enter_timestamp :
                    STRPTR_IN_SET(i->active_state, "inactive", "failed")  ? i->inactive_enter_timestamp :
                    STRPTR_IN_SET(i->active_state, "activating")          ? i->inactive_exit_timestamp :
                                                                            i->active_exit_timestamp;

        s1 = format_timestamp_relative(since1, sizeof(since1), timestamp);
        s2 = format_timestamp(since2, sizeof(since2), timestamp);

        if (s1)
                printf(" since %s; %s\n", s2, s1);
        else if (s2)
                printf(" since %s\n", s2);
        else
                printf("\n");

        if (endswith(i->id, ".timer")) {
                char tstamp1[FORMAT_TIMESTAMP_RELATIVE_MAX],
                     tstamp2[FORMAT_TIMESTAMP_MAX];
                char *next_rel_time, *next_time;
                dual_timestamp nw, next = {i->next_elapse_real,
                                           i->next_elapse_monotonic};
                usec_t next_elapse;

                printf("  Trigger: ");

                dual_timestamp_get(&nw);
                next_elapse = calc_next_elapse(&nw, &next);
                next_rel_time = format_timestamp_relative(tstamp1,
                                                          sizeof(tstamp1),
                                                          next_elapse);
                next_time = format_timestamp(tstamp2,
                                             sizeof(tstamp2),
                                             next_elapse);

                if (next_time && next_rel_time)
                        printf("%s; %s\n", next_time, next_rel_time);
                else
                        printf("n/a\n");
        }

        if (!i->condition_result && i->condition_timestamp > 0) {
                UnitCondition *c;
                int n = 0;

                s1 = format_timestamp_relative(since1, sizeof(since1), i->condition_timestamp);
                s2 = format_timestamp(since2, sizeof(since2), i->condition_timestamp);

                printf("Condition: start %scondition failed%s at %s%s%s\n",
                       ansi_highlight_yellow(), ansi_normal(),
                       s2, s1 ? "; " : "", strempty(s1));

                LIST_FOREACH(conditions, c, i->conditions)
                        if (c->tristate < 0)
                                n++;

                LIST_FOREACH(conditions, c, i->conditions)
                        if (c->tristate < 0)
                                printf("           %s %s=%s%s%s was not met\n",
                                       --n ? special_glyph(TREE_BRANCH) : special_glyph(TREE_RIGHT),
                                       c->name,
                                       c->trigger ? "|" : "",
                                       c->negate ? "!" : "",
                                       c->param);
        }

        if (!i->assert_result && i->assert_timestamp > 0) {
                s1 = format_timestamp_relative(since1, sizeof(since1), i->assert_timestamp);
                s2 = format_timestamp(since2, sizeof(since2), i->assert_timestamp);

                printf("   Assert: start %sassertion failed%s at %s%s%s\n",
                       ansi_highlight_red(), ansi_normal(),
                       s2, s1 ? "; " : "", strempty(s1));
                if (i->failed_assert_trigger)
                        printf("           none of the trigger assertions were met\n");
                else if (i->failed_assert)
                        printf("           %s=%s%s was not met\n",
                               i->failed_assert,
                               i->failed_assert_negate ? "!" : "",
                               i->failed_assert_parameter);
        }

        if (i->sysfs_path)
                printf("   Device: %s\n", i->sysfs_path);
        if (i->where)
                printf("    Where: %s\n", i->where);
        if (i->what)
                printf("     What: %s\n", i->what);

        STRV_FOREACH(t, i->documentation)
                printf(" %*s %s\n", 9, t == i->documentation ? "Docs:" : "", *t);

        STRV_FOREACH_PAIR(t, t2, i->listen)
                printf(" %*s %s (%s)\n", 9, t == i->listen ? "Listen:" : "", *t2, *t);

        if (i->accept)
                printf(" Accepted: %u; Connected: %u\n", i->n_accepted, i->n_connections);

        LIST_FOREACH(exec, p, i->exec) {
                _cleanup_free_ char *argv = NULL;
                bool good;

                /* Only show exited processes here */
                if (p->code == 0)
                        continue;

                argv = strv_join(p->argv, " ");
                printf("  Process: "PID_FMT" %s=%s ", p->pid, p->name, strna(argv));

                good = is_clean_exit(p->code, p->status, EXIT_CLEAN_DAEMON, NULL);
                if (!good) {
                        on = ansi_highlight_red();
                        off = ansi_normal();
                } else
                        on = off = "";

                printf("%s(code=%s, ", on, sigchld_code_to_string(p->code));

                if (p->code == CLD_EXITED) {
                        const char *c;

                        printf("status=%i", p->status);

                        c = exit_status_to_string(p->status, EXIT_STATUS_SYSTEMD);
                        if (c)
                                printf("/%s", c);

                } else
                        printf("signal=%s", signal_to_string(p->status));

                printf(")%s\n", off);

                if (i->main_pid == p->pid &&
                    i->start_timestamp == p->start_timestamp &&
                    i->exit_timestamp == p->start_timestamp)
                        /* Let's not show this twice */
                        i->main_pid = 0;

                if (p->pid == i->control_pid)
                        i->control_pid = 0;
        }

        if (i->main_pid > 0 || i->control_pid > 0) {
                if (i->main_pid > 0) {
                        printf(" Main PID: "PID_FMT, i->main_pid);

                        if (i->running) {

                                if (arg_transport == BUS_TRANSPORT_LOCAL) {
                                        _cleanup_free_ char *comm = NULL;

                                        (void) get_process_comm(i->main_pid, &comm);
                                        if (comm)
                                                printf(" (%s)", comm);
                                }

                        } else if (i->exit_code > 0) {
                                printf(" (code=%s, ", sigchld_code_to_string(i->exit_code));

                                if (i->exit_code == CLD_EXITED) {
                                        const char *c;

                                        printf("status=%i", i->exit_status);

                                        c = exit_status_to_string(i->exit_status, EXIT_STATUS_SYSTEMD);
                                        if (c)
                                                printf("/%s", c);

                                } else
                                        printf("signal=%s", signal_to_string(i->exit_status));
                                printf(")");
                        }
                }

                if (i->control_pid > 0) {
                        _cleanup_free_ char *c = NULL;

                        if (i->main_pid > 0)
                                fputs("; Control PID: ", stdout);
                        else
                                fputs("Cntrl PID: ", stdout); /* if first in column, abbreviated so it fits alignment */

                        printf(PID_FMT, i->control_pid);

                        if (arg_transport == BUS_TRANSPORT_LOCAL) {
                                (void) get_process_comm(i->control_pid, &c);
                                if (c)
                                        printf(" (%s)", c);
                        }
                }

                printf("\n");
        }

        if (i->status_text)
                printf("   Status: \"%s\"\n", i->status_text);
        if (i->status_errno > 0)
                printf("    Error: %i (%s)\n", i->status_errno, strerror(i->status_errno));

        if (i->ip_ingress_bytes != (uint64_t) -1 && i->ip_egress_bytes != (uint64_t) -1) {
                char buf_in[FORMAT_BYTES_MAX], buf_out[FORMAT_BYTES_MAX];

                printf("       IP: %s in, %s out\n",
                        format_bytes(buf_in, sizeof(buf_in), i->ip_ingress_bytes),
                        format_bytes(buf_out, sizeof(buf_out), i->ip_egress_bytes));
        }

        if (i->tasks_current != (uint64_t) -1) {
                printf("    Tasks: %" PRIu64, i->tasks_current);

                if (i->tasks_max != (uint64_t) -1)
                        printf(" (limit: %" PRIu64 ")\n", i->tasks_max);
                else
                        printf("\n");
        }

        if (i->memory_current != (uint64_t) -1) {
                char buf[FORMAT_BYTES_MAX];

                printf("   Memory: %s", format_bytes(buf, sizeof(buf), i->memory_current));

                if (i->memory_low > 0 || i->memory_high != CGROUP_LIMIT_MAX ||
                    i->memory_max != CGROUP_LIMIT_MAX || i->memory_swap_max != CGROUP_LIMIT_MAX ||
                    i->memory_limit != CGROUP_LIMIT_MAX) {
                        const char *prefix = "";

                        printf(" (");
                        if (i->memory_low > 0) {
                                printf("%slow: %s", prefix, format_bytes(buf, sizeof(buf), i->memory_low));
                                prefix = " ";
                        }
                        if (i->memory_high != CGROUP_LIMIT_MAX) {
                                printf("%shigh: %s", prefix, format_bytes(buf, sizeof(buf), i->memory_high));
                                prefix = " ";
                        }
                        if (i->memory_max != CGROUP_LIMIT_MAX) {
                                printf("%smax: %s", prefix, format_bytes(buf, sizeof(buf), i->memory_max));
                                prefix = " ";
                        }
                        if (i->memory_swap_max != CGROUP_LIMIT_MAX) {
                                printf("%sswap max: %s", prefix, format_bytes(buf, sizeof(buf), i->memory_swap_max));
                                prefix = " ";
                        }
                        if (i->memory_limit != CGROUP_LIMIT_MAX) {
                                printf("%slimit: %s", prefix, format_bytes(buf, sizeof(buf), i->memory_limit));
                                prefix = " ";
                        }
                        printf(")");
                }
                printf("\n");
        }

        if (i->cpu_usage_nsec != (uint64_t) -1) {
                char buf[FORMAT_TIMESPAN_MAX];
                printf("      CPU: %s\n", format_timespan(buf, sizeof(buf), i->cpu_usage_nsec / NSEC_PER_USEC, USEC_PER_MSEC));
        }

        if (i->control_group) {
                _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
                static const char prefix[] = "           ";
                unsigned c;

                printf("   CGroup: %s\n", i->control_group);

                c = columns();
                if (c > sizeof(prefix) - 1)
                        c -= sizeof(prefix) - 1;
                else
                        c = 0;

                r = unit_show_processes(bus, i->id, i->control_group, prefix, c, get_output_flags(), &error);
                if (r == -EBADR) {
                        unsigned k = 0;
                        pid_t extra[2];

                        /* Fallback for older systemd versions where the GetUnitProcesses() call is not yet available */

                        if (i->main_pid > 0)
                                extra[k++] = i->main_pid;

                        if (i->control_pid > 0)
                                extra[k++] = i->control_pid;

                        show_cgroup_and_extra(SYSTEMD_CGROUP_CONTROLLER, i->control_group, prefix, c, extra, k, get_output_flags());
                } else if (r < 0)
                        log_warning_errno(r, "Failed to dump process list, ignoring: %s", bus_error_message(&error, r));
        }

        if (i->id && arg_transport == BUS_TRANSPORT_LOCAL)
                show_journal_by_unit(
                                stdout,
                                i->id,
                                arg_output,
                                0,
                                i->inactive_exit_timestamp_monotonic,
                                arg_lines,
                                getuid(),
                                get_output_flags() | OUTPUT_BEGIN_NEWLINE,
                                SD_JOURNAL_LOCAL_ONLY,
                                arg_scope == UNIT_FILE_SYSTEM,
                                ellipsized);

        if (i->need_daemon_reload)
                warn_unit_file_changed(i->id);
}

static void show_unit_help(UnitStatusInfo *i) {
        char **p;

        assert(i);

        if (!i->documentation) {
                log_info("Documentation for %s not known.", i->id);
                return;
        }

        STRV_FOREACH(p, i->documentation)
                if (startswith(*p, "man:"))
                        show_man_page(*p + 4, false);
                else
                        log_info("Can't show: %s", *p);
}

static int status_property(const char *name, sd_bus_message *m, UnitStatusInfo *i, const char *contents) {
        int r;

        assert(name);
        assert(m);
        assert(i);

        switch (contents[0]) {

        case SD_BUS_TYPE_STRING: {
                const char *s;

                r = sd_bus_message_read(m, "s", &s);
                if (r < 0)
                        return bus_log_parse_error(r);

                if (!isempty(s)) {
                        if (streq(name, "Id"))
                                i->id = s;
                        else if (streq(name, "LoadState"))
                                i->load_state = s;
                        else if (streq(name, "ActiveState"))
                                i->active_state = s;
                        else if (streq(name, "SubState"))
                                i->sub_state = s;
                        else if (streq(name, "Description"))
                                i->description = s;
                        else if (streq(name, "FragmentPath"))
                                i->fragment_path = s;
                        else if (streq(name, "SourcePath"))
                                i->source_path = s;
#ifndef NOLEGACY
                        else if (streq(name, "DefaultControlGroup")) {
                                const char *e;
                                e = startswith(s, SYSTEMD_CGROUP_CONTROLLER ":");
                                if (e)
                                        i->control_group = e;
                        }
#endif
                        else if (streq(name, "ControlGroup"))
                                i->control_group = s;
                        else if (streq(name, "StatusText"))
                                i->status_text = s;
                        else if (streq(name, "PIDFile"))
                                i->pid_file = s;
                        else if (streq(name, "SysFSPath"))
                                i->sysfs_path = s;
                        else if (streq(name, "Where"))
                                i->where = s;
                        else if (streq(name, "What"))
                                i->what = s;
                        else if (streq(name, "Following"))
                                i->following = s;
                        else if (streq(name, "UnitFileState"))
                                i->unit_file_state = s;
                        else if (streq(name, "UnitFilePreset"))
                                i->unit_file_preset = s;
                        else if (streq(name, "Result"))
                                i->result = s;
                }

                break;
        }

        case SD_BUS_TYPE_BOOLEAN: {
                int b;

                r = sd_bus_message_read(m, "b", &b);
                if (r < 0)
                        return bus_log_parse_error(r);

                if (streq(name, "Accept"))
                        i->accept = b;
                else if (streq(name, "NeedDaemonReload"))
                        i->need_daemon_reload = b;
                else if (streq(name, "ConditionResult"))
                        i->condition_result = b;
                else if (streq(name, "AssertResult"))
                        i->assert_result = b;
                else if (streq(name, "Transient"))
                        i->transient = b;

                break;
        }

        case SD_BUS_TYPE_UINT32: {
                uint32_t u;

                r = sd_bus_message_read(m, "u", &u);
                if (r < 0)
                        return bus_log_parse_error(r);

                if (streq(name, "MainPID")) {
                        if (u > 0) {
                                i->main_pid = (pid_t) u;
                                i->running = true;
                        }
                } else if (streq(name, "ControlPID"))
                        i->control_pid = (pid_t) u;
                else if (streq(name, "ExecMainPID")) {
                        if (u > 0)
                                i->main_pid = (pid_t) u;
                } else if (streq(name, "NAccepted"))
                        i->n_accepted = u;
                else if (streq(name, "NConnections"))
                        i->n_connections = u;

                break;
        }

        case SD_BUS_TYPE_INT32: {
                int32_t j;

                r = sd_bus_message_read(m, "i", &j);
                if (r < 0)
                        return bus_log_parse_error(r);

                if (streq(name, "ExecMainCode"))
                        i->exit_code = (int) j;
                else if (streq(name, "ExecMainStatus"))
                        i->exit_status = (int) j;
                else if (streq(name, "StatusErrno"))
                        i->status_errno = (int) j;

                break;
        }

        case SD_BUS_TYPE_UINT64: {
                uint64_t u;

                r = sd_bus_message_read(m, "t", &u);
                if (r < 0)
                        return bus_log_parse_error(r);

                if (streq(name, "ExecMainStartTimestamp"))
                        i->start_timestamp = (usec_t) u;
                else if (streq(name, "ExecMainExitTimestamp"))
                        i->exit_timestamp = (usec_t) u;
                else if (streq(name, "ActiveEnterTimestamp"))
                        i->active_enter_timestamp = (usec_t) u;
                else if (streq(name, "InactiveEnterTimestamp"))
                        i->inactive_enter_timestamp = (usec_t) u;
                else if (streq(name, "InactiveExitTimestamp"))
                        i->inactive_exit_timestamp = (usec_t) u;
                else if (streq(name, "InactiveExitTimestampMonotonic"))
                        i->inactive_exit_timestamp_monotonic = (usec_t) u;
                else if (streq(name, "ActiveExitTimestamp"))
                        i->active_exit_timestamp = (usec_t) u;
                else if (streq(name, "ConditionTimestamp"))
                        i->condition_timestamp = (usec_t) u;
                else if (streq(name, "AssertTimestamp"))
                        i->assert_timestamp = (usec_t) u;
                else if (streq(name, "MemoryCurrent"))
                        i->memory_current = u;
                else if (streq(name, "MemoryLow"))
                        i->memory_low = u;
                else if (streq(name, "MemoryHigh"))
                        i->memory_high = u;
                else if (streq(name, "MemoryMax"))
                        i->memory_max = u;
                else if (streq(name, "MemorySwapMax"))
                        i->memory_swap_max = u;
                else if (streq(name, "MemoryLimit"))
                        i->memory_limit = u;
                else if (streq(name, "TasksCurrent"))
                        i->tasks_current = u;
                else if (streq(name, "TasksMax"))
                        i->tasks_max = u;
                else if (streq(name, "CPUUsageNSec"))
                        i->cpu_usage_nsec = u;
                else if (streq(name, "NextElapseUSecMonotonic"))
                        i->next_elapse_monotonic = u;
                else if (streq(name, "NextElapseUSecRealtime"))
                        i->next_elapse_real = u;
                else if (streq(name, "IPIngressBytes"))
                        i->ip_ingress_bytes = u;
                else if (streq(name, "IPEgressBytes"))
                        i->ip_egress_bytes = u;

                break;
        }

        case SD_BUS_TYPE_ARRAY:

                if (contents[1] == SD_BUS_TYPE_STRUCT_BEGIN && startswith(name, "Exec")) {
                        _cleanup_free_ ExecStatusInfo *info = NULL;

                        r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "(sasbttttuii)");
                        if (r < 0)
                                return bus_log_parse_error(r);

                        info = new0(ExecStatusInfo, 1);
                        if (!info)
                                return log_oom();

                        while ((r = exec_status_info_deserialize(m, info)) > 0) {

                                info->name = strdup(name);
                                if (!info->name)
                                        return log_oom();

                                LIST_PREPEND(exec, i->exec, info);

                                info = new0(ExecStatusInfo, 1);
                                if (!info)
                                        return log_oom();
                        }

                        if (r < 0)
                                return bus_log_parse_error(r);

                        r = sd_bus_message_exit_container(m);
                        if (r < 0)
                                return bus_log_parse_error(r);

                        return 0;

                } else if (contents[1] == SD_BUS_TYPE_STRUCT_BEGIN && streq(name, "Listen")) {
                        const char *type, *path;

                        r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "(ss)");
                        if (r < 0)
                                return bus_log_parse_error(r);

                        while ((r = sd_bus_message_read(m, "(ss)", &type, &path)) > 0) {

                                r = strv_extend(&i->listen, type);
                                if (r < 0)
                                        return r;

                                r = strv_extend(&i->listen, path);
                                if (r < 0)
                                        return r;
                        }
                        if (r < 0)
                                return bus_log_parse_error(r);

                        r = sd_bus_message_exit_container(m);
                        if (r < 0)
                                return bus_log_parse_error(r);

                        return 0;

                } else if (contents[1] == SD_BUS_TYPE_STRING && streq(name, "DropInPaths")) {

                        r = sd_bus_message_read_strv(m, &i->dropin_paths);
                        if (r < 0)
                                return bus_log_parse_error(r);

                } else if (contents[1] == SD_BUS_TYPE_STRING && streq(name, "Documentation")) {

                        r = sd_bus_message_read_strv(m, &i->documentation);
                        if (r < 0)
                                return bus_log_parse_error(r);

                } else if (contents[1] == SD_BUS_TYPE_STRUCT_BEGIN && streq(name, "Conditions")) {
                        const char *cond, *param;
                        int trigger, negate;
                        int32_t state;

                        r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "(sbbsi)");
                        if (r < 0)
                                return bus_log_parse_error(r);

                        while ((r = sd_bus_message_read(m, "(sbbsi)", &cond, &trigger, &negate, &param, &state)) > 0) {
                                _cleanup_(unit_condition_freep) UnitCondition *c = NULL;

                                log_debug("%s trigger=%d negate=%d %s →%d", cond, trigger, negate, param, state);

                                c = new0(UnitCondition, 1);
                                if (!c)
                                        return log_oom();

                                c->name = strdup(cond);
                                c->param = strdup(param);
                                if (!c->name || !c->param)
                                        return log_oom();

                                c->trigger = trigger;
                                c->negate = negate;
                                c->tristate = state;

                                LIST_PREPEND(conditions, i->conditions, c);
                                c = NULL;
                        }
                        if (r < 0)
                                return bus_log_parse_error(r);

                        r = sd_bus_message_exit_container(m);
                        if (r < 0)
                                return bus_log_parse_error(r);

                } else if (contents[1] == SD_BUS_TYPE_STRUCT_BEGIN && streq(name, "Asserts")) {
                        const char *cond, *param;
                        int trigger, negate;
                        int32_t state;

                        r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "(sbbsi)");
                        if (r < 0)
                                return bus_log_parse_error(r);

                        while ((r = sd_bus_message_read(m, "(sbbsi)", &cond, &trigger, &negate, &param, &state)) > 0) {
                                log_debug("%s %d %d %s %d", cond, trigger, negate, param, state);
                                if (state < 0 && (!trigger || !i->failed_assert)) {
                                        i->failed_assert = cond;
                                        i->failed_assert_trigger = trigger;
                                        i->failed_assert_negate = negate;
                                        i->failed_assert_parameter = param;
                                }
                        }
                        if (r < 0)
                                return bus_log_parse_error(r);

                        r = sd_bus_message_exit_container(m);
                        if (r < 0)
                                return bus_log_parse_error(r);

                } else
                        goto skip;

                break;

        case SD_BUS_TYPE_STRUCT_BEGIN:

                if (streq(name, "LoadError")) {
                        const char *n, *message;

                        r = sd_bus_message_read(m, "(ss)", &n, &message);
                        if (r < 0)
                                return bus_log_parse_error(r);

                        if (!isempty(message))
                                i->load_error = message;
                } else
                        goto skip;

                break;

        default:
                goto skip;
        }

        return 0;

skip:
        r = sd_bus_message_skip(m, contents);
        if (r < 0)
                return bus_log_parse_error(r);

        return 0;
}

#define print_prop(name, fmt, ...)                                      \
        do {                                                            \
                if (arg_value)                                          \
                        printf(fmt "\n", __VA_ARGS__);                  \
                else                                                    \
                        printf("%s=" fmt "\n", name, __VA_ARGS__);      \
        } while (0)

static int print_property(const char *name, sd_bus_message *m, const char *contents) {
        int r;

        assert(name);
        assert(m);

        /* This is a low-level property printer, see
         * print_status_info() for the nicer output */

        if (arg_properties && !strv_find(arg_properties, name)) {
                /* skip what we didn't read */
                r = sd_bus_message_skip(m, contents);
                return r;
        }

        switch (contents[0]) {

        case SD_BUS_TYPE_STRUCT_BEGIN:

                if (contents[1] == SD_BUS_TYPE_UINT32 && streq(name, "Job")) {
                        uint32_t u;

                        r = sd_bus_message_read(m, "(uo)", &u, NULL);
                        if (r < 0)
                                return bus_log_parse_error(r);

                        if (u > 0)
                                print_prop(name, "%"PRIu32, u);
                        else if (arg_all)
                                print_prop(name, "%s", "");

                        return 0;

                } else if (contents[1] == SD_BUS_TYPE_STRING && streq(name, "Unit")) {
                        const char *s;

                        r = sd_bus_message_read(m, "(so)", &s, NULL);
                        if (r < 0)
                                return bus_log_parse_error(r);

                        if (arg_all || !isempty(s))
                                print_prop(name, "%s", s);

                        return 0;

                } else if (contents[1] == SD_BUS_TYPE_STRING && streq(name, "LoadError")) {
                        const char *a = NULL, *b = NULL;

                        r = sd_bus_message_read(m, "(ss)", &a, &b);
                        if (r < 0)
                                return bus_log_parse_error(r);

                        if (arg_all || !isempty(a) || !isempty(b))
                                print_prop(name, "%s \"%s\"", strempty(a), strempty(b));

                        return 0;
                } else if (streq_ptr(name, "SystemCallFilter")) {
                        _cleanup_strv_free_ char **l = NULL;
                        int whitelist;

                        r = sd_bus_message_enter_container(m, 'r', "bas");
                        if (r < 0)
                                return bus_log_parse_error(r);

                        r = sd_bus_message_read(m, "b", &whitelist);
                        if (r < 0)
                                return bus_log_parse_error(r);

                        r = sd_bus_message_read_strv(m, &l);
                        if (r < 0)
                                return bus_log_parse_error(r);

                        r = sd_bus_message_exit_container(m);
                        if (r < 0)
                                return bus_log_parse_error(r);

                        if (arg_all || whitelist || !strv_isempty(l)) {
                                bool first = true;
                                char **i;

                                if (!arg_value) {
                                        fputs(name, stdout);
                                        fputc('=', stdout);
                                }

                                if (!whitelist)
                                        fputc('~', stdout);

                                STRV_FOREACH(i, l) {
                                        if (first)
                                                first = false;
                                        else
                                                fputc(' ', stdout);

                                        fputs(*i, stdout);
                                }
                                fputc('\n', stdout);
                        }

                        return 0;
                }

                break;

        case SD_BUS_TYPE_ARRAY:

                if (contents[1] == SD_BUS_TYPE_STRUCT_BEGIN && streq(name, "EnvironmentFiles")) {
                        const char *path;
                        int ignore;

                        r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "(sb)");
                        if (r < 0)
                                return bus_log_parse_error(r);

                        while ((r = sd_bus_message_read(m, "(sb)", &path, &ignore)) > 0)
                                print_prop(name, "%s (ignore_errors=%s)", path, yes_no(ignore));

                        if (r < 0)
                                return bus_log_parse_error(r);

                        r = sd_bus_message_exit_container(m);
                        if (r < 0)
                                return bus_log_parse_error(r);

                        return 0;

                } else if (contents[1] == SD_BUS_TYPE_STRUCT_BEGIN && streq(name, "Paths")) {
                        const char *type, *path;

                        r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "(ss)");
                        if (r < 0)
                                return bus_log_parse_error(r);

                        while ((r = sd_bus_message_read(m, "(ss)", &type, &path)) > 0)
                                print_prop(name, "%s (%s)", path, type);
                        if (r < 0)
                                return bus_log_parse_error(r);

                        r = sd_bus_message_exit_container(m);
                        if (r < 0)
                                return bus_log_parse_error(r);

                        return 0;

                } else if (contents[1] == SD_BUS_TYPE_STRUCT_BEGIN && streq(name, "Listen")) {
                        const char *type, *path;

                        r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "(ss)");
                        if (r < 0)
                                return bus_log_parse_error(r);

                        while ((r = sd_bus_message_read(m, "(ss)", &type, &path)) > 0)
                                print_prop(name, "%s (%s)", path, type);
                        if (r < 0)
                                return bus_log_parse_error(r);

                        r = sd_bus_message_exit_container(m);
                        if (r < 0)
                                return bus_log_parse_error(r);

                        return 0;

                } else if (contents[1] == SD_BUS_TYPE_STRUCT_BEGIN && streq(name, "TimersMonotonic")) {
                        const char *base;
                        uint64_t value, next_elapse;

                        r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "(stt)");
                        if (r < 0)
                                return bus_log_parse_error(r);

                        while ((r = sd_bus_message_read(m, "(stt)", &base, &value, &next_elapse)) > 0) {
                                char timespan1[FORMAT_TIMESPAN_MAX], timespan2[FORMAT_TIMESPAN_MAX];

                                print_prop(name, "{ %s=%s ; next_elapse=%s }", base,
                                           format_timespan(timespan1, sizeof(timespan1), value, 0),
                                           format_timespan(timespan2, sizeof(timespan2), next_elapse, 0));
                        }
                        if (r < 0)
                                return bus_log_parse_error(r);

                        r = sd_bus_message_exit_container(m);
                        if (r < 0)
                                return bus_log_parse_error(r);

                        return 0;

                } else if (contents[1] == SD_BUS_TYPE_STRUCT_BEGIN && streq(name, "TimersCalendar")) {
                        const char *base, *spec;
                        uint64_t next_elapse;

                        r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "(sst)");
                        if (r < 0)
                                return bus_log_parse_error(r);

                        while ((r = sd_bus_message_read(m, "(sst)", &base, &spec, &next_elapse)) > 0) {
                                char timestamp[FORMAT_TIMESTAMP_MAX];

                                print_prop(name, "{ %s=%s ; next_elapse=%s }", base, spec,
                                           format_timestamp(timestamp, sizeof(timestamp), next_elapse));
                        }
                        if (r < 0)
                                return bus_log_parse_error(r);

                        r = sd_bus_message_exit_container(m);
                        if (r < 0)
                                return bus_log_parse_error(r);

                        return 0;

                } else if (contents[1] == SD_BUS_TYPE_STRUCT_BEGIN && startswith(name, "Exec")) {
                        ExecStatusInfo info = {};

                        r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "(sasbttttuii)");
                        if (r < 0)
                                return bus_log_parse_error(r);

                        while ((r = exec_status_info_deserialize(m, &info)) > 0) {
                                char timestamp1[FORMAT_TIMESTAMP_MAX], timestamp2[FORMAT_TIMESTAMP_MAX];
                                _cleanup_free_ char *tt;

                                tt = strv_join(info.argv, " ");

                                print_prop(name,
                                           "{ path=%s ; argv[]=%s ; ignore_errors=%s ; start_time=[%s] ; stop_time=[%s] ; pid="PID_FMT" ; code=%s ; status=%i%s%s }",
                                           strna(info.path),
                                           strna(tt),
                                           yes_no(info.ignore),
                                           strna(format_timestamp(timestamp1, sizeof(timestamp1), info.start_timestamp)),
                                           strna(format_timestamp(timestamp2, sizeof(timestamp2), info.exit_timestamp)),
                                           info.pid,
                                           sigchld_code_to_string(info.code),
                                           info.status,
                                           info.code == CLD_EXITED ? "" : "/",
                                           strempty(info.code == CLD_EXITED ? NULL : signal_to_string(info.status)));

                                free(info.path);
                                strv_free(info.argv);
                                zero(info);
                        }

                        r = sd_bus_message_exit_container(m);
                        if (r < 0)
                                return bus_log_parse_error(r);

                        return 0;

                } else if (contents[1] == SD_BUS_TYPE_STRUCT_BEGIN && streq(name, "DeviceAllow")) {
                        const char *path, *rwm;

                        r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "(ss)");
                        if (r < 0)
                                return bus_log_parse_error(r);

                        while ((r = sd_bus_message_read(m, "(ss)", &path, &rwm)) > 0)
                                print_prop(name, "%s %s", strna(path), strna(rwm));
                        if (r < 0)
                                return bus_log_parse_error(r);

                        r = sd_bus_message_exit_container(m);
                        if (r < 0)
                                return bus_log_parse_error(r);

                        return 0;

                } else if (contents[1] == SD_BUS_TYPE_STRUCT_BEGIN &&
                           STR_IN_SET(name, "IODeviceWeight", "BlockIODeviceWeight")) {
                        const char *path;
                        uint64_t weight;

                        r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "(st)");
                        if (r < 0)
                                return bus_log_parse_error(r);

                        while ((r = sd_bus_message_read(m, "(st)", &path, &weight)) > 0)
                                print_prop(name, "%s %"PRIu64, strna(path), weight);
                        if (r < 0)
                                return bus_log_parse_error(r);

                        r = sd_bus_message_exit_container(m);
                        if (r < 0)
                                return bus_log_parse_error(r);

                        return 0;

                } else if (contents[1] == SD_BUS_TYPE_STRUCT_BEGIN &&
                           (cgroup_io_limit_type_from_string(name) >= 0 ||
                            STR_IN_SET(name, "BlockIOReadBandwidth", "BlockIOWriteBandwidth"))) {
                        const char *path;
                        uint64_t bandwidth;

                        r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "(st)");
                        if (r < 0)
                                return bus_log_parse_error(r);

                        while ((r = sd_bus_message_read(m, "(st)", &path, &bandwidth)) > 0)
                                print_prop(name, "%s %"PRIu64, strna(path), bandwidth);
                        if (r < 0)
                                return bus_log_parse_error(r);

                        r = sd_bus_message_exit_container(m);
                        if (r < 0)
                                return bus_log_parse_error(r);

                        return 0;

                } else if (contents[1] == SD_BUS_TYPE_BYTE && streq(name, "StandardInputData")) {
                        _cleanup_free_ char *h = NULL;
                        const void *p;
                        size_t sz;
                        ssize_t n;

                        r = sd_bus_message_read_array(m, 'y', &p, &sz);
                        if (r < 0)
                                return bus_log_parse_error(r);

                        n = base64mem(p, sz, &h);
                        if (n < 0)
                                return log_oom();

                        print_prop(name, "%s", h);

                        return 0;
                }

                break;
        }

        r = bus_print_property(name, m, arg_value, arg_all);
        if (r < 0)
                return bus_log_parse_error(r);

        if (r == 0) {
                r = sd_bus_message_skip(m, contents);
                if (r < 0)
                        return bus_log_parse_error(r);

                if (arg_all)
                        printf("%s=[unprintable]\n", name);
        }

        return 0;
}

static int show_one(
                const char *verb,
                sd_bus *bus,
                const char *path,
                const char *unit,
                bool show_properties,
                bool *new_line,
                bool *ellipsized) {

        static const struct bus_properties_map property_map[] = {
                { "LoadState",   "s", map_string_no_copy, offsetof(UnitStatusInfo, load_state)   },
                { "ActiveState", "s", map_string_no_copy, offsetof(UnitStatusInfo, active_state) },
                {}
        };

        _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_set_free_ Set *found_properties = NULL;
        _cleanup_(unit_status_info_free) UnitStatusInfo info = {
                .memory_current = (uint64_t) -1,
                .memory_high = CGROUP_LIMIT_MAX,
                .memory_max = CGROUP_LIMIT_MAX,
                .memory_swap_max = CGROUP_LIMIT_MAX,
                .memory_limit = (uint64_t) -1,
                .cpu_usage_nsec = (uint64_t) -1,
                .tasks_current = (uint64_t) -1,
                .tasks_max = (uint64_t) -1,
                .ip_ingress_bytes = (uint64_t) -1,
                .ip_egress_bytes = (uint64_t) -1,
        };
        int r;

        assert(path);
        assert(new_line);

        log_debug("Showing one %s", path);

        r = sd_bus_call_method(
                        bus,
                        "org.freedesktop.systemd1",
                        path,
                        "org.freedesktop.DBus.Properties",
                        "GetAll",
                        &error,
                        &reply,
                        "s", "");
        if (r < 0)
                return log_error_errno(r, "Failed to get properties: %s", bus_error_message(&error, r));

        if (unit) {
                r = bus_message_map_all_properties(reply, property_map, &error, &info);
                if (r < 0)
                        return log_error_errno(r, "Failed to map properties: %s", bus_error_message(&error, r));

                if (streq_ptr(info.load_state, "not-found") && streq_ptr(info.active_state, "inactive")) {
                        log_full(streq(verb, "status") ? LOG_ERR : LOG_DEBUG,
                                 "Unit %s could not be found.", unit);

                        if (streq(verb, "status"))
                                return EXIT_PROGRAM_OR_SERVICES_STATUS_UNKNOWN;

                        if (!streq(verb, "show"))
                                return -ENOENT;
                }

                r = sd_bus_message_rewind(reply, true);
                if (r < 0)
                        return log_error_errno(r, "Failed to rewind: %s", bus_error_message(&error, r));
        }

        r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "{sv}");
        if (r < 0)
                return bus_log_parse_error(r);

        if (*new_line)
                printf("\n");

        *new_line = true;

        while ((r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_DICT_ENTRY, "sv")) > 0) {
                const char *name, *contents;

                r = sd_bus_message_read(reply, "s", &name);
                if (r < 0)
                        return bus_log_parse_error(r);

                r = sd_bus_message_peek_type(reply, NULL, &contents);
                if (r < 0)
                        return bus_log_parse_error(r);

                r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_VARIANT, contents);
                if (r < 0)
                        return bus_log_parse_error(r);

                if (show_properties) {
                        r = set_ensure_allocated(&found_properties, &string_hash_ops);
                        if (r < 0)
                                return log_oom();

                        r = set_put(found_properties, name);
                        if (r < 0 && r != EEXIST)
                                return log_oom();

                        r = print_property(name, reply, contents);
                } else
                        r = status_property(name, reply, &info, contents);
                if (r < 0)
                        return r;

                r = sd_bus_message_exit_container(reply);
                if (r < 0)
                        return bus_log_parse_error(r);

                r = sd_bus_message_exit_container(reply);
                if (r < 0)
                        return bus_log_parse_error(r);
        }
        if (r < 0)
                return bus_log_parse_error(r);

        r = sd_bus_message_exit_container(reply);
        if (r < 0)
                return bus_log_parse_error(r);

        r = 0;
        if (show_properties) {
                char **pp;

                STRV_FOREACH(pp, arg_properties)
                        if (!set_contains(found_properties, *pp))
                                log_debug("Property %s does not exist.", *pp);

        } else if (streq(verb, "help"))
                show_unit_help(&info);
        else if (streq(verb, "status")) {
                print_status_info(bus, &info, ellipsized);

                if (info.active_state && !STR_IN_SET(info.active_state, "active", "reloading"))
                        r = EXIT_PROGRAM_NOT_RUNNING;
                else
                        r = EXIT_PROGRAM_RUNNING_OR_SERVICE_OK;
        }

        return r;
}

static int get_unit_dbus_path_by_pid(
                sd_bus *bus,
                uint32_t pid,
                char **unit) {

        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
        char *u;
        int r;

        r = sd_bus_call_method(
                        bus,
                        "org.freedesktop.systemd1",
                        "/org/freedesktop/systemd1",
                        "org.freedesktop.systemd1.Manager",
                        "GetUnitByPID",
                        &error,
                        &reply,
                        "u", pid);
        if (r < 0)
                return log_error_errno(r, "Failed to get unit for PID %"PRIu32": %s", pid, bus_error_message(&error, r));

        r = sd_bus_message_read(reply, "o", &u);
        if (r < 0)
                return bus_log_parse_error(r);

        u = strdup(u);
        if (!u)
                return log_oom();

        *unit = u;
        return 0;
}

static int show_all(
                const char* verb,
                sd_bus *bus,
                bool show_properties,
                bool *new_line,
                bool *ellipsized) {

        _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
        _cleanup_free_ UnitInfo *unit_infos = NULL;
        const UnitInfo *u;
        unsigned c;
        int r, ret = 0;

        r = get_unit_list(bus, NULL, NULL, &unit_infos, 0, &reply);
        if (r < 0)
                return r;

        pager_open(arg_no_pager, false);

        c = (unsigned) r;

        qsort_safe(unit_infos, c, sizeof(UnitInfo), compare_unit_info);

        for (u = unit_infos; u < unit_infos + c; u++) {
                _cleanup_free_ char *p = NULL;

                p = unit_dbus_path_from_name(u->id);
                if (!p)
                        return log_oom();

                r = show_one(verb, bus, p, u->id, show_properties, new_line, ellipsized);
                if (r < 0)
                        return r;
                else if (r > 0 && ret == 0)
                        ret = r;
        }

        return ret;
}

static int show_system_status(sd_bus *bus) {
        char since1[FORMAT_TIMESTAMP_RELATIVE_MAX], since2[FORMAT_TIMESTAMP_MAX];
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_(machine_info_clear) struct machine_info mi = {};
        _cleanup_free_ char *hn = NULL;
        const char *on, *off;
        int r;

        hn = gethostname_malloc();
        if (!hn)
                return log_oom();

        r = bus_map_all_properties(bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1", machine_info_property_map, &error, &mi);
        if (r < 0)
                return log_error_errno(r, "Failed to read server status: %s", bus_error_message(&error, r));

        if (streq_ptr(mi.state, "degraded")) {
                on = ansi_highlight_red();
                off = ansi_normal();
        } else if (streq_ptr(mi.state, "running")) {
                on = ansi_highlight_green();
                off = ansi_normal();
        } else {
                on = ansi_highlight_yellow();
                off = ansi_normal();
        }

        printf("%s%s%s %s\n", on, special_glyph(BLACK_CIRCLE), off, arg_host ? arg_host : hn);

        printf("    State: %s%s%s\n",
               on, strna(mi.state), off);

        printf("     Jobs: %" PRIu32 " queued\n", mi.n_jobs);
        printf("   Failed: %" PRIu32 " units\n", mi.n_failed_units);

        printf("    Since: %s; %s\n",
               format_timestamp(since2, sizeof(since2), mi.timestamp),
               format_timestamp_relative(since1, sizeof(since1), mi.timestamp));

        printf("   CGroup: %s\n", mi.control_group ?: "/");
        if (IN_SET(arg_transport,
                   BUS_TRANSPORT_LOCAL,
                   BUS_TRANSPORT_MACHINE)) {
                static const char prefix[] = "           ";
                unsigned c;

                c = columns();
                if (c > sizeof(prefix) - 1)
                        c -= sizeof(prefix) - 1;
                else
                        c = 0;

                show_cgroup(SYSTEMD_CGROUP_CONTROLLER, strempty(mi.control_group), prefix, c, get_output_flags());
        }

        return 0;
}

static int show(int argc, char *argv[], void *userdata) {
        bool show_properties, show_status, show_help, new_line = false;
        bool ellipsized = false;
        int r, ret = 0;
        sd_bus *bus;

        assert(argv);

        show_properties = streq(argv[0], "show");
        show_status = streq(argv[0], "status");
        show_help = streq(argv[0], "help");

        if (show_help && argc <= 1) {
                log_error("This command expects one or more unit names. Did you mean --help?");
                return -EINVAL;
        }

        r = acquire_bus(BUS_MANAGER, &bus);
        if (r < 0)
                return r;

        pager_open(arg_no_pager, false);

        if (show_status)
                /* Increase max number of open files to 16K if we can, we
                 * might needs this when browsing journal files, which might
                 * be split up into many files. */
                setrlimit_closest(RLIMIT_NOFILE, &RLIMIT_MAKE_CONST(16384));

        /* If no argument is specified inspect the manager itself */
        if (show_properties && argc <= 1)
                return show_one(argv[0], bus, "/org/freedesktop/systemd1", NULL, show_properties, &new_line, &ellipsized);

        if (show_status && argc <= 1) {

                show_system_status(bus);
                new_line = true;

                if (arg_all)
                        ret = show_all(argv[0], bus, false, &new_line, &ellipsized);
        } else {
                _cleanup_free_ char **patterns = NULL;
                char **name;

                STRV_FOREACH(name, strv_skip(argv, 1)) {
                        _cleanup_free_ char *path = NULL, *unit = NULL;
                        uint32_t id;

                        if (safe_atou32(*name, &id) < 0) {
                                if (strv_push(&patterns, *name) < 0)
                                        return log_oom();

                                continue;
                        } else if (show_properties) {
                                /* Interpret as job id */
                                if (asprintf(&path, "/org/freedesktop/systemd1/job/%u", id) < 0)
                                        return log_oom();

                        } else {
                                /* Interpret as PID */
                                r = get_unit_dbus_path_by_pid(bus, id, &path);
                                if (r < 0) {
                                        ret = r;
                                        continue;
                                }

                                r = unit_name_from_dbus_path(path, &unit);
                                if (r < 0)
                                        return log_oom();
                        }

                        r = show_one(argv[0], bus, path, unit, show_properties, &new_line, &ellipsized);
                        if (r < 0)
                                return r;
                        else if (r > 0 && ret == 0)
                                ret = r;
                }

                if (!strv_isempty(patterns)) {
                        _cleanup_strv_free_ char **names = NULL;

                        r = expand_names(bus, patterns, NULL, &names);
                        if (r < 0)
                                return log_error_errno(r, "Failed to expand names: %m");

                        STRV_FOREACH(name, names) {
                                _cleanup_free_ char *path;

                                path = unit_dbus_path_from_name(*name);
                                if (!path)
                                        return log_oom();

                                r = show_one(argv[0], bus, path, *name, show_properties, &new_line, &ellipsized);
                                if (r < 0)
                                        return r;
                                if (r > 0 && ret == 0)
                                        ret = r;
                        }
                }
        }

        if (ellipsized && !arg_quiet)
                printf("Hint: Some lines were ellipsized, use -l to show in full.\n");

        return ret;
}

static int cat_file(const char *filename, bool newline) {
        _cleanup_close_ int fd;

        fd = open(filename, O_RDONLY|O_CLOEXEC|O_NOCTTY);
        if (fd < 0)
                return -errno;

        printf("%s%s# %s%s\n",
               newline ? "\n" : "",
               ansi_highlight_blue(),
               filename,
               ansi_normal());
        fflush(stdout);

        return copy_bytes(fd, STDOUT_FILENO, (uint64_t) -1, 0);
}

static int cat(int argc, char *argv[], void *userdata) {
        _cleanup_lookup_paths_free_ LookupPaths lp = {};
        _cleanup_strv_free_ char **names = NULL;
        char **name;
        sd_bus *bus;
        bool first = true;
        int r;

        if (arg_transport != BUS_TRANSPORT_LOCAL) {
                log_error("Cannot remotely cat units.");
                return -EINVAL;
        }

        r = lookup_paths_init(&lp, arg_scope, 0, arg_root);
        if (r < 0)
                return log_error_errno(r, "Failed to determine unit paths: %m");

        r = acquire_bus(BUS_MANAGER, &bus);
        if (r < 0)
                return r;

        r = expand_names(bus, strv_skip(argv, 1), NULL, &names);
        if (r < 0)
                return log_error_errno(r, "Failed to expand names: %m");

        pager_open(arg_no_pager, false);

        STRV_FOREACH(name, names) {
                _cleanup_free_ char *fragment_path = NULL;
                _cleanup_strv_free_ char **dropin_paths = NULL;
                char **path;

                r = unit_find_paths(bus, *name, &lp, &fragment_path, &dropin_paths);
                if (r < 0)
                        return r;
                else if (r == 0)
                        return -ENOENT;

                if (first)
                        first = false;
                else
                        puts("");

                if (need_daemon_reload(bus, *name) > 0) /* ignore errors (<0), this is informational output */
                        fprintf(stderr,
                                "%s# Warning: %s changed on disk, the version systemd has loaded is outdated.\n"
                                "%s# This output shows the current version of the unit's original fragment and drop-in files.\n"
                                "%s# If fragments or drop-ins were added or removed, they are not properly reflected in this output.\n"
                                "%s# Run 'systemctl%s daemon-reload' to reload units.%s\n",
                                ansi_highlight_red(),
                                *name,
                                ansi_highlight_red(),
                                ansi_highlight_red(),
                                ansi_highlight_red(),
                                arg_scope == UNIT_FILE_SYSTEM ? "" : " --user",
                                ansi_normal());

                if (fragment_path) {
                        r = cat_file(fragment_path, false);
                        if (r < 0)
                                return log_warning_errno(r, "Failed to cat %s: %m", fragment_path);
                }

                STRV_FOREACH(path, dropin_paths) {
                        r = cat_file(*path, path == dropin_paths);
                        if (r < 0)
                                return log_warning_errno(r, "Failed to cat %s: %m", *path);
                }
        }

        return 0;
}

static int set_property(int argc, char *argv[], void *userdata) {
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *m = NULL;
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_free_ char *n = NULL;
        UnitType t;
        sd_bus *bus;
        int r;

        r = acquire_bus(BUS_MANAGER, &bus);
        if (r < 0)
                return r;

        polkit_agent_open_maybe();

        r = sd_bus_message_new_method_call(
                        bus,
                        &m,
                        "org.freedesktop.systemd1",
                        "/org/freedesktop/systemd1",
                        "org.freedesktop.systemd1.Manager",
                        "SetUnitProperties");
        if (r < 0)
                return bus_log_create_error(r);

        r = unit_name_mangle(argv[1], UNIT_NAME_NOGLOB, &n);
        if (r < 0)
                return log_error_errno(r, "Failed to mangle unit name: %m");

        t = unit_name_to_type(n);
        if (t < 0) {
                log_error("Invalid unit type: %s", n);
                return -EINVAL;
        }

        r = sd_bus_message_append(m, "sb", n, arg_runtime);
        if (r < 0)
                return bus_log_create_error(r);

        r = sd_bus_message_open_container(m, SD_BUS_TYPE_ARRAY, "(sv)");
        if (r < 0)
                return bus_log_create_error(r);

        r = bus_append_unit_property_assignment_many(m, t, strv_skip(argv, 2));
        if (r < 0)
                return r;

        r = sd_bus_message_close_container(m);
        if (r < 0)
                return bus_log_create_error(r);

        r = sd_bus_call(bus, m, 0, &error, NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to set unit properties on %s: %s", n, bus_error_message(&error, r));

        return 0;
}

static int daemon_reload(int argc, char *argv[], void *userdata) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *m = NULL;
        const char *method;
        sd_bus *bus;
        int r;

        r = acquire_bus(BUS_MANAGER, &bus);
        if (r < 0)
                return r;

        polkit_agent_open_maybe();

        switch (arg_action) {

        case ACTION_RELOAD:
                method = "Reload";
                break;

        case ACTION_REEXEC:
                method = "Reexecute";
                break;

        case ACTION_SYSTEMCTL:
                method = streq(argv[0], "daemon-reexec") ? "Reexecute" :
                                     /* "daemon-reload" */ "Reload";
                break;

        default:
                assert_not_reached("Unexpected action");
        }

        r = sd_bus_message_new_method_call(
                        bus,
                        &m,
                        "org.freedesktop.systemd1",
                        "/org/freedesktop/systemd1",
                        "org.freedesktop.systemd1.Manager",
                        method);
        if (r < 0)
                return bus_log_create_error(r);

        /* Note we use an extra-long timeout here. This is because a reload or reexec means generators are rerun which
         * are timed out after DEFAULT_TIMEOUT_USEC. Let's use twice that time here, so that the generators can have
         * their timeout, and for everything else there's the same time budget in place. */

        r = sd_bus_call(bus, m, DEFAULT_TIMEOUT_USEC * 2, &error, NULL);

        /* On reexecution, we expect a disconnect, not a reply */
        if (IN_SET(r, -ETIMEDOUT, -ECONNRESET) && streq(method, "Reexecute"))
                r = 0;

        if (r < 0 && arg_action == ACTION_SYSTEMCTL)
                return log_error_errno(r, "Failed to reload daemon: %s", bus_error_message(&error, r));

        /* Note that for the legacy commands (i.e. those with action != ACTION_SYSTEMCTL) we support fallbacks to the
         * old ways of doing things, hence don't log any error in that case here. */

        return r < 0 ? r : 0;
}

static int trivial_method(int argc, char *argv[], void *userdata) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        const char *method;
        sd_bus *bus;
        int r;

        if (arg_dry_run)
                return 0;

        r = acquire_bus(BUS_MANAGER, &bus);
        if (r < 0)
                return r;

        polkit_agent_open_maybe();

        method =
                streq(argv[0], "clear-jobs")    ||
                streq(argv[0], "cancel")        ? "ClearJobs" :
                streq(argv[0], "reset-failed")  ? "ResetFailed" :
                streq(argv[0], "halt")          ? "Halt" :
                streq(argv[0], "reboot")        ? "Reboot" :
                streq(argv[0], "kexec")         ? "KExec" :
                streq(argv[0], "exit")          ? "Exit" :
                             /* poweroff */       "PowerOff";

        r = sd_bus_call_method(
                        bus,
                        "org.freedesktop.systemd1",
                        "/org/freedesktop/systemd1",
                        "org.freedesktop.systemd1.Manager",
                        method,
                        &error,
                        NULL,
                        NULL);
        if (r < 0 && arg_action == ACTION_SYSTEMCTL)
                return log_error_errno(r, "Failed to execute operation: %s", bus_error_message(&error, r));

        /* Note that for the legacy commands (i.e. those with action != ACTION_SYSTEMCTL) we support fallbacks to the
         * old ways of doing things, hence don't log any error in that case here. */

        return r < 0 ? r : 0;
}

static int reset_failed(int argc, char *argv[], void *userdata) {
        _cleanup_strv_free_ char **names = NULL;
        sd_bus *bus;
        char **name;
        int r, q;

        if (argc <= 1)
                return trivial_method(argc, argv, userdata);

        r = acquire_bus(BUS_MANAGER, &bus);
        if (r < 0)
                return r;

        polkit_agent_open_maybe();

        r = expand_names(bus, strv_skip(argv, 1), NULL, &names);
        if (r < 0)
                return log_error_errno(r, "Failed to expand names: %m");

        STRV_FOREACH(name, names) {
                _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;

                q = sd_bus_call_method(
                                bus,
                                "org.freedesktop.systemd1",
                                "/org/freedesktop/systemd1",
                                "org.freedesktop.systemd1.Manager",
                                "ResetFailedUnit",
                                &error,
                                NULL,
                                "s", *name);
                if (q < 0) {
                        log_error_errno(q, "Failed to reset failed state of unit %s: %s", *name, bus_error_message(&error, q));
                        if (r == 0)
                                r = q;
                }
        }

        return r;
}

static int print_variable(const char *s) {
        const char *sep;
        _cleanup_free_ char *esc = NULL;

        sep = strchr(s, '=');
        if (!sep) {
                log_error("Invalid environment block");
                return -EUCLEAN;
        }

        esc = shell_maybe_quote(sep + 1, ESCAPE_POSIX);
        if (!esc)
                return log_oom();

        printf("%.*s=%s\n", (int)(sep-s), s, esc);
        return 0;
}

static int show_environment(int argc, char *argv[], void *userdata) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
        const char *text;
        sd_bus *bus;
        int r;

        r = acquire_bus(BUS_MANAGER, &bus);
        if (r < 0)
                return r;

        pager_open(arg_no_pager, false);

        r = sd_bus_get_property(
                        bus,
                        "org.freedesktop.systemd1",
                        "/org/freedesktop/systemd1",
                        "org.freedesktop.systemd1.Manager",
                        "Environment",
                        &error,
                        &reply,
                        "as");
        if (r < 0)
                return log_error_errno(r, "Failed to get environment: %s", bus_error_message(&error, r));

        r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "s");
        if (r < 0)
                return bus_log_parse_error(r);

        while ((r = sd_bus_message_read_basic(reply, SD_BUS_TYPE_STRING, &text)) > 0) {
                r = print_variable(text);
                if (r < 0)
                        return r;
        }
        if (r < 0)
                return bus_log_parse_error(r);

        r = sd_bus_message_exit_container(reply);
        if (r < 0)
                return bus_log_parse_error(r);

        return 0;
}

static int switch_root(int argc, char *argv[], void *userdata) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_free_ char *cmdline_init = NULL;
        const char *root, *init;
        sd_bus *bus;
        int r;

        if (arg_transport != BUS_TRANSPORT_LOCAL) {
                log_error("Cannot switch root remotely.");
                return -EINVAL;
        }

        if (argc < 2 || argc > 3) {
                log_error("Wrong number of arguments.");
                return -EINVAL;
        }

        root = argv[1];

        if (argc >= 3)
                init = argv[2];
        else {
                r = parse_env_file("/proc/cmdline", WHITESPACE,
                                   "init", &cmdline_init,
                                   NULL);
                if (r < 0)
                        log_debug_errno(r, "Failed to parse /proc/cmdline: %m");

                init = cmdline_init;
        }

        init = empty_to_null(init);
        if (init) {
                const char *root_systemd_path = NULL, *root_init_path = NULL;

                root_systemd_path = strjoina(root, "/" SYSTEMD_BINARY_PATH);
                root_init_path = strjoina(root, "/", init);

                /* If the passed init is actually the same as the
                 * systemd binary, then let's suppress it. */
                if (files_same(root_init_path, root_systemd_path, 0) > 0)
                        init = NULL;
        }

        /* Instruct PID1 to exclude us from its killing spree applied during
         * the transition. Otherwise we would exit with a failure status even
         * though the switch to the new root has succeed. */
        argv_cmdline[0] = '@';

        r = acquire_bus(BUS_MANAGER, &bus);
        if (r < 0)
                return r;

        /* If we are slow to exit after the root switch, the new systemd instance
         * will send us a signal to terminate. Just ignore it and exit normally.
         * This way the unit does not end up as failed.
         */
        r = ignore_signals(SIGTERM, -1);
        if (r < 0)
                log_warning_errno(r, "Failed to change disposition of SIGTERM to ignore: %m");

        log_debug("Switching root - root: %s; init: %s", root, strna(init));

        r = sd_bus_call_method(
                        bus,
                        "org.freedesktop.systemd1",
                        "/org/freedesktop/systemd1",
                        "org.freedesktop.systemd1.Manager",
                        "SwitchRoot",
                        &error,
                        NULL,
                        "ss", root, init);
        if (r < 0) {
                (void) default_signals(SIGTERM, -1);

                return log_error_errno(r, "Failed to switch root: %s", bus_error_message(&error, r));
        }

        return 0;
}

static int set_environment(int argc, char *argv[], void *userdata) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *m = NULL;
        const char *method;
        sd_bus *bus;
        int r;

        assert(argc > 1);
        assert(argv);

        r = acquire_bus(BUS_MANAGER, &bus);
        if (r < 0)
                return r;

        polkit_agent_open_maybe();

        method = streq(argv[0], "set-environment")
                ? "SetEnvironment"
                : "UnsetEnvironment";

        r = sd_bus_message_new_method_call(
                        bus,
                        &m,
                        "org.freedesktop.systemd1",
                        "/org/freedesktop/systemd1",
                        "org.freedesktop.systemd1.Manager",
                        method);
        if (r < 0)
                return bus_log_create_error(r);

        r = sd_bus_message_append_strv(m, strv_skip(argv, 1));
        if (r < 0)
                return bus_log_create_error(r);

        r = sd_bus_call(bus, m, 0, &error, NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to set environment: %s", bus_error_message(&error, r));

        return 0;
}

static int import_environment(int argc, char *argv[], void *userdata) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *m = NULL;
        sd_bus *bus;
        int r;

        r = acquire_bus(BUS_MANAGER, &bus);
        if (r < 0)
                return r;

        polkit_agent_open_maybe();

        r = sd_bus_message_new_method_call(
                        bus,
                        &m,
                        "org.freedesktop.systemd1",
                        "/org/freedesktop/systemd1",
                        "org.freedesktop.systemd1.Manager",
                        "SetEnvironment");
        if (r < 0)
                return bus_log_create_error(r);

        if (argc < 2)
                r = sd_bus_message_append_strv(m, environ);
        else {
                char **a, **b;

                r = sd_bus_message_open_container(m, 'a', "s");
                if (r < 0)
                        return bus_log_create_error(r);

                STRV_FOREACH(a, strv_skip(argv, 1)) {

                        if (!env_name_is_valid(*a)) {
                                log_error("Not a valid environment variable name: %s", *a);
                                return -EINVAL;
                        }

                        STRV_FOREACH(b, environ) {
                                const char *eq;

                                eq = startswith(*b, *a);
                                if (eq && *eq == '=') {

                                        r = sd_bus_message_append(m, "s", *b);
                                        if (r < 0)
                                                return bus_log_create_error(r);

                                        break;
                                }
                        }
                }

                r = sd_bus_message_close_container(m);
        }
        if (r < 0)
                return bus_log_create_error(r);

        r = sd_bus_call(bus, m, 0, &error, NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to import environment: %s", bus_error_message(&error, r));

        return 0;
}

static int enable_sysv_units(const char *verb, char **args) {
        int r = 0;

#if HAVE_SYSV_COMPAT
        _cleanup_lookup_paths_free_ LookupPaths paths = {};
        unsigned f = 0;

        /* Processes all SysV units, and reshuffles the array so that afterwards only the native units remain */

        if (arg_scope != UNIT_FILE_SYSTEM)
                return 0;

        if (getenv_bool("SYSTEMCTL_SKIP_SYSV") > 0)
                return 0;

        if (!STR_IN_SET(verb,
                        "enable",
                        "disable",
                        "is-enabled"))
                return 0;

        r = lookup_paths_init(&paths, arg_scope, LOOKUP_PATHS_EXCLUDE_GENERATED, arg_root);
        if (r < 0)
                return r;

        r = 0;
        while (args[f]) {

                const char *argv[] = {
                        ROOTLIBEXECDIR "/systemd-sysv-install",
                        NULL,
                        NULL,
                        NULL,
                        NULL,
                };

                _cleanup_free_ char *p = NULL, *q = NULL, *l = NULL;
                bool found_native = false, found_sysv;
                const char *name;
                unsigned c = 1;
                pid_t pid;
                int j;

                name = args[f++];

                if (!endswith(name, ".service"))
                        continue;

                if (path_is_absolute(name))
                        continue;

                j = unit_file_exists(arg_scope, &paths, name);
                if (j < 0 && !IN_SET(j, -ELOOP, -ERFKILL, -EADDRNOTAVAIL))
                        return log_error_errno(j, "Failed to lookup unit file state: %m");
                found_native = j != 0;

                /* If we have both a native unit and a SysV script, enable/disable them both (below); for is-enabled,
                 * prefer the native unit */
                if (found_native && streq(verb, "is-enabled"))
                        continue;

                p = path_join(arg_root, SYSTEM_SYSVINIT_PATH, name);
                if (!p)
                        return log_oom();

                p[strlen(p) - STRLEN(".service")] = 0;
                found_sysv = access(p, F_OK) >= 0;
                if (!found_sysv)
                        continue;

                if (!arg_quiet) {
                        if (found_native)
                                log_info("Synchronizing state of %s with SysV service script with %s.", name, argv[0]);
                        else
                                log_info("%s is not a native service, redirecting to systemd-sysv-install.", name);
                }

                if (!isempty(arg_root))
                        argv[c++] = q = strappend("--root=", arg_root);

                argv[c++] = verb;
                argv[c++] = basename(p);
                argv[c] = NULL;

                l = strv_join((char**)argv, " ");
                if (!l)
                        return log_oom();

                if (!arg_quiet)
                        log_info("Executing: %s", l);

                j = safe_fork("(sysv-install)", FORK_RESET_SIGNALS|FORK_DEATHSIG|FORK_LOG, &pid);
                if (j < 0)
                        return j;
                if (j == 0) {
                        /* Child */
                        execv(argv[0], (char**) argv);
                        log_error_errno(errno, "Failed to execute %s: %m", argv[0]);
                        _exit(EXIT_FAILURE);
                }

                j = wait_for_terminate_and_check("sysv-install", pid, WAIT_LOG_ABNORMAL);
                if (j < 0)
                        return j;
                if (streq(verb, "is-enabled")) {
                        if (j == EXIT_SUCCESS) {
                                if (!arg_quiet)
                                        puts("enabled");
                                r = 1;
                        } else {
                                if (!arg_quiet)
                                        puts("disabled");
                        }

                } else if (j != EXIT_SUCCESS)
                        return -EBADE; /* We don't warn here, under the assumption the script already showed an explanation */

                if (found_native)
                        continue;

                /* Remove this entry, so that we don't try enabling it as native unit */
                assert(f > 0);
                f--;
                assert(args[f] == name);
                strv_remove(args, name);
        }

#endif
        return r;
}

static int mangle_names(char **original_names, char ***mangled_names) {
        char **i, **l, **name;
        int r;

        l = i = new(char*, strv_length(original_names) + 1);
        if (!l)
                return log_oom();

        STRV_FOREACH(name, original_names) {

                /* When enabling units qualified path names are OK,
                 * too, hence allow them explicitly. */

                if (is_path(*name)) {
                        *i = strdup(*name);
                        if (!*i) {
                                strv_free(l);
                                return log_oom();
                        }
                } else {
                        r = unit_name_mangle(*name, UNIT_NAME_NOGLOB, i);
                        if (r < 0) {
                                *i = NULL;
                                strv_free(l);
                                return log_error_errno(r, "Failed to mangle unit name: %m");
                        }
                }

                i++;
        }

        *i = NULL;
        *mangled_names = l;

        return 0;
}

static int normalize_filenames(char **names) {
        char **u;
        int r;

        STRV_FOREACH(u, names)
                if (!path_is_absolute(*u)) {
                        char* normalized_path;

                        if (!isempty(arg_root)) {
                                log_error("Non-absolute paths are not allowed when --root is used: %s", *u);
                                return -EINVAL;
                        }

                        if (!strchr(*u,'/')) {
                                log_error("Link argument does contain at least one directory separator: %s", *u);
                                return -EINVAL;
                        }

                        r = path_make_absolute_cwd(*u, &normalized_path);
                        if (r < 0)
                                return r;

                        free_and_replace(*u, normalized_path);
                }

        return 0;
}

static int normalize_names(char **names, bool warn_if_path) {
        char **u;
        bool was_path = false;

        STRV_FOREACH(u, names) {
                int r;

                if (!is_path(*u))
                        continue;

                r = free_and_strdup(u, basename(*u));
                if (r < 0)
                        return log_error_errno(r, "Failed to normalize unit file path: %m");

                was_path = true;
        }

        if (warn_if_path && was_path)
                log_warning("Warning: Can't execute disable on the unit file path. Proceeding with the unit name.");

        return 0;
}

static int unit_exists(LookupPaths *lp, const char *unit) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_free_ char *path = NULL;
        static const struct bus_properties_map property_map[] = {
                { "LoadState",   "s", map_string_no_copy, offsetof(UnitStatusInfo, load_state)  },
                { "ActiveState", "s", map_string_no_copy, offsetof(UnitStatusInfo, active_state)},
                {},
        };
        UnitStatusInfo info = {};
        sd_bus *bus;
        int r;

        if (unit_name_is_valid(unit, UNIT_NAME_TEMPLATE))
                return unit_find_template_path(unit, lp, NULL, NULL);

        path = unit_dbus_path_from_name(unit);
        if (!path)
                return log_oom();

        r = acquire_bus(BUS_MANAGER, &bus);
        if (r < 0)
                return r;

        r = bus_map_all_properties(bus, "org.freedesktop.systemd1", path, property_map, &error, &info);
        if (r < 0)
                return log_error_errno(r, "Failed to get properties: %s", bus_error_message(&error, r));

        return !streq_ptr(info.load_state, "not-found") || !streq_ptr(info.active_state, "inactive");
}

static int enable_unit(int argc, char *argv[], void *userdata) {
        _cleanup_strv_free_ char **names = NULL;
        const char *verb = argv[0];
        UnitFileChange *changes = NULL;
        unsigned n_changes = 0;
        int carries_install_info = -1;
        bool ignore_carries_install_info = arg_quiet;
        int r;

        if (!argv[1])
                return 0;

        r = mangle_names(strv_skip(argv, 1), &names);
        if (r < 0)
                return r;

        r = enable_sysv_units(verb, names);
        if (r < 0)
                return r;

        /* If the operation was fully executed by the SysV compat, let's finish early */
        if (strv_isempty(names)) {
                if (arg_no_reload || install_client_side())
                        return 0;
                return daemon_reload(argc, argv, userdata);
        }

        if (streq(verb, "disable")) {
                r = normalize_names(names, true);
                if (r < 0)
                        return r;
        }

        if (streq(verb, "link")) {
                r = normalize_filenames(names);
                if (r < 0)
                        return r;
        }

        if (install_client_side()) {
                UnitFileFlags flags;

                flags = args_to_flags();
                if (streq(verb, "enable")) {
                        r = unit_file_enable(arg_scope, flags, arg_root, names, &changes, &n_changes);
                        carries_install_info = r;
                } else if (streq(verb, "disable"))
                        r = unit_file_disable(arg_scope, flags, arg_root, names, &changes, &n_changes);
                else if (streq(verb, "reenable")) {
                        r = unit_file_reenable(arg_scope, flags, arg_root, names, &changes, &n_changes);
                        carries_install_info = r;
                } else if (streq(verb, "link"))
                        r = unit_file_link(arg_scope, flags, arg_root, names, &changes, &n_changes);
                else if (streq(verb, "preset")) {
                        r = unit_file_preset(arg_scope, flags, arg_root, names, arg_preset_mode, &changes, &n_changes);
                } else if (streq(verb, "mask"))
                        r = unit_file_mask(arg_scope, flags, arg_root, names, &changes, &n_changes);
                else if (streq(verb, "unmask"))
                        r = unit_file_unmask(arg_scope, flags, arg_root, names, &changes, &n_changes);
                else if (streq(verb, "revert"))
                        r = unit_file_revert(arg_scope, arg_root, names, &changes, &n_changes);
                else
                        assert_not_reached("Unknown verb");

                unit_file_dump_changes(r, verb, changes, n_changes, arg_quiet);
                if (r < 0)
                        goto finish;
                r = 0;
        } else {
                _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL, *m = NULL;
                _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
                bool expect_carries_install_info = false;
                bool send_runtime = true, send_force = true, send_preset_mode = false;
                const char *method;
                sd_bus *bus;

                if (STR_IN_SET(verb, "mask", "unmask")) {
                        char **name;
                        _cleanup_lookup_paths_free_ LookupPaths lp = {};

                        r = lookup_paths_init(&lp, arg_scope, 0, arg_root);
                        if (r < 0)
                                return r;

                        STRV_FOREACH(name, names) {
                                r = unit_exists(&lp, *name);
                                if (r < 0)
                                        return r;
                                if (r == 0)
                                        log_notice("Unit %s does not exist, proceeding anyway.", *names);
                        }
                }

                r = acquire_bus(BUS_MANAGER, &bus);
                if (r < 0)
                        return r;

                polkit_agent_open_maybe();

                if (streq(verb, "enable")) {
                        method = "EnableUnitFiles";
                        expect_carries_install_info = true;
                } else if (streq(verb, "disable")) {
                        method = "DisableUnitFiles";
                        send_force = false;
                } else if (streq(verb, "reenable")) {
                        method = "ReenableUnitFiles";
                        expect_carries_install_info = true;
                } else if (streq(verb, "link"))
                        method = "LinkUnitFiles";
                else if (streq(verb, "preset")) {

                        if (arg_preset_mode != UNIT_FILE_PRESET_FULL) {
                                method = "PresetUnitFilesWithMode";
                                send_preset_mode = true;
                        } else
                                method = "PresetUnitFiles";

                        expect_carries_install_info = true;
                        ignore_carries_install_info = true;
                } else if (streq(verb, "mask"))
                        method = "MaskUnitFiles";
                else if (streq(verb, "unmask")) {
                        method = "UnmaskUnitFiles";
                        send_force = false;
                } else if (streq(verb, "revert")) {
                        method = "RevertUnitFiles";
                        send_runtime = send_force = false;
                } else
                        assert_not_reached("Unknown verb");

                r = sd_bus_message_new_method_call(
                                bus,
                                &m,
                                "org.freedesktop.systemd1",
                                "/org/freedesktop/systemd1",
                                "org.freedesktop.systemd1.Manager",
                                method);
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_append_strv(m, names);
                if (r < 0)
                        return bus_log_create_error(r);

                if (send_preset_mode) {
                        r = sd_bus_message_append(m, "s", unit_file_preset_mode_to_string(arg_preset_mode));
                        if (r < 0)
                                return bus_log_create_error(r);
                }

                if (send_runtime) {
                        r = sd_bus_message_append(m, "b", arg_runtime);
                        if (r < 0)
                                return bus_log_create_error(r);
                }

                if (send_force) {
                        r = sd_bus_message_append(m, "b", arg_force);
                        if (r < 0)
                                return bus_log_create_error(r);
                }

                r = sd_bus_call(bus, m, 0, &error, &reply);
                if (r < 0)
                        return log_error_errno(r, "Failed to %s unit: %s", verb, bus_error_message(&error, r));

                if (expect_carries_install_info) {
                        r = sd_bus_message_read(reply, "b", &carries_install_info);
                        if (r < 0)
                                return bus_log_parse_error(r);
                }

                r = bus_deserialize_and_dump_unit_file_changes(reply, arg_quiet, &changes, &n_changes);
                if (r < 0)
                        goto finish;

                /* Try to reload if enabled */
                if (!arg_no_reload)
                        r = daemon_reload(argc, argv, userdata);
                else
                        r = 0;
        }

        if (carries_install_info == 0 && !ignore_carries_install_info)
                log_warning("The unit files have no installation config (WantedBy, RequiredBy, Also, Alias\n"
                            "settings in the [Install] section, and DefaultInstance for template units).\n"
                            "This means they are not meant to be enabled using systemctl.\n"
                            "Possible reasons for having this kind of units are:\n"
                            "1) A unit may be statically enabled by being symlinked from another unit's\n"
                            "   .wants/ or .requires/ directory.\n"
                            "2) A unit's purpose may be to act as a helper for some other unit which has\n"
                            "   a requirement dependency on it.\n"
                            "3) A unit may be started when needed via activation (socket, path, timer,\n"
                            "   D-Bus, udev, scripted systemctl call, ...).\n"
                            "4) In case of template units, the unit is meant to be enabled with some\n"
                            "   instance name specified.");

        if (arg_now && STR_IN_SET(argv[0], "enable", "disable", "mask")) {
                sd_bus *bus;
                unsigned len, i;

                r = acquire_bus(BUS_MANAGER, &bus);
                if (r < 0)
                        goto finish;

                len = strv_length(names);
                {
                        char *new_args[len + 2];

                        new_args[0] = (char*) (streq(argv[0], "enable") ? "start" : "stop");
                        for (i = 0; i < len; i++)
                                new_args[i + 1] = basename(names[i]);
                        new_args[i + 1] = NULL;

                        r = start_unit(len + 1, new_args, userdata);
                }
        }

finish:
        unit_file_changes_free(changes, n_changes);

        return r;
}

static int add_dependency(int argc, char *argv[], void *userdata) {
        _cleanup_strv_free_ char **names = NULL;
        _cleanup_free_ char *target = NULL;
        const char *verb = argv[0];
        UnitFileChange *changes = NULL;
        unsigned n_changes = 0;
        UnitDependency dep;
        int r = 0;

        if (!argv[1])
                return 0;

        r = unit_name_mangle_with_suffix(argv[1], UNIT_NAME_NOGLOB, ".target", &target);
        if (r < 0)
                return log_error_errno(r, "Failed to mangle unit name: %m");

        r = mangle_names(strv_skip(argv, 2), &names);
        if (r < 0)
                return r;

        if (streq(verb, "add-wants"))
                dep = UNIT_WANTS;
        else if (streq(verb, "add-requires"))
                dep = UNIT_REQUIRES;
        else
                assert_not_reached("Unknown verb");

        if (install_client_side()) {
                r = unit_file_add_dependency(arg_scope, args_to_flags(), arg_root, names, target, dep, &changes, &n_changes);
                unit_file_dump_changes(r, "add dependency on", changes, n_changes, arg_quiet);

                if (r > 0)
                        r = 0;
        } else {
                _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL, *m = NULL;
                _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
                sd_bus *bus;

                r = acquire_bus(BUS_MANAGER, &bus);
                if (r < 0)
                        return r;

                polkit_agent_open_maybe();

                r = sd_bus_message_new_method_call(
                                bus,
                                &m,
                                "org.freedesktop.systemd1",
                                "/org/freedesktop/systemd1",
                                "org.freedesktop.systemd1.Manager",
                                "AddDependencyUnitFiles");
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_append_strv(m, names);
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_append(m, "ssbb", target, unit_dependency_to_string(dep), arg_runtime, arg_force);
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_call(bus, m, 0, &error, &reply);
                if (r < 0)
                        return log_error_errno(r, "Failed to add dependency: %s", bus_error_message(&error, r));

                r = bus_deserialize_and_dump_unit_file_changes(reply, arg_quiet, &changes, &n_changes);
                if (r < 0)
                        goto finish;

                if (arg_no_reload) {
                        r = 0;
                        goto finish;
                }

                r = daemon_reload(argc, argv, userdata);
        }

finish:
        unit_file_changes_free(changes, n_changes);

        return r;
}

static int preset_all(int argc, char *argv[], void *userdata) {
        UnitFileChange *changes = NULL;
        unsigned n_changes = 0;
        int r;

        if (install_client_side()) {
                r = unit_file_preset_all(arg_scope, args_to_flags(), arg_root, arg_preset_mode, &changes, &n_changes);
                unit_file_dump_changes(r, "preset", changes, n_changes, arg_quiet);

                if (r > 0)
                        r = 0;
        } else {
                _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
                _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
                sd_bus *bus;

                r = acquire_bus(BUS_MANAGER, &bus);
                if (r < 0)
                        return r;

                polkit_agent_open_maybe();

                r = sd_bus_call_method(
                                bus,
                                "org.freedesktop.systemd1",
                                "/org/freedesktop/systemd1",
                                "org.freedesktop.systemd1.Manager",
                                "PresetAllUnitFiles",
                                &error,
                                &reply,
                                "sbb",
                                unit_file_preset_mode_to_string(arg_preset_mode),
                                arg_runtime,
                                arg_force);
                if (r < 0)
                        return log_error_errno(r, "Failed to preset all units: %s", bus_error_message(&error, r));

                r = bus_deserialize_and_dump_unit_file_changes(reply, arg_quiet, &changes, &n_changes);
                if (r < 0)
                        goto finish;

                if (arg_no_reload) {
                        r = 0;
                        goto finish;
                }

                r = daemon_reload(argc, argv, userdata);
        }

finish:
        unit_file_changes_free(changes, n_changes);

        return r;
}

static int show_installation_targets_client_side(const char *name) {
        UnitFileChange *changes = NULL;
        unsigned n_changes = 0, i;
        UnitFileFlags flags;
        char **p;
        int r;

        p = STRV_MAKE(name);
        flags = UNIT_FILE_DRY_RUN |
                (arg_runtime ? UNIT_FILE_RUNTIME : 0);

        r = unit_file_disable(UNIT_FILE_SYSTEM, flags, NULL, p, &changes, &n_changes);
        if (r < 0)
                return log_error_errno(r, "Failed to get file links for %s: %m", name);

        for (i = 0; i < n_changes; i++)
                if (changes[i].type == UNIT_FILE_UNLINK)
                        printf("  %s\n", changes[i].path);

        return 0;
}

static int show_installation_targets(sd_bus *bus, const char *name) {
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        const char *link;
        int r;

        r = sd_bus_call_method(
                        bus,
                        "org.freedesktop.systemd1",
                        "/org/freedesktop/systemd1",
                        "org.freedesktop.systemd1.Manager",
                        "GetUnitFileLinks",
                        &error,
                        &reply,
                        "sb", name, arg_runtime);
        if (r < 0)
                return log_error_errno(r, "Failed to get unit file links for %s: %s", name, bus_error_message(&error, r));

        r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "s");
        if (r < 0)
                return bus_log_parse_error(r);

        while ((r = sd_bus_message_read(reply, "s", &link)) > 0)
                printf("  %s\n", link);

        if (r < 0)
                return bus_log_parse_error(r);

        r = sd_bus_message_exit_container(reply);
        if (r < 0)
                return bus_log_parse_error(r);

        return 0;
}

static int unit_is_enabled(int argc, char *argv[], void *userdata) {

        _cleanup_strv_free_ char **names = NULL;
        bool enabled;
        char **name;
        int r;

        r = mangle_names(strv_skip(argv, 1), &names);
        if (r < 0)
                return r;

        r = enable_sysv_units(argv[0], names);
        if (r < 0)
                return r;

        enabled = r > 0;

        if (install_client_side()) {
                STRV_FOREACH(name, names) {
                        UnitFileState state;

                        r = unit_file_get_state(arg_scope, arg_root, *name, &state);
                        if (r < 0)
                                return log_error_errno(r, "Failed to get unit file state for %s: %m", *name);

                        if (IN_SET(state,
                                   UNIT_FILE_ENABLED,
                                   UNIT_FILE_ENABLED_RUNTIME,
                                   UNIT_FILE_STATIC,
                                   UNIT_FILE_INDIRECT,
                                   UNIT_FILE_GENERATED))
                                enabled = true;

                        if (!arg_quiet) {
                                puts(unit_file_state_to_string(state));
                                if (arg_full) {
                                        r = show_installation_targets_client_side(*name);
                                        if (r < 0)
                                                return r;
                                }
                        }
                }

                r = 0;
        } else {
                _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
                sd_bus *bus;

                r = acquire_bus(BUS_MANAGER, &bus);
                if (r < 0)
                        return r;

                STRV_FOREACH(name, names) {
                        _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
                        const char *s;

                        r = sd_bus_call_method(
                                        bus,
                                        "org.freedesktop.systemd1",
                                        "/org/freedesktop/systemd1",
                                        "org.freedesktop.systemd1.Manager",
                                        "GetUnitFileState",
                                        &error,
                                        &reply,
                                        "s", *name);
                        if (r < 0)
                                return log_error_errno(r, "Failed to get unit file state for %s: %s", *name, bus_error_message(&error, r));

                        r = sd_bus_message_read(reply, "s", &s);
                        if (r < 0)
                                return bus_log_parse_error(r);

                        if (STR_IN_SET(s, "enabled", "enabled-runtime", "static", "indirect", "generated"))
                                enabled = true;

                        if (!arg_quiet) {
                                puts(s);
                                if (arg_full) {
                                        r = show_installation_targets(bus, *name);
                                        if (r < 0)
                                                return r;
                                }
                        }
                }
        }

        return enabled ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int is_system_running(int argc, char *argv[], void *userdata) {
        _cleanup_free_ char *state = NULL;
        sd_bus *bus;
        int r;

        if (running_in_chroot() > 0 || (arg_transport == BUS_TRANSPORT_LOCAL && !sd_booted())) {
                if (!arg_quiet)
                        puts("offline");
                return EXIT_FAILURE;
        }

        r = acquire_bus(BUS_MANAGER, &bus);
        if (r < 0)
                return r;

        r = sd_bus_get_property_string(
                        bus,
                        "org.freedesktop.systemd1",
                        "/org/freedesktop/systemd1",
                        "org.freedesktop.systemd1.Manager",
                        "SystemState",
                        NULL,
                        &state);
        if (r < 0) {
                if (!arg_quiet)
                        puts("unknown");
                return 0;
        }

        if (!arg_quiet)
                puts(state);

        return streq(state, "running") ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int create_edit_temp_file(const char *new_path, const char *original_path, char **ret_tmp_fn) {
        _cleanup_free_ char *t = NULL;
        int r;

        assert(new_path);
        assert(original_path);
        assert(ret_tmp_fn);

        r = tempfn_random(new_path, NULL, &t);
        if (r < 0)
                return log_error_errno(r, "Failed to determine temporary filename for \"%s\": %m", new_path);

        r = mkdir_parents(new_path, 0755);
        if (r < 0)
                return log_error_errno(r, "Failed to create directories for \"%s\": %m", new_path);

        r = copy_file(original_path, t, 0, 0644, 0, COPY_REFLINK);
        if (r == -ENOENT) {

                r = touch(t);
                if (r < 0)
                        return log_error_errno(r, "Failed to create temporary file \"%s\": %m", t);

        } else if (r < 0)
                return log_error_errno(r, "Failed to create temporary file for \"%s\": %m", new_path);

        *ret_tmp_fn = t;
        t = NULL;

        return 0;
}

static int get_file_to_edit(
                const LookupPaths *paths,
                const char *name,
                char **ret_path) {

        _cleanup_free_ char *path = NULL, *run = NULL;

        assert(name);
        assert(ret_path);

        path = strjoin(paths->persistent_config, "/", name);
        if (!path)
                return log_oom();

        if (arg_runtime) {
                run = strjoin(paths->runtime_config, "/", name);
                if (!run)
                        return log_oom();
        }

        if (arg_runtime) {
                if (access(path, F_OK) >= 0) {
                        log_error("Refusing to create \"%s\" because it would be overridden by \"%s\" anyway.", run, path);
                        return -EEXIST;
                }

                *ret_path = run;
                run = NULL;
        } else {
                *ret_path = path;
                path = NULL;
        }

        return 0;
}

static int unit_file_create_new(
                const LookupPaths *paths,
                const char *unit_name,
                const char *suffix,
                char **ret_new_path,
                char **ret_tmp_path) {

        char *tmp_new_path, *tmp_tmp_path, *ending;
        int r;

        assert(unit_name);
        assert(ret_new_path);
        assert(ret_tmp_path);

        ending = strjoina(unit_name, suffix);
        r = get_file_to_edit(paths, ending, &tmp_new_path);
        if (r < 0)
                return r;

        r = create_edit_temp_file(tmp_new_path, tmp_new_path, &tmp_tmp_path);
        if (r < 0) {
                free(tmp_new_path);
                return r;
        }

        *ret_new_path = tmp_new_path;
        *ret_tmp_path = tmp_tmp_path;

        return 0;
}

static int unit_file_create_copy(
                const LookupPaths *paths,
                const char *unit_name,
                const char *fragment_path,
                char **ret_new_path,
                char **ret_tmp_path) {

        char *tmp_new_path, *tmp_tmp_path;
        int r;

        assert(fragment_path);
        assert(unit_name);
        assert(ret_new_path);
        assert(ret_tmp_path);

        r = get_file_to_edit(paths, unit_name, &tmp_new_path);
        if (r < 0)
                return r;

        if (!path_equal(fragment_path, tmp_new_path) && access(tmp_new_path, F_OK) == 0) {
                char response;

                r = ask_char(&response, "yn", "\"%s\" already exists. Overwrite with \"%s\"? [(y)es, (n)o] ", tmp_new_path, fragment_path);
                if (r < 0) {
                        free(tmp_new_path);
                        return r;
                }
                if (response != 'y') {
                        log_warning("%s ignored", unit_name);
                        free(tmp_new_path);
                        return -EKEYREJECTED;
                }
        }

        r = create_edit_temp_file(tmp_new_path, fragment_path, &tmp_tmp_path);
        if (r < 0) {
                free(tmp_new_path);
                return r;
        }

        *ret_new_path = tmp_new_path;
        *ret_tmp_path = tmp_tmp_path;

        return 0;
}

static int run_editor(char **paths) {
        int r;

        assert(paths);

        r = safe_fork("(editor)", FORK_RESET_SIGNALS|FORK_DEATHSIG|FORK_LOG|FORK_WAIT, NULL);
        if (r < 0)
                return r;
        if (r == 0) {
                const char **args;
                char *editor, **editor_args = NULL;
                char **tmp_path, **original_path, *p;
                unsigned n_editor_args = 0, i = 1;
                size_t argc;

                argc = strv_length(paths)/2 + 1;

                /* SYSTEMD_EDITOR takes precedence over EDITOR which takes precedence over VISUAL
                 * If neither SYSTEMD_EDITOR nor EDITOR nor VISUAL are present,
                 * we try to execute well known editors
                 */
                editor = getenv("SYSTEMD_EDITOR");
                if (!editor)
                        editor = getenv("EDITOR");
                if (!editor)
                        editor = getenv("VISUAL");

                if (!isempty(editor)) {
                        editor_args = strv_split(editor, WHITESPACE);
                        if (!editor_args) {
                                (void) log_oom();
                                _exit(EXIT_FAILURE);
                        }
                        n_editor_args = strv_length(editor_args);
                        argc += n_editor_args - 1;
                }
                args = newa(const char*, argc + 1);

                if (n_editor_args > 0) {
                        args[0] = editor_args[0];
                        for (; i < n_editor_args; i++)
                                args[i] = editor_args[i];
                }

                STRV_FOREACH_PAIR(original_path, tmp_path, paths) {
                        args[i] = *tmp_path;
                        i++;
                }
                args[i] = NULL;

                if (n_editor_args > 0)
                        execvp(args[0], (char* const*) args);

                FOREACH_STRING(p, "editor", "nano", "vim", "vi") {
                        args[0] = p;
                        execvp(p, (char* const*) args);
                        /* We do not fail if the editor doesn't exist
                         * because we want to try each one of them before
                         * failing.
                         */
                        if (errno != ENOENT) {
                                log_error_errno(errno, "Failed to execute %s: %m", editor);
                                _exit(EXIT_FAILURE);
                        }
                }

                log_error("Cannot edit unit(s), no editor available. Please set either $SYSTEMD_EDITOR, $EDITOR or $VISUAL.");
                _exit(EXIT_FAILURE);
        }

        return 0;
}

static int find_paths_to_edit(sd_bus *bus, char **names, char ***paths) {
        _cleanup_lookup_paths_free_ LookupPaths lp = {};
        char **name;
        int r;

        assert(names);
        assert(paths);

        r = lookup_paths_init(&lp, arg_scope, 0, arg_root);
        if (r < 0)
                return r;

        STRV_FOREACH(name, names) {
                _cleanup_free_ char *path = NULL, *new_path = NULL, *tmp_path = NULL, *tmp_name = NULL;
                const char *unit_name;

                r = unit_find_paths(bus, *name, &lp, &path, NULL);
                if (r < 0)
                        return r;

                if (r == 0) {
                        assert(!path);

                        if (!arg_force) {
                                log_error("Run 'systemctl edit%s --force %s' to create a new unit.",
                                          arg_scope == UNIT_FILE_GLOBAL ? " --global" :
                                          arg_scope == UNIT_FILE_USER ? " --user" : "",
                                          *name);
                                return -ENOENT;
                        }

                        /* Create a new unit from scratch */
                        unit_name = *name;
                        r = unit_file_create_new(&lp, unit_name,
                                                 arg_full ? NULL : ".d/override.conf",
                                                 &new_path, &tmp_path);
                } else {
                        assert(path);

                        unit_name = basename(path);
                        /* We follow unit aliases, but we need to propagate the instance */
                        if (unit_name_is_valid(*name, UNIT_NAME_INSTANCE) &&
                            unit_name_is_valid(unit_name, UNIT_NAME_TEMPLATE)) {
                                _cleanup_free_ char *instance = NULL;

                                r = unit_name_to_instance(*name, &instance);
                                if (r < 0)
                                        return r;

                                r = unit_name_replace_instance(unit_name, instance, &tmp_name);
                                if (r < 0)
                                        return r;

                                unit_name = tmp_name;
                        }

                        if (arg_full)
                                r = unit_file_create_copy(&lp, unit_name, path, &new_path, &tmp_path);
                        else
                                r = unit_file_create_new(&lp, unit_name, ".d/override.conf", &new_path, &tmp_path);
                }
                if (r < 0)
                        return r;

                r = strv_push_pair(paths, new_path, tmp_path);
                if (r < 0)
                        return log_oom();
                new_path = tmp_path = NULL;
        }

        return 0;
}

static int edit(int argc, char *argv[], void *userdata) {
        _cleanup_strv_free_ char **names = NULL;
        _cleanup_strv_free_ char **paths = NULL;
        char **original, **tmp;
        sd_bus *bus;
        int r;

        if (!on_tty()) {
                log_error("Cannot edit units if not on a tty.");
                return -EINVAL;
        }

        if (arg_transport != BUS_TRANSPORT_LOCAL) {
                log_error("Cannot edit units remotely.");
                return -EINVAL;
        }

        r = acquire_bus(BUS_MANAGER, &bus);
        if (r < 0)
                return r;

        r = expand_names(bus, strv_skip(argv, 1), NULL, &names);
        if (r < 0)
                return log_error_errno(r, "Failed to expand names: %m");

        r = find_paths_to_edit(bus, names, &paths);
        if (r < 0)
                return r;

        if (strv_isempty(paths))
                return -ENOENT;

        r = run_editor(paths);
        if (r < 0)
                goto end;

        STRV_FOREACH_PAIR(original, tmp, paths) {
                /* If the temporary file is empty we ignore it.  It's
                 * useful if the user wants to cancel its modification
                 */
                if (null_or_empty_path(*tmp)) {
                        log_warning("Editing \"%s\" canceled: temporary file is empty.", *original);
                        continue;
                }

                r = rename(*tmp, *original);
                if (r < 0) {
                        r = log_error_errno(errno, "Failed to rename \"%s\" to \"%s\": %m", *tmp, *original);
                        goto end;
                }
        }

        r = 0;

        if (!arg_no_reload && !install_client_side())
                r = daemon_reload(argc, argv, userdata);

end:
        STRV_FOREACH_PAIR(original, tmp, paths) {
                (void) unlink(*tmp);

                /* Removing empty dropin dirs */
                if (!arg_full) {
                        _cleanup_free_ char *dir;

                        dir = dirname_malloc(*original);
                        if (!dir)
                                return log_oom();

                        /* no need to check if the dir is empty, rmdir
                         * does nothing if it is not the case.
                         */
                        (void) rmdir(dir);
                }
        }

        return r;
}

static void systemctl_help(void) {

        pager_open(arg_no_pager, false);

        printf("%s [OPTIONS...] {COMMAND} ...\n\n"
               "Query or send control commands to the systemd manager.\n\n"
               "  -h --help           Show this help\n"
               "     --version        Show package version\n"
               "     --system         Connect to system manager\n"
               "     --user           Connect to user service manager\n"
               "  -H --host=[USER@]HOST\n"
               "                      Operate on remote host\n"
               "  -M --machine=CONTAINER\n"
               "                      Operate on local container\n"
               "  -t --type=TYPE      List units of a particular type\n"
               "     --state=STATE    List units with particular LOAD or SUB or ACTIVE state\n"
               "  -p --property=NAME  Show only properties by this name\n"
               "  -a --all            Show all properties/all units currently in memory,\n"
               "                      including dead/empty ones. To list all units installed on\n"
               "                      the system, use the 'list-unit-files' command instead.\n"
               "     --failed         Same as --state=failed\n"
               "  -l --full           Don't ellipsize unit names on output\n"
               "  -r --recursive      Show unit list of host and local containers\n"
               "     --reverse        Show reverse dependencies with 'list-dependencies'\n"
               "     --job-mode=MODE  Specify how to deal with already queued jobs, when\n"
               "                      queueing a new job\n"
               "     --show-types     When showing sockets, explicitly show their type\n"
               "     --value          When showing properties, only print the value\n"
               "  -i --ignore-inhibitors\n"
               "                      When shutting down or sleeping, ignore inhibitors\n"
               "     --kill-who=WHO   Who to send signal to\n"
               "  -s --signal=SIGNAL  Which signal to send\n"
               "     --now            Start or stop unit in addition to enabling or disabling it\n"
               "     --dry-run        Only print what would be done\n"
               "  -q --quiet          Suppress output\n"
               "     --wait           For (re)start, wait until service stopped again\n"
               "     --no-block       Do not wait until operation finished\n"
               "     --no-wall        Don't send wall message before halt/power-off/reboot\n"
               "     --no-reload      Don't reload daemon after en-/dis-abling unit files\n"
               "     --no-legend      Do not print a legend (column headers and hints)\n"
               "     --no-pager       Do not pipe output into a pager\n"
               "     --no-ask-password\n"
               "                      Do not ask for system passwords\n"
               "     --global         Enable/disable/mask unit files globally\n"
               "     --runtime        Enable/disable/mask unit files temporarily until next\n"
               "                      reboot\n"
               "  -f --force          When enabling unit files, override existing symlinks\n"
               "                      When shutting down, execute action immediately\n"
               "     --preset-mode=   Apply only enable, only disable, or all presets\n"
               "     --root=PATH      Enable/disable/mask unit files in the specified root\n"
               "                      directory\n"
               "  -n --lines=INTEGER  Number of journal entries to show\n"
               "  -o --output=STRING  Change journal output mode (short, short-precise,\n"
               "                             short-iso, short-iso-precise, short-full,\n"
               "                             short-monotonic, short-unix,\n"
               "                             verbose, export, json, json-pretty, json-sse, cat)\n"
               "     --firmware-setup Tell the firmware to show the setup menu on next boot\n"
               "     --plain          Print unit dependencies as a list instead of a tree\n\n"
               "Unit Commands:\n"
               "  list-units [PATTERN...]             List units currently in memory\n"
               "  list-sockets [PATTERN...]           List socket units currently in memory,\n"
               "                                      ordered by address\n"
               "  list-timers [PATTERN...]            List timer units currently in memory,\n"
               "                                      ordered by next elapse\n"
               "  start UNIT...                       Start (activate) one or more units\n"
               "  stop UNIT...                        Stop (deactivate) one or more units\n"
               "  reload UNIT...                      Reload one or more units\n"
               "  restart UNIT...                     Start or restart one or more units\n"
               "  try-restart UNIT...                 Restart one or more units if active\n"
               "  reload-or-restart UNIT...           Reload one or more units if possible,\n"
               "                                      otherwise start or restart\n"
               "  try-reload-or-restart UNIT...       If active, reload one or more units,\n"
               "                                      if supported, otherwise restart\n"
               "  isolate UNIT                        Start one unit and stop all others\n"
               "  kill UNIT...                        Send signal to processes of a unit\n"
               "  is-active PATTERN...                Check whether units are active\n"
               "  is-failed PATTERN...                Check whether units are failed\n"
               "  status [PATTERN...|PID...]          Show runtime status of one or more units\n"
               "  show [PATTERN...|JOB...]            Show properties of one or more\n"
               "                                      units/jobs or the manager\n"
               "  cat PATTERN...                      Show files and drop-ins of specified units\n"
               "  set-property UNIT PROPERTY=VALUE... Sets one or more properties of a unit\n"
               "  help PATTERN...|PID...              Show manual for one or more units\n"
               "  reset-failed [PATTERN...]           Reset failed state for all, one, or more\n"
               "                                      units\n"
               "  list-dependencies [UNIT]            Recursively show units which are required\n"
               "                                      or wanted by this unit or by which this\n"
               "                                      unit is required or wanted\n\n"
               "Unit File Commands:\n"
               "  list-unit-files [PATTERN...]        List installed unit files\n"
               "  enable [UNIT...|PATH...]            Enable one or more unit files\n"
               "  disable UNIT...                     Disable one or more unit files\n"
               "  reenable UNIT...                    Reenable one or more unit files\n"
               "  preset UNIT...                      Enable/disable one or more unit files\n"
               "                                      based on preset configuration\n"
               "  preset-all                          Enable/disable all unit files based on\n"
               "                                      preset configuration\n"
               "  is-enabled UNIT...                  Check whether unit files are enabled\n"
               "  mask UNIT...                        Mask one or more units\n"
               "  unmask UNIT...                      Unmask one or more units\n"
               "  link PATH...                        Link one or more units files into\n"
               "                                      the search path\n"
               "  revert UNIT...                      Revert one or more unit files to vendor\n"
               "                                      version\n"
               "  add-wants TARGET UNIT...            Add 'Wants' dependency for the target\n"
               "                                      on specified one or more units\n"
               "  add-requires TARGET UNIT...         Add 'Requires' dependency for the target\n"
               "                                      on specified one or more units\n"
               "  edit UNIT...                        Edit one or more unit files\n"
               "  get-default                         Get the name of the default target\n"
               "  set-default TARGET                  Set the default target\n\n"
               "Machine Commands:\n"
               "  list-machines [PATTERN...]          List local containers and host\n\n"
               "Job Commands:\n"
               "  list-jobs [PATTERN...]              List jobs\n"
               "  cancel [JOB...]                     Cancel all, one, or more jobs\n\n"
               "Environment Commands:\n"
               "  show-environment                    Dump environment\n"
               "  set-environment VARIABLE=VALUE...   Set one or more environment variables\n"
               "  unset-environment VARIABLE...       Unset one or more environment variables\n"
               "  import-environment [VARIABLE...]    Import all or some environment variables\n\n"
               "Manager Lifecycle Commands:\n"
               "  daemon-reload                       Reload systemd manager configuration\n"
               "  daemon-reexec                       Reexecute systemd manager\n\n"
               "System Commands:\n"
               "  is-system-running                   Check whether system is fully running\n"
               "  default                             Enter system default mode\n"
               "  rescue                              Enter system rescue mode\n"
               "  emergency                           Enter system emergency mode\n"
               "  halt                                Shut down and halt the system\n"
               "  poweroff                            Shut down and power-off the system\n"
               "  reboot [ARG]                        Shut down and reboot the system\n"
               "  kexec                               Shut down and reboot the system with kexec\n"
               "  exit [EXIT_CODE]                    Request user instance or container exit\n"
               "  switch-root ROOT [INIT]             Change to a different root file system\n"
               "  suspend                             Suspend the system\n"
               "  hibernate                           Hibernate the system\n"
               "  hybrid-sleep                        Hibernate and suspend the system\n",
               program_invocation_short_name);
}

static void halt_help(void) {
        printf("%s [OPTIONS...]%s\n\n"
               "%s the system.\n\n"
               "     --help      Show this help\n"
               "     --halt      Halt the machine\n"
               "  -p --poweroff  Switch off the machine\n"
               "     --reboot    Reboot the machine\n"
               "  -f --force     Force immediate halt/power-off/reboot\n"
               "  -w --wtmp-only Don't halt/power-off/reboot, just write wtmp record\n"
               "  -d --no-wtmp   Don't write wtmp record\n"
               "     --no-wall   Don't send wall message before halt/power-off/reboot\n",
               program_invocation_short_name,
               arg_action == ACTION_REBOOT   ? " [ARG]" : "",
               arg_action == ACTION_REBOOT   ? "Reboot" :
               arg_action == ACTION_POWEROFF ? "Power off" :
                                               "Halt");
}

static void shutdown_help(void) {
        printf("%s [OPTIONS...] [TIME] [WALL...]\n\n"
               "Shut down the system.\n\n"
               "     --help      Show this help\n"
               "  -H --halt      Halt the machine\n"
               "  -P --poweroff  Power-off the machine\n"
               "  -r --reboot    Reboot the machine\n"
               "  -h             Equivalent to --poweroff, overridden by --halt\n"
               "  -k             Don't halt/power-off/reboot, just send warnings\n"
               "     --no-wall   Don't send wall message before halt/power-off/reboot\n"
               "  -c             Cancel a pending shutdown\n",
               program_invocation_short_name);
}

static void telinit_help(void) {
        printf("%s [OPTIONS...] {COMMAND}\n\n"
               "Send control commands to the init daemon.\n\n"
               "     --help      Show this help\n"
               "     --no-wall   Don't send wall message before halt/power-off/reboot\n\n"
               "Commands:\n"
               "  0              Power-off the machine\n"
               "  6              Reboot the machine\n"
               "  2, 3, 4, 5     Start runlevelX.target unit\n"
               "  1, s, S        Enter rescue mode\n"
               "  q, Q           Reload init daemon configuration\n"
               "  u, U           Reexecute init daemon\n",
               program_invocation_short_name);
}

static void runlevel_help(void) {
        printf("%s [OPTIONS...]\n\n"
               "Prints the previous and current runlevel of the init system.\n\n"
               "     --help      Show this help\n",
               program_invocation_short_name);
}

static void help_types(void) {
        int i;

        if (!arg_no_legend)
                puts("Available unit types:");
        for (i = 0; i < _UNIT_TYPE_MAX; i++)
                puts(unit_type_to_string(i));
}

static void help_states(void) {
        int i;

        if (!arg_no_legend)
                puts("Available unit load states:");
        for (i = 0; i < _UNIT_LOAD_STATE_MAX; i++)
                puts(unit_load_state_to_string(i));

        if (!arg_no_legend)
                puts("\nAvailable unit active states:");
        for (i = 0; i < _UNIT_ACTIVE_STATE_MAX; i++)
                puts(unit_active_state_to_string(i));

        if (!arg_no_legend)
                puts("\nAvailable automount unit substates:");
        for (i = 0; i < _AUTOMOUNT_STATE_MAX; i++)
                puts(automount_state_to_string(i));

        if (!arg_no_legend)
                puts("\nAvailable device unit substates:");
        for (i = 0; i < _DEVICE_STATE_MAX; i++)
                puts(device_state_to_string(i));

        if (!arg_no_legend)
                puts("\nAvailable mount unit substates:");
        for (i = 0; i < _MOUNT_STATE_MAX; i++)
                puts(mount_state_to_string(i));

        if (!arg_no_legend)
                puts("\nAvailable path unit substates:");
        for (i = 0; i < _PATH_STATE_MAX; i++)
                puts(path_state_to_string(i));

        if (!arg_no_legend)
                puts("\nAvailable scope unit substates:");
        for (i = 0; i < _SCOPE_STATE_MAX; i++)
                puts(scope_state_to_string(i));

        if (!arg_no_legend)
                puts("\nAvailable service unit substates:");
        for (i = 0; i < _SERVICE_STATE_MAX; i++)
                puts(service_state_to_string(i));

        if (!arg_no_legend)
                puts("\nAvailable slice unit substates:");
        for (i = 0; i < _SLICE_STATE_MAX; i++)
                puts(slice_state_to_string(i));

        if (!arg_no_legend)
                puts("\nAvailable socket unit substates:");
        for (i = 0; i < _SOCKET_STATE_MAX; i++)
                puts(socket_state_to_string(i));

        if (!arg_no_legend)
                puts("\nAvailable swap unit substates:");
        for (i = 0; i < _SWAP_STATE_MAX; i++)
                puts(swap_state_to_string(i));

        if (!arg_no_legend)
                puts("\nAvailable target unit substates:");
        for (i = 0; i < _TARGET_STATE_MAX; i++)
                puts(target_state_to_string(i));

        if (!arg_no_legend)
                puts("\nAvailable timer unit substates:");
        for (i = 0; i < _TIMER_STATE_MAX; i++)
                puts(timer_state_to_string(i));
}

static int systemctl_parse_argv(int argc, char *argv[]) {

        enum {
                ARG_FAIL = 0x100,
                ARG_REVERSE,
                ARG_AFTER,
                ARG_BEFORE,
                ARG_DRY_RUN,
                ARG_SHOW_TYPES,
                ARG_IRREVERSIBLE,
                ARG_IGNORE_DEPENDENCIES,
                ARG_VALUE,
                ARG_VERSION,
                ARG_USER,
                ARG_SYSTEM,
                ARG_GLOBAL,
                ARG_NO_BLOCK,
                ARG_NO_LEGEND,
                ARG_NO_PAGER,
                ARG_NO_WALL,
                ARG_ROOT,
                ARG_NO_RELOAD,
                ARG_KILL_WHO,
                ARG_NO_ASK_PASSWORD,
                ARG_FAILED,
                ARG_RUNTIME,
                ARG_PLAIN,
                ARG_STATE,
                ARG_JOB_MODE,
                ARG_PRESET_MODE,
                ARG_FIRMWARE_SETUP,
                ARG_NOW,
                ARG_MESSAGE,
                ARG_WAIT,
        };

        static const struct option options[] = {
                { "help",                no_argument,       NULL, 'h'                     },
                { "version",             no_argument,       NULL, ARG_VERSION             },
                { "type",                required_argument, NULL, 't'                     },
                { "property",            required_argument, NULL, 'p'                     },
                { "all",                 no_argument,       NULL, 'a'                     },
                { "reverse",             no_argument,       NULL, ARG_REVERSE             },
                { "after",               no_argument,       NULL, ARG_AFTER               },
                { "before",              no_argument,       NULL, ARG_BEFORE              },
                { "show-types",          no_argument,       NULL, ARG_SHOW_TYPES          },
                { "failed",              no_argument,       NULL, ARG_FAILED              }, /* compatibility only */
                { "full",                no_argument,       NULL, 'l'                     },
                { "job-mode",            required_argument, NULL, ARG_JOB_MODE            },
                { "fail",                no_argument,       NULL, ARG_FAIL                }, /* compatibility only */
                { "irreversible",        no_argument,       NULL, ARG_IRREVERSIBLE        }, /* compatibility only */
                { "ignore-dependencies", no_argument,       NULL, ARG_IGNORE_DEPENDENCIES }, /* compatibility only */
                { "ignore-inhibitors",   no_argument,       NULL, 'i'                     },
                { "value",               no_argument,       NULL, ARG_VALUE               },
                { "user",                no_argument,       NULL, ARG_USER                },
                { "system",              no_argument,       NULL, ARG_SYSTEM              },
                { "global",              no_argument,       NULL, ARG_GLOBAL              },
                { "wait",                no_argument,       NULL, ARG_WAIT                },
                { "no-block",            no_argument,       NULL, ARG_NO_BLOCK            },
                { "no-legend",           no_argument,       NULL, ARG_NO_LEGEND           },
                { "no-pager",            no_argument,       NULL, ARG_NO_PAGER            },
                { "no-wall",             no_argument,       NULL, ARG_NO_WALL             },
                { "dry-run",             no_argument,       NULL, ARG_DRY_RUN             },
                { "quiet",               no_argument,       NULL, 'q'                     },
                { "root",                required_argument, NULL, ARG_ROOT                },
                { "force",               no_argument,       NULL, 'f'                     },
                { "no-reload",           no_argument,       NULL, ARG_NO_RELOAD           },
                { "kill-who",            required_argument, NULL, ARG_KILL_WHO            },
                { "signal",              required_argument, NULL, 's'                     },
                { "no-ask-password",     no_argument,       NULL, ARG_NO_ASK_PASSWORD     },
                { "host",                required_argument, NULL, 'H'                     },
                { "machine",             required_argument, NULL, 'M'                     },
                { "runtime",             no_argument,       NULL, ARG_RUNTIME             },
                { "lines",               required_argument, NULL, 'n'                     },
                { "output",              required_argument, NULL, 'o'                     },
                { "plain",               no_argument,       NULL, ARG_PLAIN               },
                { "state",               required_argument, NULL, ARG_STATE               },
                { "recursive",           no_argument,       NULL, 'r'                     },
                { "preset-mode",         required_argument, NULL, ARG_PRESET_MODE         },
                { "firmware-setup",      no_argument,       NULL, ARG_FIRMWARE_SETUP      },
                { "now",                 no_argument,       NULL, ARG_NOW                 },
                { "message",             required_argument, NULL, ARG_MESSAGE             },
                {}
        };

        const char *p;
        int c, r;

        assert(argc >= 0);
        assert(argv);

        /* we default to allowing interactive authorization only in systemctl (not in the legacy commands) */
        arg_ask_password = true;

        while ((c = getopt_long(argc, argv, "ht:p:alqfs:H:M:n:o:ir", options, NULL)) >= 0)

                switch (c) {

                case 'h':
                        systemctl_help();
                        return 0;

                case ARG_VERSION:
                        return version();

                case 't': {
                        if (isempty(optarg)) {
                                log_error("--type= requires arguments.");
                                return -EINVAL;
                        }

                        for (p = optarg;;) {
                                _cleanup_free_ char *type = NULL;

                                r = extract_first_word(&p, &type, ",", 0);
                                if (r < 0)
                                        return log_error_errno(r, "Failed to parse type: %s", optarg);
                                if (r == 0)
                                        break;

                                if (streq(type, "help")) {
                                        help_types();
                                        return 0;
                                }

                                if (unit_type_from_string(type) >= 0) {
                                        if (strv_push(&arg_types, type) < 0)
                                                return log_oom();
                                        type = NULL;
                                        continue;
                                }

                                /* It's much nicer to use --state= for
                                 * load states, but let's support this
                                 * in --types= too for compatibility
                                 * with old versions */
                                if (unit_load_state_from_string(type) >= 0) {
                                        if (strv_push(&arg_states, type) < 0)
                                                return log_oom();
                                        type = NULL;
                                        continue;
                                }

                                log_error("Unknown unit type or load state '%s'.", type);
                                log_info("Use -t help to see a list of allowed values.");
                                return -EINVAL;
                        }

                        break;
                }

                case 'p': {
                        /* Make sure that if the empty property list
                           was specified, we won't show any properties. */
                        if (isempty(optarg) && !arg_properties) {
                                arg_properties = new0(char*, 1);
                                if (!arg_properties)
                                        return log_oom();
                        } else
                                for (p = optarg;;) {
                                        _cleanup_free_ char *prop = NULL;

                                        r = extract_first_word(&p, &prop, ",", 0);
                                        if (r < 0)
                                                return log_error_errno(r, "Failed to parse property: %s", optarg);
                                        if (r == 0)
                                                break;

                                        if (strv_push(&arg_properties, prop) < 0)
                                                return log_oom();

                                        prop = NULL;
                                }

                        /* If the user asked for a particular
                         * property, show it to him, even if it is
                         * empty. */
                        arg_all = true;

                        break;
                }

                case 'a':
                        arg_all = true;
                        break;

                case ARG_REVERSE:
                        arg_dependency = DEPENDENCY_REVERSE;
                        break;

                case ARG_AFTER:
                        arg_dependency = DEPENDENCY_AFTER;
                        arg_jobs_after = true;
                        break;

                case ARG_BEFORE:
                        arg_dependency = DEPENDENCY_BEFORE;
                        arg_jobs_before = true;
                        break;

                case ARG_SHOW_TYPES:
                        arg_show_types = true;
                        break;

                case ARG_VALUE:
                        arg_value = true;
                        break;

                case ARG_JOB_MODE:
                        arg_job_mode = optarg;
                        break;

                case ARG_FAIL:
                        arg_job_mode = "fail";
                        break;

                case ARG_IRREVERSIBLE:
                        arg_job_mode = "replace-irreversibly";
                        break;

                case ARG_IGNORE_DEPENDENCIES:
                        arg_job_mode = "ignore-dependencies";
                        break;

                case ARG_USER:
                        arg_scope = UNIT_FILE_USER;
                        break;

                case ARG_SYSTEM:
                        arg_scope = UNIT_FILE_SYSTEM;
                        break;

                case ARG_GLOBAL:
                        arg_scope = UNIT_FILE_GLOBAL;
                        break;

                case ARG_WAIT:
                        arg_wait = true;
                        break;

                case ARG_NO_BLOCK:
                        arg_no_block = true;
                        break;

                case ARG_NO_LEGEND:
                        arg_no_legend = true;
                        break;

                case ARG_NO_PAGER:
                        arg_no_pager = true;
                        break;

                case ARG_NO_WALL:
                        arg_no_wall = true;
                        break;

                case ARG_ROOT:
                        r = parse_path_argument_and_warn(optarg, false, &arg_root);
                        if (r < 0)
                                return r;
                        break;

                case 'l':
                        arg_full = true;
                        break;

                case ARG_FAILED:
                        if (strv_extend(&arg_states, "failed") < 0)
                                return log_oom();

                        break;

                case ARG_DRY_RUN:
                        arg_dry_run = true;
                        break;

                case 'q':
                        arg_quiet = true;
                        break;

                case 'f':
                        arg_force++;
                        break;

                case ARG_NO_RELOAD:
                        arg_no_reload = true;
                        break;

                case ARG_KILL_WHO:
                        arg_kill_who = optarg;
                        break;

                case 's':
                        arg_signal = signal_from_string_try_harder(optarg);
                        if (arg_signal < 0) {
                                log_error("Failed to parse signal string %s.", optarg);
                                return -EINVAL;
                        }
                        break;

                case ARG_NO_ASK_PASSWORD:
                        arg_ask_password = false;
                        break;

                case 'H':
                        arg_transport = BUS_TRANSPORT_REMOTE;
                        arg_host = optarg;
                        break;

                case 'M':
                        arg_transport = BUS_TRANSPORT_MACHINE;
                        arg_host = optarg;
                        break;

                case ARG_RUNTIME:
                        arg_runtime = true;
                        break;

                case 'n':
                        if (safe_atou(optarg, &arg_lines) < 0) {
                                log_error("Failed to parse lines '%s'", optarg);
                                return -EINVAL;
                        }
                        break;

                case 'o':
                        arg_output = output_mode_from_string(optarg);
                        if (arg_output < 0) {
                                log_error("Unknown output '%s'.", optarg);
                                return -EINVAL;
                        }
                        break;

                case 'i':
                        arg_ignore_inhibitors = true;
                        break;

                case ARG_PLAIN:
                        arg_plain = true;
                        break;

                case ARG_FIRMWARE_SETUP:
                        arg_firmware_setup = true;
                        break;

                case ARG_STATE: {
                        if (isempty(optarg)) {
                                log_error("--state= requires arguments.");
                                return -EINVAL;
                        }

                        for (p = optarg;;) {
                                _cleanup_free_ char *s = NULL;

                                r = extract_first_word(&p, &s, ",", 0);
                                if (r < 0)
                                        return log_error_errno(r, "Failed to parse state: %s", optarg);
                                if (r == 0)
                                        break;

                                if (streq(s, "help")) {
                                        help_states();
                                        return 0;
                                }

                                if (strv_push(&arg_states, s) < 0)
                                        return log_oom();

                                s = NULL;
                        }
                        break;
                }

                case 'r':
                        if (geteuid() != 0) {
                                log_error("--recursive requires root privileges.");
                                return -EPERM;
                        }

                        arg_recursive = true;
                        break;

                case ARG_PRESET_MODE:

                        arg_preset_mode = unit_file_preset_mode_from_string(optarg);
                        if (arg_preset_mode < 0) {
                                log_error("Failed to parse preset mode: %s.", optarg);
                                return -EINVAL;
                        }

                        break;

                case ARG_NOW:
                        arg_now = true;
                        break;

                case ARG_MESSAGE:
                        if (strv_extend(&arg_wall, optarg) < 0)
                                return log_oom();
                        break;

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached("Unhandled option");
                }

        if (arg_transport != BUS_TRANSPORT_LOCAL && arg_scope != UNIT_FILE_SYSTEM) {
                log_error("Cannot access user instance remotely.");
                return -EINVAL;
        }

        if (arg_wait && arg_no_block) {
                log_error("--wait may not be combined with --no-block.");
                return -EINVAL;
        }

        return 1;
}

static int halt_parse_argv(int argc, char *argv[]) {

        enum {
                ARG_HELP = 0x100,
                ARG_HALT,
                ARG_REBOOT,
                ARG_NO_WALL
        };

        static const struct option options[] = {
                { "help",      no_argument,       NULL, ARG_HELP    },
                { "halt",      no_argument,       NULL, ARG_HALT    },
                { "poweroff",  no_argument,       NULL, 'p'         },
                { "reboot",    no_argument,       NULL, ARG_REBOOT  },
                { "force",     no_argument,       NULL, 'f'         },
                { "wtmp-only", no_argument,       NULL, 'w'         },
                { "no-wtmp",   no_argument,       NULL, 'd'         },
                { "no-sync",   no_argument,       NULL, 'n'         },
                { "no-wall",   no_argument,       NULL, ARG_NO_WALL },
                {}
        };

        int c, r, runlevel;

        assert(argc >= 0);
        assert(argv);

        if (utmp_get_runlevel(&runlevel, NULL) >= 0)
                if (IN_SET(runlevel, '0', '6'))
                        arg_force = 2;

        while ((c = getopt_long(argc, argv, "pfwdnih", options, NULL)) >= 0)
                switch (c) {

                case ARG_HELP:
                        halt_help();
                        return 0;

                case ARG_HALT:
                        arg_action = ACTION_HALT;
                        break;

                case 'p':
                        if (arg_action != ACTION_REBOOT)
                                arg_action = ACTION_POWEROFF;
                        break;

                case ARG_REBOOT:
                        arg_action = ACTION_REBOOT;
                        break;

                case 'f':
                        arg_force = 2;
                        break;

                case 'w':
                        arg_dry_run = true;
                        break;

                case 'd':
                        arg_no_wtmp = true;
                        break;

                case 'n':
                        arg_no_sync = true;
                        break;

                case ARG_NO_WALL:
                        arg_no_wall = true;
                        break;

                case 'i':
                case 'h':
                        /* Compatibility nops */
                        break;

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached("Unhandled option");
                }

        if (arg_action == ACTION_REBOOT && (argc == optind || argc == optind + 1)) {
                r = update_reboot_parameter_and_warn(argc == optind + 1 ? argv[optind] : NULL);
                if (r < 0)
                        return r;
        } else if (optind < argc) {
                log_error("Too many arguments.");
                return -EINVAL;
        }

        return 1;
}

static int parse_shutdown_time_spec(const char *t, usec_t *_u) {
        assert(t);
        assert(_u);

        if (streq(t, "now"))
                *_u = 0;
        else if (!strchr(t, ':')) {
                uint64_t u;

                if (safe_atou64(t, &u) < 0)
                        return -EINVAL;

                *_u = now(CLOCK_REALTIME) + USEC_PER_MINUTE * u;
        } else {
                char *e = NULL;
                long hour, minute;
                struct tm tm = {};
                time_t s;
                usec_t n;

                errno = 0;
                hour = strtol(t, &e, 10);
                if (errno > 0 || *e != ':' || hour < 0 || hour > 23)
                        return -EINVAL;

                minute = strtol(e+1, &e, 10);
                if (errno > 0 || *e != 0 || minute < 0 || minute > 59)
                        return -EINVAL;

                n = now(CLOCK_REALTIME);
                s = (time_t) (n / USEC_PER_SEC);

                assert_se(localtime_r(&s, &tm));

                tm.tm_hour = (int) hour;
                tm.tm_min = (int) minute;
                tm.tm_sec = 0;

                assert_se(s = mktime(&tm));

                *_u = (usec_t) s * USEC_PER_SEC;

                while (*_u <= n)
                        *_u += USEC_PER_DAY;
        }

        return 0;
}

static int shutdown_parse_argv(int argc, char *argv[]) {

        enum {
                ARG_HELP = 0x100,
                ARG_NO_WALL
        };

        static const struct option options[] = {
                { "help",      no_argument,       NULL, ARG_HELP    },
                { "halt",      no_argument,       NULL, 'H'         },
                { "poweroff",  no_argument,       NULL, 'P'         },
                { "reboot",    no_argument,       NULL, 'r'         },
                { "kexec",     no_argument,       NULL, 'K'         }, /* not documented extension */
                { "no-wall",   no_argument,       NULL, ARG_NO_WALL },
                {}
        };

        char **wall = NULL;
        int c, r;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "HPrhkKat:fFc", options, NULL)) >= 0)
                switch (c) {

                case ARG_HELP:
                        shutdown_help();
                        return 0;

                case 'H':
                        arg_action = ACTION_HALT;
                        break;

                case 'P':
                        arg_action = ACTION_POWEROFF;
                        break;

                case 'r':
                        if (kexec_loaded())
                                arg_action = ACTION_KEXEC;
                        else
                                arg_action = ACTION_REBOOT;
                        break;

                case 'K':
                        arg_action = ACTION_KEXEC;
                        break;

                case 'h':
                        if (arg_action != ACTION_HALT)
                                arg_action = ACTION_POWEROFF;
                        break;

                case 'k':
                        arg_dry_run = true;
                        break;

                case ARG_NO_WALL:
                        arg_no_wall = true;
                        break;

                case 'a':
                case 't': /* Note that we also ignore any passed argument to -t, not just the -t itself */
                case 'f':
                case 'F':
                        /* Compatibility nops */
                        break;

                case 'c':
                        arg_action = ACTION_CANCEL_SHUTDOWN;
                        break;

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached("Unhandled option");
                }

        if (argc > optind && arg_action != ACTION_CANCEL_SHUTDOWN) {
                r = parse_shutdown_time_spec(argv[optind], &arg_when);
                if (r < 0) {
                        log_error("Failed to parse time specification: %s", argv[optind]);
                        return r;
                }
        } else
                arg_when = now(CLOCK_REALTIME) + USEC_PER_MINUTE;

        if (argc > optind && arg_action == ACTION_CANCEL_SHUTDOWN)
                /* No time argument for shutdown cancel */
                wall = argv + optind;
        else if (argc > optind + 1)
                /* We skip the time argument */
                wall = argv + optind + 1;

        if (wall) {
                arg_wall = strv_copy(wall);
                if (!arg_wall)
                        return log_oom();
        }

        optind = argc;

        return 1;
}

static int telinit_parse_argv(int argc, char *argv[]) {

        enum {
                ARG_HELP = 0x100,
                ARG_NO_WALL
        };

        static const struct option options[] = {
                { "help",      no_argument,       NULL, ARG_HELP    },
                { "no-wall",   no_argument,       NULL, ARG_NO_WALL },
                {}
        };

        static const struct {
                char from;
                enum action to;
        } table[] = {
                { '0', ACTION_POWEROFF },
                { '6', ACTION_REBOOT },
                { '1', ACTION_RESCUE },
                { '2', ACTION_RUNLEVEL2 },
                { '3', ACTION_RUNLEVEL3 },
                { '4', ACTION_RUNLEVEL4 },
                { '5', ACTION_RUNLEVEL5 },
                { 's', ACTION_RESCUE },
                { 'S', ACTION_RESCUE },
                { 'q', ACTION_RELOAD },
                { 'Q', ACTION_RELOAD },
                { 'u', ACTION_REEXEC },
                { 'U', ACTION_REEXEC }
        };

        unsigned i;
        int c;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "", options, NULL)) >= 0)
                switch (c) {

                case ARG_HELP:
                        telinit_help();
                        return 0;

                case ARG_NO_WALL:
                        arg_no_wall = true;
                        break;

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached("Unhandled option");
                }

        if (optind >= argc) {
                log_error("%s: required argument missing.", program_invocation_short_name);
                return -EINVAL;
        }

        if (optind + 1 < argc) {
                log_error("Too many arguments.");
                return -EINVAL;
        }

        if (strlen(argv[optind]) != 1) {
                log_error("Expected single character argument.");
                return -EINVAL;
        }

        for (i = 0; i < ELEMENTSOF(table); i++)
                if (table[i].from == argv[optind][0])
                        break;

        if (i >= ELEMENTSOF(table)) {
                log_error("Unknown command '%s'.", argv[optind]);
                return -EINVAL;
        }

        arg_action = table[i].to;

        optind++;

        return 1;
}

static int runlevel_parse_argv(int argc, char *argv[]) {

        enum {
                ARG_HELP = 0x100,
        };

        static const struct option options[] = {
                { "help",      no_argument,       NULL, ARG_HELP    },
                {}
        };

        int c;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "", options, NULL)) >= 0)
                switch (c) {

                case ARG_HELP:
                        runlevel_help();
                        return 0;

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached("Unhandled option");
                }

        if (optind < argc) {
                log_error("Too many arguments.");
                return -EINVAL;
        }

        return 1;
}

static int parse_argv(int argc, char *argv[]) {
        assert(argc >= 0);
        assert(argv);

        if (program_invocation_short_name) {

                if (strstr(program_invocation_short_name, "halt")) {
                        arg_action = ACTION_HALT;
                        return halt_parse_argv(argc, argv);

                } else if (strstr(program_invocation_short_name, "poweroff")) {
                        arg_action = ACTION_POWEROFF;
                        return halt_parse_argv(argc, argv);

                } else if (strstr(program_invocation_short_name, "reboot")) {
                        if (kexec_loaded())
                                arg_action = ACTION_KEXEC;
                        else
                                arg_action = ACTION_REBOOT;
                        return halt_parse_argv(argc, argv);

                } else if (strstr(program_invocation_short_name, "shutdown")) {
                        arg_action = ACTION_POWEROFF;
                        return shutdown_parse_argv(argc, argv);

                } else if (strstr(program_invocation_short_name, "init")) {

                        /* Matches invocations as "init" as well as "telinit", which are synonymous when run as PID !=
                         * 1 on SysV.
                         *
                         * On SysV "telinit" was the official command to communicate with PID 1, but "init" would
                         * redirect itself to "telinit" if called with PID != 1. We follow the same logic here still,
                         * though we add one level of indirection, as we implement "telinit" in "systemctl". Hence, for
                         * us if you invoke "init" you get "systemd", but it will execve() "systemctl" immediately with
                         * argv[] unmodified if PID is != 1. If you invoke "telinit" you directly get "systemctl". In
                         * both cases we shall do the same thing, which is why we do strstr(p_i_s_n, "init") here, as a
                         * quick way to match both.
                         *
                         * Also see redirect_telinit() in src/core/main.c. */

                        if (sd_booted() > 0) {
                                arg_action = _ACTION_INVALID;
                                return telinit_parse_argv(argc, argv);
                        } else {
                                /* Hmm, so some other init system is running, we need to forward this request to
                                 * it. For now we simply guess that it is Upstart. */

                                execv(TELINIT, argv);

                                log_error("Couldn't find an alternative telinit implementation to spawn.");
                                return -EIO;
                        }

                } else if (strstr(program_invocation_short_name, "runlevel")) {
                        arg_action = ACTION_RUNLEVEL;
                        return runlevel_parse_argv(argc, argv);
                }
        }

        arg_action = ACTION_SYSTEMCTL;
        return systemctl_parse_argv(argc, argv);
}

#if HAVE_SYSV_COMPAT
_pure_ static int action_to_runlevel(void) {

        static const char table[_ACTION_MAX] = {
                [ACTION_HALT] =      '0',
                [ACTION_POWEROFF] =  '0',
                [ACTION_REBOOT] =    '6',
                [ACTION_RUNLEVEL2] = '2',
                [ACTION_RUNLEVEL3] = '3',
                [ACTION_RUNLEVEL4] = '4',
                [ACTION_RUNLEVEL5] = '5',
                [ACTION_RESCUE] =    '1'
        };

        assert(arg_action >= 0 && arg_action < _ACTION_MAX);

        return table[arg_action];
}
#endif

static int talk_initctl(void) {
#if HAVE_SYSV_COMPAT
        struct init_request request = {
                .magic = INIT_MAGIC,
                .sleeptime  = 0,
                .cmd = INIT_CMD_RUNLVL
        };

        _cleanup_close_ int fd = -1;
        char rl;
        int r;
        const char *p;

        rl = action_to_runlevel();
        if (!rl)
                return 0;

        request.runlevel = rl;

        FOREACH_STRING(p, "/run/initctl", "/dev/initctl") {
                fd = open(p, O_WRONLY|O_NONBLOCK|O_CLOEXEC|O_NOCTTY);
                if (fd >= 0 || errno != ENOENT)
                        break;
        }
        if (fd < 0) {
                if (errno == ENOENT)
                        return 0;

                return log_error_errno(errno, "Failed to open initctl fifo: %m");
        }

        r = loop_write(fd, &request, sizeof(request), false);
        if (r < 0)
                return log_error_errno(r, "Failed to write to %s: %m", p);

        return 1;
#else
        return 0;
#endif
}

static int systemctl_main(int argc, char *argv[]) {

        static const Verb verbs[] = {
                { "list-units",            VERB_ANY, VERB_ANY, VERB_DEFAULT|VERB_ONLINE_ONLY, list_units },
                { "list-unit-files",       VERB_ANY, VERB_ANY, 0,                list_unit_files      },
                { "list-sockets",          VERB_ANY, VERB_ANY, VERB_ONLINE_ONLY, list_sockets         },
                { "list-timers",           VERB_ANY, VERB_ANY, VERB_ONLINE_ONLY, list_timers          },
                { "list-jobs",             VERB_ANY, VERB_ANY, VERB_ONLINE_ONLY, list_jobs            },
                { "list-machines",         VERB_ANY, VERB_ANY, VERB_ONLINE_ONLY|VERB_MUST_BE_ROOT, list_machines },
                { "clear-jobs",            VERB_ANY, 1,        VERB_ONLINE_ONLY, trivial_method       },
                { "cancel",                VERB_ANY, VERB_ANY, VERB_ONLINE_ONLY, cancel_job           },
                { "start",                 2,        VERB_ANY, VERB_ONLINE_ONLY, start_unit           },
                { "stop",                  2,        VERB_ANY, VERB_ONLINE_ONLY, start_unit           },
                { "condstop",              2,        VERB_ANY, VERB_ONLINE_ONLY, start_unit           }, /* For compatibility with ALTLinux */
                { "reload",                2,        VERB_ANY, VERB_ONLINE_ONLY, start_unit           },
                { "restart",               2,        VERB_ANY, VERB_ONLINE_ONLY, start_unit           },
                { "try-restart",           2,        VERB_ANY, VERB_ONLINE_ONLY, start_unit           },
                { "reload-or-restart",     2,        VERB_ANY, VERB_ONLINE_ONLY, start_unit           },
                { "reload-or-try-restart", 2,        VERB_ANY, VERB_ONLINE_ONLY, start_unit           }, /* For compatbility with old systemctl <= 228 */
                { "try-reload-or-restart", 2,        VERB_ANY, VERB_ONLINE_ONLY, start_unit           },
                { "force-reload",          2,        VERB_ANY, VERB_ONLINE_ONLY, start_unit           }, /* For compatibility with SysV */
                { "condreload",            2,        VERB_ANY, VERB_ONLINE_ONLY, start_unit           }, /* For compatibility with ALTLinux */
                { "condrestart",           2,        VERB_ANY, VERB_ONLINE_ONLY, start_unit           }, /* For compatibility with RH */
                { "isolate",               2,        2,        VERB_ONLINE_ONLY, start_unit           },
                { "kill",                  2,        VERB_ANY, VERB_ONLINE_ONLY, kill_unit            },
                { "is-active",             2,        VERB_ANY, VERB_ONLINE_ONLY, check_unit_active    },
                { "check",                 2,        VERB_ANY, VERB_ONLINE_ONLY, check_unit_active    }, /* deprecated alias of is-active */
                { "is-failed",             2,        VERB_ANY, VERB_ONLINE_ONLY, check_unit_failed    },
                { "show",                  VERB_ANY, VERB_ANY, VERB_ONLINE_ONLY, show                 },
                { "cat",                   2,        VERB_ANY, VERB_ONLINE_ONLY, cat                  },
                { "status",                VERB_ANY, VERB_ANY, VERB_ONLINE_ONLY, show                 },
                { "help",                  VERB_ANY, VERB_ANY, VERB_ONLINE_ONLY, show                 },
                { "daemon-reload",         VERB_ANY, 1,        VERB_ONLINE_ONLY, daemon_reload        },
                { "daemon-reexec",         VERB_ANY, 1,        VERB_ONLINE_ONLY, daemon_reload        },
                { "show-environment",      VERB_ANY, 1,        VERB_ONLINE_ONLY, show_environment     },
                { "set-environment",       2,        VERB_ANY, VERB_ONLINE_ONLY, set_environment      },
                { "unset-environment",     2,        VERB_ANY, VERB_ONLINE_ONLY, set_environment      },
                { "import-environment",    VERB_ANY, VERB_ANY, VERB_ONLINE_ONLY, import_environment   },
                { "halt",                  VERB_ANY, 1,        VERB_ONLINE_ONLY, start_system_special },
                { "poweroff",              VERB_ANY, 1,        VERB_ONLINE_ONLY, start_system_special },
                { "reboot",                VERB_ANY, 2,        VERB_ONLINE_ONLY, start_system_special },
                { "kexec",                 VERB_ANY, 1,        VERB_ONLINE_ONLY, start_system_special },
                { "suspend",               VERB_ANY, 1,        VERB_ONLINE_ONLY, start_system_special },
                { "hibernate",             VERB_ANY, 1,        VERB_ONLINE_ONLY, start_system_special },
                { "hybrid-sleep",          VERB_ANY, 1,        VERB_ONLINE_ONLY, start_system_special },
                { "default",               VERB_ANY, 1,        VERB_ONLINE_ONLY, start_special        },
                { "rescue",                VERB_ANY, 1,        VERB_ONLINE_ONLY, start_system_special },
                { "emergency",             VERB_ANY, 1,        VERB_ONLINE_ONLY, start_system_special },
                { "exit",                  VERB_ANY, 2,        VERB_ONLINE_ONLY, start_special        },
                { "reset-failed",          VERB_ANY, VERB_ANY, VERB_ONLINE_ONLY, reset_failed         },
                { "enable",                2,        VERB_ANY, 0,                enable_unit          },
                { "disable",               2,        VERB_ANY, 0,                enable_unit          },
                { "is-enabled",            2,        VERB_ANY, 0,                unit_is_enabled      },
                { "reenable",              2,        VERB_ANY, 0,                enable_unit          },
                { "preset",                2,        VERB_ANY, 0,                enable_unit          },
                { "preset-all",            VERB_ANY, 1,        0,                preset_all           },
                { "mask",                  2,        VERB_ANY, 0,                enable_unit          },
                { "unmask",                2,        VERB_ANY, 0,                enable_unit          },
                { "link",                  2,        VERB_ANY, 0,                enable_unit          },
                { "revert",                2,        VERB_ANY, 0,                enable_unit          },
                { "switch-root",           2,        VERB_ANY, VERB_ONLINE_ONLY, switch_root          },
                { "list-dependencies",     VERB_ANY, 2,        VERB_ONLINE_ONLY, list_dependencies    },
                { "set-default",           2,        2,        0,                set_default          },
                { "get-default",           VERB_ANY, 1,        0,                get_default          },
                { "set-property",          3,        VERB_ANY, VERB_ONLINE_ONLY, set_property         },
                { "is-system-running",     VERB_ANY, 1,        0,                is_system_running    },
                { "add-wants",             3,        VERB_ANY, 0,                add_dependency       },
                { "add-requires",          3,        VERB_ANY, 0,                add_dependency       },
                { "edit",                  2,        VERB_ANY, VERB_ONLINE_ONLY, edit                 },
                {}
        };

        return dispatch_verb(argc, argv, verbs, NULL);
}

static int reload_with_fallback(void) {

        /* First, try systemd via D-Bus. */
        if (daemon_reload(0, NULL, NULL) >= 0)
                return 0;

        /* Nothing else worked, so let's try signals */
        assert(IN_SET(arg_action, ACTION_RELOAD, ACTION_REEXEC));

        if (kill(1, arg_action == ACTION_RELOAD ? SIGHUP : SIGTERM) < 0)
                return log_error_errno(errno, "kill() failed: %m");

        return 0;
}

static int start_with_fallback(void) {

        /* First, try systemd via D-Bus. */
        if (start_unit(0, NULL, NULL) >= 0)
                return 0;

        /* Nothing else worked, so let's try /dev/initctl */
        if (talk_initctl() > 0)
                return 0;

        log_error("Failed to talk to init daemon.");
        return -EIO;
}

static int halt_now(enum action a) {

        /* The kernel will automatically flush ATA disks and suchlike on reboot(), but the file systems need to be
         * synce'd explicitly in advance. */
        if (!arg_no_sync && !arg_dry_run)
                (void) sync();

        /* Make sure C-A-D is handled by the kernel from this point on... */
        if (!arg_dry_run)
                (void) reboot(RB_ENABLE_CAD);

        switch (a) {

        case ACTION_HALT:
                if (!arg_quiet)
                        log_info("Halting.");
                if (arg_dry_run)
                        return 0;
                (void) reboot(RB_HALT_SYSTEM);
                return -errno;

        case ACTION_POWEROFF:
                if (!arg_quiet)
                        log_info("Powering off.");
                if (arg_dry_run)
                        return 0;
                (void) reboot(RB_POWER_OFF);
                return -errno;

        case ACTION_KEXEC:
        case ACTION_REBOOT:
                return reboot_with_parameter(REBOOT_FALLBACK |
                                             (arg_quiet ? 0 : REBOOT_LOG) |
                                             (arg_dry_run ? REBOOT_DRY_RUN : 0));

        default:
                assert_not_reached("Unknown action.");
        }
}

static int logind_schedule_shutdown(void) {

#if ENABLE_LOGIND
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        char date[FORMAT_TIMESTAMP_MAX];
        const char *action;
        sd_bus *bus;
        int r;

        r = acquire_bus(BUS_FULL, &bus);
        if (r < 0)
                return r;

        switch (arg_action) {
        case ACTION_HALT:
                action = "halt";
                break;
        case ACTION_POWEROFF:
                action = "poweroff";
                break;
        case ACTION_KEXEC:
                action = "kexec";
                break;
        case ACTION_EXIT:
                action = "exit";
                break;
        case ACTION_REBOOT:
        default:
                action = "reboot";
                break;
        }

        if (arg_dry_run)
                action = strjoina("dry-", action);

        (void) logind_set_wall_message();

        r = sd_bus_call_method(
                        bus,
                        "org.freedesktop.login1",
                        "/org/freedesktop/login1",
                        "org.freedesktop.login1.Manager",
                        "ScheduleShutdown",
                        &error,
                        NULL,
                        "st",
                        action,
                        arg_when);
        if (r < 0)
                return log_warning_errno(r, "Failed to call ScheduleShutdown in logind, proceeding with immediate shutdown: %s", bus_error_message(&error, r));

        if (!arg_quiet)
                log_info("Shutdown scheduled for %s, use 'shutdown -c' to cancel.", format_timestamp(date, sizeof(date), arg_when));
        return 0;
#else
        log_error("Cannot schedule shutdown without logind support, proceeding with immediate shutdown.");
        return -ENOSYS;
#endif
}

static int halt_main(void) {
        int r;

        r = logind_check_inhibitors(arg_action);
        if (r < 0)
                return r;

        if (arg_when > 0)
                return logind_schedule_shutdown();

        if (geteuid() != 0) {
                if (arg_dry_run || arg_force > 0) {
                        (void) must_be_root();
                        return -EPERM;
                }

                /* Try logind if we are a normal user and no special
                 * mode applies. Maybe PolicyKit allows us to shutdown
                 * the machine. */
                if (IN_SET(arg_action, ACTION_POWEROFF, ACTION_REBOOT, ACTION_HALT)) {
                        r = logind_reboot(arg_action);
                        if (r >= 0)
                                return r;
                        if (IN_SET(r, -EOPNOTSUPP, -EINPROGRESS))
                                /* requested operation is not
                                 * supported on the local system or
                                 * already in progress */
                                return r;
                        /* on all other errors, try low-level operation */
                }
        }

        if (!arg_dry_run && !arg_force)
                return start_with_fallback();

        assert(geteuid() == 0);

        if (!arg_no_wtmp) {
                if (sd_booted() > 0)
                        log_debug("Not writing utmp record, assuming that systemd-update-utmp is used.");
                else {
                        r = utmp_put_shutdown();
                        if (r < 0)
                                log_warning_errno(r, "Failed to write utmp record: %m");
                }
        }

        if (arg_dry_run)
                return 0;

        r = halt_now(arg_action);
        return log_error_errno(r, "Failed to reboot: %m");
}

static int runlevel_main(void) {
        int r, runlevel, previous;

        r = utmp_get_runlevel(&runlevel, &previous);
        if (r < 0) {
                puts("unknown");
                return r;
        }

        printf("%c %c\n",
               previous <= 0 ? 'N' : previous,
               runlevel <= 0 ? 'N' : runlevel);

        return 0;
}

static int logind_cancel_shutdown(void) {
#if ENABLE_LOGIND
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus *bus;
        int r;

        r = acquire_bus(BUS_FULL, &bus);
        if (r < 0)
                return r;

        (void) logind_set_wall_message();

        r = sd_bus_call_method(
                        bus,
                        "org.freedesktop.login1",
                        "/org/freedesktop/login1",
                        "org.freedesktop.login1.Manager",
                        "CancelScheduledShutdown",
                        &error,
                        NULL, NULL);
        if (r < 0)
                return log_warning_errno(r, "Failed to talk to logind, shutdown hasn't been cancelled: %s", bus_error_message(&error, r));

        return 0;
#else
        log_error("Not compiled with logind support, cannot cancel scheduled shutdowns.");
        return -ENOSYS;
#endif
}

int main(int argc, char*argv[]) {
        int r;

        argv_cmdline = argv[0];

        setlocale(LC_ALL, "");
        log_parse_environment();
        log_open();
        sigbus_install();

        /* Explicitly not on_tty() to avoid setting cached value.
         * This becomes relevant for piping output which might be
         * ellipsized. */
        original_stdout_is_tty = isatty(STDOUT_FILENO);

        r = parse_argv(argc, argv);
        if (r <= 0)
                goto finish;

        if (arg_action != ACTION_SYSTEMCTL && running_in_chroot() > 0) {

                if (!arg_quiet)
                        log_info("Running in chroot, ignoring request.");
                r = 0;
                goto finish;
        }

        /* systemctl_main() will print an error message for the bus
         * connection, but only if it needs to */

        switch (arg_action) {

        case ACTION_SYSTEMCTL:
                r = systemctl_main(argc, argv);
                break;

        /* Legacy command aliases set arg_action. They provide some fallbacks,
         * e.g. to tell sysvinit to reboot after you have installed systemd
         * binaries. */

        case ACTION_HALT:
        case ACTION_POWEROFF:
        case ACTION_REBOOT:
        case ACTION_KEXEC:
                r = halt_main();
                break;

        case ACTION_RUNLEVEL2:
        case ACTION_RUNLEVEL3:
        case ACTION_RUNLEVEL4:
        case ACTION_RUNLEVEL5:
        case ACTION_RESCUE:
                r = start_with_fallback();
                break;

        case ACTION_RELOAD:
        case ACTION_REEXEC:
                r = reload_with_fallback();
                break;

        case ACTION_CANCEL_SHUTDOWN:
                r = logind_cancel_shutdown();
                break;

        case ACTION_RUNLEVEL:
                r = runlevel_main();
                break;

        case ACTION_EXIT:
        case ACTION_SUSPEND:
        case ACTION_HIBERNATE:
        case ACTION_HYBRID_SLEEP:
        case ACTION_EMERGENCY:
        case ACTION_DEFAULT:
                /* systemctl verbs with no equivalent in the legacy commands.
                 * These cannot appear in arg_action.  Fall through. */

        case _ACTION_INVALID:
        default:
                assert_not_reached("Unknown action");
        }

finish:
        release_busses();

        pager_close();
        ask_password_agent_close();
        polkit_agent_close();

        strv_free(arg_types);
        strv_free(arg_states);
        strv_free(arg_properties);

        strv_free(arg_wall);
        free(arg_root);
        free(arg_esp_path);

        /* Note that we return r here, not EXIT_SUCCESS, so that we can implement the LSB-like return codes */
        return r < 0 ? EXIT_FAILURE : r;
}
