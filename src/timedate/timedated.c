/* SPDX-License-Identifier: LGPL-2.1+ */

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "sd-bus.h"
#include "sd-event.h"
#include "sd-messages.h"

#include "alloc-util.h"
#include "bus-common-errors.h"
#include "bus-error.h"
#include "bus-util.h"
#include "clock-util.h"
#include "def.h"
#include "fileio-label.h"
#include "fs-util.h"
#include "hashmap.h"
#include "list.h"
#include "path-util.h"
#include "selinux-util.h"
#include "string-util.h"
#include "strv.h"
#include "unit-def.h"
#include "unit-name.h"
#include "user-util.h"
#include "util.h"

#define NULL_ADJTIME_UTC "0.0 0 0\n0\nUTC\n"
#define NULL_ADJTIME_LOCAL "0.0 0 0\n0\nLOCAL\n"

typedef struct UnitStatusInfo {
        char *name;
        char *load_state;
        char *unit_file_state;
        char *active_state;

        LIST_FIELDS(struct UnitStatusInfo, units);
} UnitStatusInfo;

typedef struct Context {
        char *zone;
        bool local_rtc;
        Hashmap *polkit_registry;
        sd_bus_message *cache;

        sd_bus_slot *slot_job_removed;
        char *path_ntp_unit;

        LIST_HEAD(UnitStatusInfo, units);
} Context;

static void unit_status_info_clear(UnitStatusInfo *p) {
        assert(p);

        p->load_state = mfree(p->load_state);
        p->unit_file_state = mfree(p->unit_file_state);
        p->active_state = mfree(p->active_state);
}

static void unit_status_info_free(UnitStatusInfo *p) {
        assert(p);

        unit_status_info_clear(p);
        free(p->name);
        free(p);
}

static void context_free(Context *c) {
        UnitStatusInfo *p;

        assert(c);

        free(c->zone);
        bus_verify_polkit_async_registry_free(c->polkit_registry);
        sd_bus_message_unref(c->cache);

        sd_bus_slot_unref(c->slot_job_removed);
        free(c->path_ntp_unit);

        while ((p = c->units)) {
                LIST_REMOVE(units, c->units, p);
                unit_status_info_free(p);
        }
}

static int context_add_ntp_service(Context *c, const char *s) {
        UnitStatusInfo *u;

        if (!unit_name_is_valid(s, UNIT_NAME_PLAIN))
                return -EINVAL;

        /* Do not add this if it is already listed */
        LIST_FOREACH(units, u, c->units)
                if (streq(u->name, s))
                        return 0;

        u = new0(UnitStatusInfo, 1);
        if (!u)
                return -ENOMEM;

        u->name = strdup(s);
        if (!u->name) {
                free(u);
                return -ENOMEM;
        }

        LIST_APPEND(units, c->units, u);

        return 0;
}

static int context_parse_ntp_services(Context *c) {
        const char *env, *p;
        int r;

        assert(c);

        env = getenv("SYSTEMD_TIMEDATED_NTP_SERVICES");
        if (!env) {
                r = context_add_ntp_service(c, "systemd-timesyncd.service");
                if (r < 0)
                        log_warning_errno(r, "Failed to add NTP service \"systemd-timesyncd.service\", ignoring: %m");

                return 0;
        }

        for (p = env;;) {
                _cleanup_free_ char *word = NULL;

                r = extract_first_word(&p, &word, ":", 0);
                if (r == 0)
                        break;
                if (r == -ENOMEM)
                        return log_oom();
                if (r < 0) {
                        log_error("Invalid syntax, ignoring: %s", env);
                        break;
                }

                r = context_add_ntp_service(c, word);
                if (r < 0)
                        log_warning_errno(r, "Failed to add NTP service \"%s\", ignoring: %m", word);
        }

        return 0;
}

static int context_ntp_service_is_active(Context *c) {
        UnitStatusInfo *info;
        int count = 0;

        assert(c);

        /* Call context_update_ntp_status() to update UnitStatusInfo before calling this. */

        LIST_FOREACH(units, info, c->units)
                count += streq_ptr(info->active_state, "active");

        return count;
}

static int context_ntp_service_is_enabled(Context *c) {
        UnitStatusInfo *info;
        int count = 0;

        assert(c);

        /* Call context_update_ntp_status() to update UnitStatusInfo before calling this. */

        LIST_FOREACH(units, info, c->units)
                count += STRPTR_IN_SET(info->unit_file_state, "enabled", "enabled-runtime");

        return count;
}

static int context_ntp_service_exists(Context *c) {
        UnitStatusInfo *info;
        int count = 0;

        assert(c);

        /* Call context_update_ntp_status() to update UnitStatusInfo before calling this. */

        LIST_FOREACH(units, info, c->units)
                count += streq_ptr(info->load_state, "loaded");

        return count;
}

static int context_read_data(Context *c) {
        _cleanup_free_ char *t = NULL;
        int r;

        assert(c);

        r = get_timezone(&t);
        if (r == -EINVAL)
                log_warning_errno(r, "/etc/localtime should be a symbolic link to a time zone data file in /usr/share/zoneinfo/.");
        else if (r < 0)
                log_warning_errno(r, "Failed to get target of /etc/localtime: %m");

        free_and_replace(c->zone, t);

        c->local_rtc = clock_is_localtime(NULL) > 0;

        return 0;
}

static int context_write_data_timezone(Context *c) {
        _cleanup_free_ char *p = NULL;
        int r = 0;

        assert(c);

        if (isempty(c->zone)) {
                if (unlink("/etc/localtime") < 0 && errno != ENOENT)
                        r = -errno;

                return r;
        }

        p = strappend("../usr/share/zoneinfo/", c->zone);
        if (!p)
                return log_oom();

        r = symlink_atomic(p, "/etc/localtime");
        if (r < 0)
                return r;

        return 0;
}

static int context_write_data_local_rtc(Context *c) {
        int r;
        _cleanup_free_ char *s = NULL, *w = NULL;

        assert(c);

        r = read_full_file("/etc/adjtime", &s, NULL);
        if (r < 0) {
                if (r != -ENOENT)
                        return r;

                if (!c->local_rtc)
                        return 0;

                w = strdup(NULL_ADJTIME_LOCAL);
                if (!w)
                        return -ENOMEM;
        } else {
                char *p;
                const char *e = "\n"; /* default if there is less than 3 lines */
                const char *prepend = "";
                size_t a, b;

                p = strchrnul(s, '\n');
                if (*p == '\0')
                        /* only one line, no \n terminator */
                        prepend = "\n0\n";
                else if (p[1] == '\0') {
                        /* only one line, with \n terminator */
                        ++p;
                        prepend = "0\n";
                } else {
                        p = strchr(p+1, '\n');
                        if (!p) {
                                /* only two lines, no \n terminator */
                                prepend = "\n";
                                p = s + strlen(s);
                        } else {
                                char *end;
                                /* third line might have a \n terminator or not */
                                p++;
                                end = strchr(p, '\n');
                                /* if we actually have a fourth line, use that as suffix "e", otherwise the default \n */
                                if (end)
                                        e = end;
                        }
                }

                a = p - s;
                b = strlen(e);

                w = new(char, a + (c->local_rtc ? 5 : 3) + strlen(prepend) + b + 1);
                if (!w)
                        return -ENOMEM;

                *(char*) mempcpy(stpcpy(stpcpy(mempcpy(w, s, a), prepend), c->local_rtc ? "LOCAL" : "UTC"), e, b) = 0;

                if (streq(w, NULL_ADJTIME_UTC)) {
                        if (unlink("/etc/adjtime") < 0)
                                if (errno != ENOENT)
                                        return -errno;

                        return 0;
                }
        }

        mac_selinux_init();
        return write_string_file_atomic_label("/etc/adjtime", w);
}

static int context_update_ntp_status(Context *c, sd_bus *bus, sd_bus_message *m) {
        static const struct bus_properties_map map[] = {
                { "LoadState",     "s", NULL, offsetof(UnitStatusInfo, load_state)      },
                { "ActiveState",   "s", NULL, offsetof(UnitStatusInfo, active_state)    },
                { "UnitFileState", "s", NULL, offsetof(UnitStatusInfo, unit_file_state) },
                {}
        };
        UnitStatusInfo *u;
        int r;

        assert(c);
        assert(bus);

        /* Suppress calling context_update_ntp_status() multiple times within single DBus transaction. */
        if (m) {
                if (m == c->cache)
                        return 0;

                sd_bus_message_unref(c->cache);
                c->cache = sd_bus_message_ref(m);
        }

        LIST_FOREACH(units, u, c->units) {
                _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
                _cleanup_free_ char *path = NULL;

                unit_status_info_clear(u);

                path = unit_dbus_path_from_name(u->name);
                if (!path)
                        return -ENOMEM;

                r = bus_map_all_properties(
                                bus,
                                "org.freedesktop.systemd1",
                                path,
                                map,
                                BUS_MAP_STRDUP,
                                &error,
                                NULL,
                                u);
                if (r < 0)
                        return log_error_errno(r, "Failed to get properties: %s", bus_error_message(&error, r));
        }

        return 0;
}

static int match_job_removed(sd_bus_message *m, void *userdata, sd_bus_error *error) {
        const char *path;
        Context *c = userdata;
        int r;

        assert(c);
        assert(m);

        r = sd_bus_message_read(m, "uoss", NULL, &path, NULL, NULL);
        if (r < 0) {
                bus_log_parse_error(r);
                return 0;
        }

        if (!streq_ptr(path, c->path_ntp_unit))
                return 0;

        (void) sd_bus_emit_properties_changed(sd_bus_message_get_bus(m), "/org/freedesktop/timedate1", "org.freedesktop.timedate1", "NTP", NULL);

        c->slot_job_removed = sd_bus_slot_unref(c->slot_job_removed);
        c->path_ntp_unit = mfree(c->path_ntp_unit);

        return 0;
}

static int unit_start_or_stop(Context *c, UnitStatusInfo *u, sd_bus *bus, sd_bus_error *error, bool start) {
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
        _cleanup_(sd_bus_slot_unrefp) sd_bus_slot *slot = NULL;
        const char *path;
        int r;

        assert(c);
        assert(u);
        assert(bus);
        assert(error);

        /* This method may be called frequently. Forget the previous job if it has not completed yet. */
        c->slot_job_removed = sd_bus_slot_unref(c->slot_job_removed);

        r = sd_bus_match_signal_async(
                        bus,
                        &slot,
                        "org.freedesktop.systemd1",
                        "/org/freedesktop/systemd1",
                        "org.freedesktop.systemd1.Manager",
                        "JobRemoved",
                        match_job_removed, NULL, c);
        if (r < 0)
                return r;

        r = sd_bus_call_method(
                bus,
                "org.freedesktop.systemd1",
                "/org/freedesktop/systemd1",
                "org.freedesktop.systemd1.Manager",
                start ? "StartUnit" : "StopUnit",
                error,
                &reply,
                "ss",
                u->name,
                "replace");
        if (r < 0)
                return r;

        r = sd_bus_message_read(reply, "o", &path);
        if (r < 0)
                return bus_log_parse_error(r);

        r = free_and_strdup(&c->path_ntp_unit, path);
        if (r < 0)
                return log_oom();

        c->slot_job_removed = TAKE_PTR(slot);
        return 0;
}

static int unit_enable_or_disable(UnitStatusInfo *u, sd_bus *bus, sd_bus_error *error, bool enable) {
        int r;

        assert(u);
        assert(bus);
        assert(error);

        /* Call context_update_ntp_status() to update UnitStatusInfo before calling this. */

        if (streq(u->unit_file_state, "enabled") == enable)
                return 0;

        if (enable)
                r = sd_bus_call_method(
                                bus,
                                "org.freedesktop.systemd1",
                                "/org/freedesktop/systemd1",
                                "org.freedesktop.systemd1.Manager",
                                "EnableUnitFiles",
                                error,
                                NULL,
                                "asbb", 1,
                                u->name,
                                false, true);
        else
                r = sd_bus_call_method(
                                bus,
                                "org.freedesktop.systemd1",
                                "/org/freedesktop/systemd1",
                                "org.freedesktop.systemd1.Manager",
                                "DisableUnitFiles",
                                error,
                                NULL,
                                "asb", 1,
                                u->name,
                                false);
        if (r < 0)
                return r;

        r = sd_bus_call_method(
                        bus,
                        "org.freedesktop.systemd1",
                        "/org/freedesktop/systemd1",
                        "org.freedesktop.systemd1.Manager",
                        "Reload",
                        error,
                        NULL,
                        NULL);
        if (r < 0)
                return r;

        return 0;
}

static BUS_DEFINE_PROPERTY_GET_GLOBAL(property_get_time, "t", now(CLOCK_REALTIME));
static BUS_DEFINE_PROPERTY_GET_GLOBAL(property_get_ntp_sync, "b", ntp_synced());

static int property_get_rtc_time(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        struct tm tm;
        usec_t t;
        int r;

        zero(tm);
        r = clock_get_hwclock(&tm);
        if (r == -EBUSY) {
                log_warning("/dev/rtc is busy. Is somebody keeping it open continuously? That's not a good idea... Returning a bogus RTC timestamp.");
                t = 0;
        } else if (r == -ENOENT) {
                log_debug("/dev/rtc not found.");
                t = 0; /* no RTC found */
        } else if (r < 0)
                return sd_bus_error_set_errnof(error, r, "Failed to read RTC: %m");
        else
                t = (usec_t) timegm(&tm) * USEC_PER_SEC;

        return sd_bus_message_append(reply, "t", t);
}

static int property_get_can_ntp(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        Context *c = userdata;
        int r;

        assert(c);
        assert(bus);
        assert(property);
        assert(reply);
        assert(error);

        r = context_update_ntp_status(c, bus, reply);
        if (r < 0)
                return r;

        return sd_bus_message_append(reply, "b", context_ntp_service_exists(c) > 0);
}

static int property_get_ntp(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        Context *c = userdata;
        int r;

        assert(c);
        assert(bus);
        assert(property);
        assert(reply);
        assert(error);

        r = context_update_ntp_status(c, bus, reply);
        if (r < 0)
                return r;

        return sd_bus_message_append(reply, "b", context_ntp_service_is_active(c) > 0);
}

static int method_set_timezone(sd_bus_message *m, void *userdata, sd_bus_error *error) {
        Context *c = userdata;
        int interactive, r;
        const char *z;

        assert(m);
        assert(c);

        r = sd_bus_message_read(m, "sb", &z, &interactive);
        if (r < 0)
                return r;

        if (!timezone_is_valid(z, LOG_DEBUG))
                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid time zone '%s'", z);

        if (streq_ptr(z, c->zone))
                return sd_bus_reply_method_return(m, NULL);

        r = bus_verify_polkit_async(
                        m,
                        CAP_SYS_TIME,
                        "org.freedesktop.timedate1.set-timezone",
                        NULL,
                        interactive,
                        UID_INVALID,
                        &c->polkit_registry,
                        error);
        if (r < 0)
                return r;
        if (r == 0)
                return 1; /* No authorization for now, but the async polkit stuff will call us again when it has it */

        r = free_and_strdup(&c->zone, z);
        if (r < 0)
                return r;

        /* 1. Write new configuration file */
        r = context_write_data_timezone(c);
        if (r < 0) {
                log_error_errno(r, "Failed to set time zone: %m");
                return sd_bus_error_set_errnof(error, r, "Failed to set time zone: %m");
        }

        /* 2. Make glibc notice the new timezone */
        tzset();

        /* 3. Tell the kernel our timezone */
        r = clock_set_timezone(NULL);
        if (r < 0)
                log_debug_errno(r, "Failed to tell kernel about timezone, ignoring: %m");

        if (c->local_rtc) {
                struct timespec ts;
                struct tm tm;

                /* 4. Sync RTC from system clock, with the new delta */
                assert_se(clock_gettime(CLOCK_REALTIME, &ts) == 0);
                assert_se(localtime_r(&ts.tv_sec, &tm));

                r = clock_set_hwclock(&tm);
                if (r < 0)
                        log_debug_errno(r, "Failed to sync time to hardware clock, ignoring: %m");
        }

        log_struct(LOG_INFO,
                   "MESSAGE_ID=" SD_MESSAGE_TIMEZONE_CHANGE_STR,
                   "TIMEZONE=%s", c->zone,
                   "TIMEZONE_SHORTNAME=%s", tzname[daylight],
                   "DAYLIGHT=%i", daylight,
                   LOG_MESSAGE("Changed time zone to '%s' (%s).", c->zone, tzname[daylight]));

        (void) sd_bus_emit_properties_changed(sd_bus_message_get_bus(m), "/org/freedesktop/timedate1", "org.freedesktop.timedate1", "Timezone", NULL);

        return sd_bus_reply_method_return(m, NULL);
}

static int method_set_local_rtc(sd_bus_message *m, void *userdata, sd_bus_error *error) {
        int lrtc, fix_system, interactive;
        Context *c = userdata;
        struct timespec ts;
        int r;

        assert(m);
        assert(c);

        r = sd_bus_message_read(m, "bbb", &lrtc, &fix_system, &interactive);
        if (r < 0)
                return r;

        if (lrtc == c->local_rtc)
                return sd_bus_reply_method_return(m, NULL);

        r = bus_verify_polkit_async(
                        m,
                        CAP_SYS_TIME,
                        "org.freedesktop.timedate1.set-local-rtc",
                        NULL,
                        interactive,
                        UID_INVALID,
                        &c->polkit_registry,
                        error);
        if (r < 0)
                return r;
        if (r == 0)
                return 1;

        c->local_rtc = lrtc;

        /* 1. Write new configuration file */
        r = context_write_data_local_rtc(c);
        if (r < 0) {
                log_error_errno(r, "Failed to set RTC to local/UTC: %m");
                return sd_bus_error_set_errnof(error, r, "Failed to set RTC to local/UTC: %m");
        }

        /* 2. Tell the kernel our timezone */
        r = clock_set_timezone(NULL);
        if (r < 0)
                log_debug_errno(r, "Failed to tell kernel about timezone, ignoring: %m");

        /* 3. Synchronize clocks */
        assert_se(clock_gettime(CLOCK_REALTIME, &ts) == 0);

        if (fix_system) {
                struct tm tm;

                /* Sync system clock from RTC; first, initialize the timezone fields of struct tm. */
                if (c->local_rtc)
                        localtime_r(&ts.tv_sec, &tm);
                else
                        gmtime_r(&ts.tv_sec, &tm);

                /* Override the main fields of struct tm, but not the timezone fields */
                r = clock_get_hwclock(&tm);
                if (r < 0)
                        log_debug_errno(r, "Failed to get hardware clock, ignoring: %m");
                else {
                        /* And set the system clock with this */
                        if (c->local_rtc)
                                ts.tv_sec = mktime(&tm);
                        else
                                ts.tv_sec = timegm(&tm);

                        if (clock_settime(CLOCK_REALTIME, &ts) < 0)
                                log_debug_errno(errno, "Failed to update system clock, ignoring: %m");
                }

        } else {
                struct tm tm;

                /* Sync RTC from system clock */
                if (c->local_rtc)
                        localtime_r(&ts.tv_sec, &tm);
                else
                        gmtime_r(&ts.tv_sec, &tm);

                r = clock_set_hwclock(&tm);
                if (r < 0)
                        log_debug_errno(r, "Failed to sync time to hardware clock, ignoring: %m");
        }

        log_info("RTC configured to %s time.", c->local_rtc ? "local" : "UTC");

        (void) sd_bus_emit_properties_changed(sd_bus_message_get_bus(m), "/org/freedesktop/timedate1", "org.freedesktop.timedate1", "LocalRTC", NULL);

        return sd_bus_reply_method_return(m, NULL);
}

static int method_set_time(sd_bus_message *m, void *userdata, sd_bus_error *error) {
        sd_bus *bus = sd_bus_message_get_bus(m);
        int relative, interactive, r;
        Context *c = userdata;
        int64_t utc;
        struct timespec ts;
        usec_t start;
        struct tm tm;

        assert(m);
        assert(c);

        r = context_update_ntp_status(c, bus, m);
        if (r < 0)
                return sd_bus_error_set_errnof(error, r, "Failed to update context: %m");

        if (context_ntp_service_is_active(c) > 0)
                return sd_bus_error_set(error, BUS_ERROR_AUTOMATIC_TIME_SYNC_ENABLED, "Automatic time synchronization is enabled");

        /* this only gets used if dbus does not provide a timestamp */
        start = now(CLOCK_MONOTONIC);

        r = sd_bus_message_read(m, "xbb", &utc, &relative, &interactive);
        if (r < 0)
                return r;

        if (!relative && utc <= 0)
                return sd_bus_error_set(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid absolute time");

        if (relative && utc == 0)
                return sd_bus_reply_method_return(m, NULL);

        if (relative) {
                usec_t n, x;

                n = now(CLOCK_REALTIME);
                x = n + utc;

                if ((utc > 0 && x < n) ||
                    (utc < 0 && x > n))
                        return sd_bus_error_set(error, SD_BUS_ERROR_INVALID_ARGS, "Time value overflow");

                timespec_store(&ts, x);
        } else
                timespec_store(&ts, (usec_t) utc);

        r = bus_verify_polkit_async(
                        m,
                        CAP_SYS_TIME,
                        "org.freedesktop.timedate1.set-time",
                        NULL,
                        interactive,
                        UID_INVALID,
                        &c->polkit_registry,
                        error);
        if (r < 0)
                return r;
        if (r == 0)
                return 1;

        /* adjust ts for time spent in program */
        r = sd_bus_message_get_monotonic_usec(m, &start);
        /* when sd_bus_message_get_monotonic_usec() returns -ENODATA it does not modify &start */
        if (r < 0 && r != -ENODATA)
                return r;

        timespec_store(&ts, timespec_load(&ts) + (now(CLOCK_MONOTONIC) - start));

        /* Set system clock */
        if (clock_settime(CLOCK_REALTIME, &ts) < 0) {
                log_error_errno(errno, "Failed to set local time: %m");
                return sd_bus_error_set_errnof(error, errno, "Failed to set local time: %m");
        }

        /* Sync down to RTC */
        if (c->local_rtc)
                localtime_r(&ts.tv_sec, &tm);
        else
                gmtime_r(&ts.tv_sec, &tm);

        r = clock_set_hwclock(&tm);
        if (r < 0)
                log_debug_errno(r, "Failed to update hardware clock, ignoring: %m");

        log_struct(LOG_INFO,
                   "MESSAGE_ID=" SD_MESSAGE_TIME_CHANGE_STR,
                   "REALTIME="USEC_FMT, timespec_load(&ts),
                   LOG_MESSAGE("Changed local time to %s", ctime(&ts.tv_sec)));

        return sd_bus_reply_method_return(m, NULL);
}

static int method_set_ntp(sd_bus_message *m, void *userdata, sd_bus_error *error) {
        sd_bus *bus = sd_bus_message_get_bus(m);
        Context *c = userdata;
        UnitStatusInfo *u;
        int enable, interactive, q, r;

        assert(m);
        assert(bus);
        assert(c);

        r = sd_bus_message_read(m, "bb", &enable, &interactive);
        if (r < 0)
                return r;

        r = context_update_ntp_status(c, bus, m);
        if (r < 0)
                return r;

        if (context_ntp_service_exists(c) <= 0)
                return sd_bus_error_set(error, BUS_ERROR_NO_NTP_SUPPORT, "NTP not supported");

        r = bus_verify_polkit_async(
                        m,
                        CAP_SYS_TIME,
                        "org.freedesktop.timedate1.set-ntp",
                        NULL,
                        interactive,
                        UID_INVALID,
                        &c->polkit_registry,
                        error);
        if (r < 0)
                return r;
        if (r == 0)
                return 1;

        if (!enable)
                LIST_FOREACH(units, u, c->units) {
                        if (!streq(u->load_state, "loaded"))
                                continue;

                        q = unit_enable_or_disable(u, bus, error, enable);
                        if (q < 0)
                                r = q;

                        q = unit_start_or_stop(c, u, bus, error, enable);
                        if (q < 0)
                                r = q;
                }

        else if (context_ntp_service_is_enabled(c) <= 0)
                LIST_FOREACH(units, u, c->units) {
                        if (!streq(u->load_state, "loaded"))
                                continue;

                        r = unit_enable_or_disable(u, bus, error, enable);
                        if (r < 0)
                                continue;

                        r = unit_start_or_stop(c, u, bus, error, enable);
                        break;
                }

        else
                LIST_FOREACH(units, u, c->units) {
                        if (!streq(u->load_state, "loaded") ||
                            !streq(u->unit_file_state, "enabled"))
                                continue;

                        r = unit_start_or_stop(c, u, bus, error, enable);
                        break;
                }

        if (r < 0)
                return r;

        log_info("Set NTP to %sd", enable_disable(enable));

        return sd_bus_reply_method_return(m, NULL);
}

static const sd_bus_vtable timedate_vtable[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_PROPERTY("Timezone", "s", NULL, offsetof(Context, zone), SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("LocalRTC", "b", bus_property_get_bool, offsetof(Context, local_rtc), SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("CanNTP", "b", property_get_can_ntp, 0, 0),
        SD_BUS_PROPERTY("NTP", "b", property_get_ntp, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("NTPSynchronized", "b", property_get_ntp_sync, 0, 0),
        SD_BUS_PROPERTY("TimeUSec", "t", property_get_time, 0, 0),
        SD_BUS_PROPERTY("RTCTimeUSec", "t", property_get_rtc_time, 0, 0),
        SD_BUS_METHOD("SetTime", "xbb", NULL, method_set_time, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("SetTimezone", "sb", NULL, method_set_timezone, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("SetLocalRTC", "bbb", NULL, method_set_local_rtc, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("SetNTP", "bb", NULL, method_set_ntp, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_VTABLE_END,
};

static int connect_bus(Context *c, sd_event *event, sd_bus **_bus) {
        _cleanup_(sd_bus_flush_close_unrefp) sd_bus *bus = NULL;
        int r;

        assert(c);
        assert(event);
        assert(_bus);

        r = sd_bus_default_system(&bus);
        if (r < 0)
                return log_error_errno(r, "Failed to get system bus connection: %m");

        r = sd_bus_add_object_vtable(bus, NULL, "/org/freedesktop/timedate1", "org.freedesktop.timedate1", timedate_vtable, c);
        if (r < 0)
                return log_error_errno(r, "Failed to register object: %m");

        r = sd_bus_request_name_async(bus, NULL, "org.freedesktop.timedate1", 0, NULL, NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to request name: %m");

        r = sd_bus_attach_event(bus, event, 0);
        if (r < 0)
                return log_error_errno(r, "Failed to attach bus to event loop: %m");

        *_bus = TAKE_PTR(bus);

        return 0;
}

int main(int argc, char *argv[]) {
        Context context = {};
        _cleanup_(sd_event_unrefp) sd_event *event = NULL;
        _cleanup_(sd_bus_flush_close_unrefp) sd_bus *bus = NULL;
        int r;

        log_set_target(LOG_TARGET_AUTO);
        log_parse_environment();
        log_open();

        umask(0022);

        if (argc != 1) {
                log_error("This program takes no arguments.");
                r = -EINVAL;
                goto finish;
        }

        r = sd_event_default(&event);
        if (r < 0) {
                log_error_errno(r, "Failed to allocate event loop: %m");
                goto finish;
        }

        sd_event_set_watchdog(event, true);

        r = connect_bus(&context, event, &bus);
        if (r < 0)
                goto finish;

        (void) sd_bus_negotiate_timestamp(bus, true);

        r = context_read_data(&context);
        if (r < 0) {
                log_error_errno(r, "Failed to read time zone data: %m");
                goto finish;
        }

        r = context_parse_ntp_services(&context);
        if (r < 0)
                goto finish;

        r = bus_event_loop_with_idle(event, bus, "org.freedesktop.timedate1", DEFAULT_EXIT_USEC, NULL, NULL);
        if (r < 0) {
                log_error_errno(r, "Failed to run event loop: %m");
                goto finish;
        }

finish:
        context_free(&context);

        return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
