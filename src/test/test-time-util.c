/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "env-util.h"
#include "random-util.h"
#include "serialize.h"
#include "string-util.h"
#include "strv.h"
#include "tests.h"
#include "time-util.h"

TEST(parse_sec) {
        usec_t u;

        assert_se(parse_sec("5s", &u) >= 0);
        assert_se(u == 5 * USEC_PER_SEC);
        assert_se(parse_sec("5s500ms", &u) >= 0);
        assert_se(u == 5 * USEC_PER_SEC + 500 * USEC_PER_MSEC);
        assert_se(parse_sec(" 5s 500ms  ", &u) >= 0);
        assert_se(u == 5 * USEC_PER_SEC + 500 * USEC_PER_MSEC);
        assert_se(parse_sec(" 5.5s  ", &u) >= 0);
        assert_se(u == 5 * USEC_PER_SEC + 500 * USEC_PER_MSEC);
        assert_se(parse_sec(" 5.5s 0.5ms ", &u) >= 0);
        assert_se(u == 5 * USEC_PER_SEC + 500 * USEC_PER_MSEC + 500);
        assert_se(parse_sec(" .22s ", &u) >= 0);
        assert_se(u == 220 * USEC_PER_MSEC);
        assert_se(parse_sec(" .50y ", &u) >= 0);
        assert_se(u == USEC_PER_YEAR / 2);
        assert_se(parse_sec("2.5", &u) >= 0);
        assert_se(u == 2500 * USEC_PER_MSEC);
        assert_se(parse_sec(".7", &u) >= 0);
        assert_se(u == 700 * USEC_PER_MSEC);
        assert_se(parse_sec("23us", &u) >= 0);
        assert_se(u == 23);
        assert_se(parse_sec("23μs", &u) >= 0); /* greek small letter mu */
        assert_se(u == 23);
        assert_se(parse_sec("23µs", &u) >= 0); /* micro symbol */
        assert_se(u == 23);
        assert_se(parse_sec("infinity", &u) >= 0);
        assert_se(u == USEC_INFINITY);
        assert_se(parse_sec(" infinity ", &u) >= 0);
        assert_se(u == USEC_INFINITY);
        assert_se(parse_sec("+3.1s", &u) >= 0);
        assert_se(u == 3100 * USEC_PER_MSEC);
        assert_se(parse_sec("3.1s.2", &u) >= 0);
        assert_se(u == 3300 * USEC_PER_MSEC);
        assert_se(parse_sec("3.1 .2", &u) >= 0);
        assert_se(u == 3300 * USEC_PER_MSEC);
        assert_se(parse_sec("3.1 sec .2 sec", &u) >= 0);
        assert_se(u == 3300 * USEC_PER_MSEC);
        assert_se(parse_sec("3.1 sec 1.2 sec", &u) >= 0);
        assert_se(u == 4300 * USEC_PER_MSEC);

        assert_se(parse_sec(" xyz ", &u) < 0);
        assert_se(parse_sec("", &u) < 0);
        assert_se(parse_sec(" . ", &u) < 0);
        assert_se(parse_sec(" 5. ", &u) < 0);
        assert_se(parse_sec(".s ", &u) < 0);
        assert_se(parse_sec("-5s ", &u) < 0);
        assert_se(parse_sec("-0.3s ", &u) < 0);
        assert_se(parse_sec("-0.0s ", &u) < 0);
        assert_se(parse_sec("-0.-0s ", &u) < 0);
        assert_se(parse_sec("0.-0s ", &u) < 0);
        assert_se(parse_sec("3.-0s ", &u) < 0);
        assert_se(parse_sec(" infinity .7", &u) < 0);
        assert_se(parse_sec(".3 infinity", &u) < 0);
        assert_se(parse_sec("3.+1s", &u) < 0);
        assert_se(parse_sec("3. 1s", &u) < 0);
        assert_se(parse_sec("3.s", &u) < 0);
        assert_se(parse_sec("12.34.56", &u) < 0);
        assert_se(parse_sec("12..34", &u) < 0);
        assert_se(parse_sec("..1234", &u) < 0);
        assert_se(parse_sec("1234..", &u) < 0);
}

TEST(parse_sec_fix_0) {
        usec_t u;

        assert_se(parse_sec_fix_0("5s", &u) >= 0);
        assert_se(u == 5 * USEC_PER_SEC);
        assert_se(parse_sec_fix_0("0s", &u) >= 0);
        assert_se(u == USEC_INFINITY);
        assert_se(parse_sec_fix_0("0", &u) >= 0);
        assert_se(u == USEC_INFINITY);
        assert_se(parse_sec_fix_0(" 0", &u) >= 0);
        assert_se(u == USEC_INFINITY);
}

TEST(parse_sec_def_infinity) {
        usec_t u;

        assert_se(parse_sec_def_infinity("5s", &u) >= 0);
        assert_se(u == 5 * USEC_PER_SEC);
        assert_se(parse_sec_def_infinity("", &u) >= 0);
        assert_se(u == USEC_INFINITY);
        assert_se(parse_sec_def_infinity("     ", &u) >= 0);
        assert_se(u == USEC_INFINITY);
        assert_se(parse_sec_def_infinity("0s", &u) >= 0);
        assert_se(u == 0);
        assert_se(parse_sec_def_infinity("0", &u) >= 0);
        assert_se(u == 0);
        assert_se(parse_sec_def_infinity(" 0", &u) >= 0);
        assert_se(u == 0);
        assert_se(parse_sec_def_infinity("-5s", &u) < 0);
}

TEST(parse_time) {
        usec_t u;

        assert_se(parse_time("5", &u, 1) >= 0);
        assert_se(u == 5);

        assert_se(parse_time("5", &u, USEC_PER_MSEC) >= 0);
        assert_se(u == 5 * USEC_PER_MSEC);

        assert_se(parse_time("5", &u, USEC_PER_SEC) >= 0);
        assert_se(u == 5 * USEC_PER_SEC);

        assert_se(parse_time("5s", &u, 1) >= 0);
        assert_se(u == 5 * USEC_PER_SEC);

        assert_se(parse_time("5s", &u, USEC_PER_SEC) >= 0);
        assert_se(u == 5 * USEC_PER_SEC);

        assert_se(parse_time("5s", &u, USEC_PER_MSEC) >= 0);
        assert_se(u == 5 * USEC_PER_SEC);

        assert_se(parse_time("11111111111111y", &u, 1) == -ERANGE);
        assert_se(parse_time("1.1111111111111y", &u, 1) >= 0);
}

TEST(parse_nsec) {
        nsec_t u;

        assert_se(parse_nsec("5s", &u) >= 0);
        assert_se(u == 5 * NSEC_PER_SEC);
        assert_se(parse_nsec("5s500ms", &u) >= 0);
        assert_se(u == 5 * NSEC_PER_SEC + 500 * NSEC_PER_MSEC);
        assert_se(parse_nsec(" 5s 500ms  ", &u) >= 0);
        assert_se(u == 5 * NSEC_PER_SEC + 500 * NSEC_PER_MSEC);
        assert_se(parse_nsec(" 5.5s  ", &u) >= 0);
        assert_se(u == 5 * NSEC_PER_SEC + 500 * NSEC_PER_MSEC);
        assert_se(parse_nsec(" 5.5s 0.5ms ", &u) >= 0);
        assert_se(u == 5 * NSEC_PER_SEC + 500 * NSEC_PER_MSEC + 500 * NSEC_PER_USEC);
        assert_se(parse_nsec(" .22s ", &u) >= 0);
        assert_se(u == 220 * NSEC_PER_MSEC);
        assert_se(parse_nsec(" .50y ", &u) >= 0);
        assert_se(u == NSEC_PER_YEAR / 2);
        assert_se(parse_nsec("2.5", &u) >= 0);
        assert_se(u == 2);
        assert_se(parse_nsec(".7", &u) >= 0);
        assert_se(u == 0);
        assert_se(parse_nsec("infinity", &u) >= 0);
        assert_se(u == NSEC_INFINITY);
        assert_se(parse_nsec(" infinity ", &u) >= 0);
        assert_se(u == NSEC_INFINITY);
        assert_se(parse_nsec("+3.1s", &u) >= 0);
        assert_se(u == 3100 * NSEC_PER_MSEC);
        assert_se(parse_nsec("3.1s.2", &u) >= 0);
        assert_se(u == 3100 * NSEC_PER_MSEC);
        assert_se(parse_nsec("3.1 .2s", &u) >= 0);
        assert_se(u == 200 * NSEC_PER_MSEC + 3);
        assert_se(parse_nsec("3.1 sec .2 sec", &u) >= 0);
        assert_se(u == 3300 * NSEC_PER_MSEC);
        assert_se(parse_nsec("3.1 sec 1.2 sec", &u) >= 0);
        assert_se(u == 4300 * NSEC_PER_MSEC);

        assert_se(parse_nsec(" xyz ", &u) < 0);
        assert_se(parse_nsec("", &u) < 0);
        assert_se(parse_nsec(" . ", &u) < 0);
        assert_se(parse_nsec(" 5. ", &u) < 0);
        assert_se(parse_nsec(".s ", &u) < 0);
        assert_se(parse_nsec(" infinity .7", &u) < 0);
        assert_se(parse_nsec(".3 infinity", &u) < 0);
        assert_se(parse_nsec("-5s ", &u) < 0);
        assert_se(parse_nsec("-0.3s ", &u) < 0);
        assert_se(parse_nsec("-0.0s ", &u) < 0);
        assert_se(parse_nsec("-0.-0s ", &u) < 0);
        assert_se(parse_nsec("0.-0s ", &u) < 0);
        assert_se(parse_nsec("3.-0s ", &u) < 0);
        assert_se(parse_nsec(" infinity .7", &u) < 0);
        assert_se(parse_nsec(".3 infinity", &u) < 0);
        assert_se(parse_nsec("3.+1s", &u) < 0);
        assert_se(parse_nsec("3. 1s", &u) < 0);
        assert_se(parse_nsec("3.s", &u) < 0);
        assert_se(parse_nsec("12.34.56", &u) < 0);
        assert_se(parse_nsec("12..34", &u) < 0);
        assert_se(parse_nsec("..1234", &u) < 0);
        assert_se(parse_nsec("1234..", &u) < 0);
        assert_se(parse_nsec("1111111111111y", &u) == -ERANGE);
        assert_se(parse_nsec("1.111111111111y", &u) >= 0);
}

static void test_format_timespan_one(usec_t x, usec_t accuracy) {
        char l[FORMAT_TIMESPAN_MAX];
        const char *t;
        usec_t y;

        log_debug(USEC_FMT"     (at accuracy "USEC_FMT")", x, accuracy);

        assert_se(t = format_timespan(l, sizeof l, x, accuracy));
        log_debug(" = <%s>", t);

        assert_se(parse_sec(t, &y) >= 0);
        log_debug(" = "USEC_FMT, y);

        if (accuracy <= 0)
                accuracy = 1;

        assert_se(x / accuracy == y / accuracy);
}

static void test_format_timespan_accuracy(usec_t accuracy) {
        log_info("/* %s accuracy="USEC_FMT" */", __func__, accuracy);

        test_format_timespan_one(0, accuracy);
        test_format_timespan_one(1, accuracy);
        test_format_timespan_one(1*USEC_PER_SEC, accuracy);
        test_format_timespan_one(999*USEC_PER_MSEC, accuracy);
        test_format_timespan_one(1234567, accuracy);
        test_format_timespan_one(12, accuracy);
        test_format_timespan_one(123, accuracy);
        test_format_timespan_one(1234, accuracy);
        test_format_timespan_one(12345, accuracy);
        test_format_timespan_one(123456, accuracy);
        test_format_timespan_one(1234567, accuracy);
        test_format_timespan_one(12345678, accuracy);
        test_format_timespan_one(1200000, accuracy);
        test_format_timespan_one(1230000, accuracy);
        test_format_timespan_one(1234000, accuracy);
        test_format_timespan_one(1234500, accuracy);
        test_format_timespan_one(1234560, accuracy);
        test_format_timespan_one(1234567, accuracy);
        test_format_timespan_one(986087, accuracy);
        test_format_timespan_one(500 * USEC_PER_MSEC, accuracy);
        test_format_timespan_one(9*USEC_PER_YEAR/5 - 23, accuracy);
        test_format_timespan_one(USEC_INFINITY, accuracy);
}

TEST(format_timespan) {
        test_format_timespan_accuracy(1);
        test_format_timespan_accuracy(USEC_PER_MSEC);
        test_format_timespan_accuracy(USEC_PER_SEC);

        /* See issue #23928. */
        _cleanup_free_ char *buf = NULL;
        assert_se(buf = new(char, 5));
        assert_se(buf == format_timespan(buf, 5, 100005, 1000));
}

TEST(verify_timezone) {
        assert_se(verify_timezone("Europe/Berlin", LOG_DEBUG) == 0);
        assert_se(verify_timezone("Australia/Sydney", LOG_DEBUG) == 0);
        assert_se(verify_timezone("Europe/Do not exist", LOG_DEBUG) == -EINVAL);
        assert_se(verify_timezone("Europe/DoNotExist", LOG_DEBUG) == -ENOENT);
        assert_se(verify_timezone("/DoNotExist", LOG_DEBUG) == -EINVAL);
        assert_se(verify_timezone("DoNotExist/", LOG_DEBUG) == -EINVAL);
}

TEST(timezone_is_valid) {
        assert_se(timezone_is_valid("Europe/Berlin", LOG_ERR));
        assert_se(timezone_is_valid("Australia/Sydney", LOG_ERR));
        assert_se(!timezone_is_valid("Europe/Do not exist", LOG_ERR));
}

TEST(get_timezones) {
        _cleanup_strv_free_ char **zones = NULL;
        int r;

        r = get_timezones(&zones);
        assert_se(r == 0);

        STRV_FOREACH(zone, zones) {
                r = verify_timezone(*zone, LOG_ERR);
                log_debug_errno(r, "verify_timezone(\"%s\"): %m", *zone);
                assert_se(r >= 0 || r == -ENOENT);
        }
}

TEST(usec_add) {
        assert_se(usec_add(0, 0) == 0);
        assert_se(usec_add(1, 4) == 5);
        assert_se(usec_add(USEC_INFINITY, 5) == USEC_INFINITY);
        assert_se(usec_add(5, USEC_INFINITY) == USEC_INFINITY);
        assert_se(usec_add(USEC_INFINITY-5, 2) == USEC_INFINITY-3);
        assert_se(usec_add(USEC_INFINITY-2, 2) == USEC_INFINITY);
        assert_se(usec_add(USEC_INFINITY-1, 2) == USEC_INFINITY);
        assert_se(usec_add(USEC_INFINITY, 2) == USEC_INFINITY);
}

TEST(usec_sub_unsigned) {
        assert_se(usec_sub_unsigned(0, 0) == 0);
        assert_se(usec_sub_unsigned(0, 2) == 0);
        assert_se(usec_sub_unsigned(0, USEC_INFINITY) == 0);
        assert_se(usec_sub_unsigned(1, 0) == 1);
        assert_se(usec_sub_unsigned(1, 1) == 0);
        assert_se(usec_sub_unsigned(1, 2) == 0);
        assert_se(usec_sub_unsigned(1, 3) == 0);
        assert_se(usec_sub_unsigned(1, USEC_INFINITY) == 0);
        assert_se(usec_sub_unsigned(USEC_INFINITY-1, 0) == USEC_INFINITY-1);
        assert_se(usec_sub_unsigned(USEC_INFINITY-1, 1) == USEC_INFINITY-2);
        assert_se(usec_sub_unsigned(USEC_INFINITY-1, 2) == USEC_INFINITY-3);
        assert_se(usec_sub_unsigned(USEC_INFINITY-1, USEC_INFINITY-2) == 1);
        assert_se(usec_sub_unsigned(USEC_INFINITY-1, USEC_INFINITY-1) == 0);
        assert_se(usec_sub_unsigned(USEC_INFINITY-1, USEC_INFINITY) == 0);
        assert_se(usec_sub_unsigned(USEC_INFINITY, 0) == USEC_INFINITY);
        assert_se(usec_sub_unsigned(USEC_INFINITY, 1) == USEC_INFINITY);
        assert_se(usec_sub_unsigned(USEC_INFINITY, 2) == USEC_INFINITY);
        assert_se(usec_sub_unsigned(USEC_INFINITY, USEC_INFINITY) == USEC_INFINITY);
}

TEST(usec_sub_signed) {
        assert_se(usec_sub_signed(0, 0) == 0);
        assert_se(usec_sub_signed(4, 1) == 3);
        assert_se(usec_sub_signed(4, 4) == 0);
        assert_se(usec_sub_signed(4, 5) == 0);

        assert_se(usec_sub_signed(USEC_INFINITY-3, -3) == USEC_INFINITY);
        assert_se(usec_sub_signed(USEC_INFINITY-3, -4) == USEC_INFINITY);
        assert_se(usec_sub_signed(USEC_INFINITY-3, -5) == USEC_INFINITY);
        assert_se(usec_sub_signed(USEC_INFINITY, 5) == USEC_INFINITY);

        assert_se(usec_sub_signed(0, INT64_MAX) == 0);
        assert_se(usec_sub_signed(0, -INT64_MAX) == INT64_MAX);
        assert_se(usec_sub_signed(0, INT64_MIN) == (usec_t) INT64_MAX + 1);
        assert_se(usec_sub_signed(0, -(INT64_MIN+1)) == 0);

        assert_se(usec_sub_signed(USEC_INFINITY, INT64_MAX) == USEC_INFINITY);
        assert_se(usec_sub_signed(USEC_INFINITY, -INT64_MAX) == USEC_INFINITY);
        assert_se(usec_sub_signed(USEC_INFINITY, INT64_MIN) == USEC_INFINITY);
        assert_se(usec_sub_signed(USEC_INFINITY, -(INT64_MIN+1)) == USEC_INFINITY);

        assert_se(usec_sub_signed(USEC_INFINITY-1, INT64_MAX) == USEC_INFINITY-1-INT64_MAX);
        assert_se(usec_sub_signed(USEC_INFINITY-1, -INT64_MAX) == USEC_INFINITY);
        assert_se(usec_sub_signed(USEC_INFINITY-1, INT64_MIN) == USEC_INFINITY);
        assert_se(usec_sub_signed(USEC_INFINITY-1, -(INT64_MIN+1)) == USEC_INFINITY-1-((usec_t) (-(INT64_MIN+1))));
}

TEST(format_timestamp) {
        for (unsigned i = 0; i < 100; i++) {
                char buf[CONST_MAX(FORMAT_TIMESTAMP_MAX, FORMAT_TIMESPAN_MAX)];
                usec_t x, y;

                x = random_u64_range(2147483600 * USEC_PER_SEC) + 1;

                assert_se(format_timestamp(buf, sizeof(buf), x));
                log_debug("%s", buf);
                assert_se(parse_timestamp(buf, &y) >= 0);
                assert_se(x / USEC_PER_SEC == y / USEC_PER_SEC);

                assert_se(format_timestamp_style(buf, sizeof(buf), x, TIMESTAMP_UNIX));
                log_debug("%s", buf);
                assert_se(parse_timestamp(buf, &y) >= 0);
                assert_se(x / USEC_PER_SEC == y / USEC_PER_SEC);

                assert_se(format_timestamp_style(buf, sizeof(buf), x, TIMESTAMP_UTC));
                log_debug("%s", buf);
                assert_se(parse_timestamp(buf, &y) >= 0);
                assert_se(x / USEC_PER_SEC == y / USEC_PER_SEC);

                assert_se(format_timestamp_style(buf, sizeof(buf), x, TIMESTAMP_US));
                log_debug("%s", buf);
                assert_se(parse_timestamp(buf, &y) >= 0);
                assert_se(x == y);

                assert_se(format_timestamp_style(buf, sizeof(buf), x, TIMESTAMP_US_UTC));
                log_debug("%s", buf);
                assert_se(parse_timestamp(buf, &y) >= 0);
                assert_se(x == y);

                assert_se(format_timestamp_relative(buf, sizeof(buf), x));
                log_debug("%s", buf);
                assert_se(parse_timestamp(buf, &y) >= 0);

                /* The two calls above will run with a slightly different local time. Make sure we are in the same
                 * range however, but give enough leeway that this is unlikely to explode. And of course,
                 * format_timestamp_relative() scales the accuracy with the distance from the current time up to one
                 * month, cover for that too. */
                assert_se(y > x ? y - x : x - y <= USEC_PER_MONTH + USEC_PER_DAY);
        }
}

TEST(FORMAT_TIMESTAMP) {
        for (unsigned i = 0; i < 100; i++) {
                _cleanup_free_ char *buf;
                usec_t x, y;

                x = random_u64_range(2147483600 * USEC_PER_SEC) + 1;

                /* strbuf() is to test the macro in an argument to a function call. */
                assert_se(buf = strdup(FORMAT_TIMESTAMP(x)));
                log_debug("%s", buf);
                assert_se(parse_timestamp(buf, &y) >= 0);
                assert_se(x / USEC_PER_SEC == y / USEC_PER_SEC);

                assert_se(streq(FORMAT_TIMESTAMP(x), buf));
        }
}

TEST(format_timestamp_relative) {
        char buf[CONST_MAX(FORMAT_TIMESTAMP_MAX, FORMAT_TIMESPAN_MAX)];
        usec_t x;

        /* Only testing timestamps in the past so we don't need to add some delta to account for time passing
         * by while we are running the tests (unless we're running on potatoes and 24 hours somehow passes
         * between our call to now() and format_timestamp_relative's call to now()). */

        /* Years and months */
        x = now(CLOCK_REALTIME) - (1*USEC_PER_YEAR + 1*USEC_PER_MONTH);
        assert_se(format_timestamp_relative(buf, sizeof(buf), x));
        log_debug("%s", buf);
        assert_se(streq(buf, "1 year 1 month ago"));

        x = now(CLOCK_REALTIME) - (1*USEC_PER_YEAR + 2*USEC_PER_MONTH);
        assert_se(format_timestamp_relative(buf, sizeof(buf), x));
        log_debug("%s", buf);
        assert_se(streq(buf, "1 year 2 months ago"));

        x = now(CLOCK_REALTIME) - (2*USEC_PER_YEAR + 1*USEC_PER_MONTH);
        assert_se(format_timestamp_relative(buf, sizeof(buf), x));
        log_debug("%s", buf);
        assert_se(streq(buf, "2 years 1 month ago"));

        x = now(CLOCK_REALTIME) - (2*USEC_PER_YEAR + 2*USEC_PER_MONTH);
        assert_se(format_timestamp_relative(buf, sizeof(buf), x));
        log_debug("%s", buf);
        assert_se(streq(buf, "2 years 2 months ago"));

        /* Months and days */
        x = now(CLOCK_REALTIME) - (1*USEC_PER_MONTH + 1*USEC_PER_DAY);
        assert_se(format_timestamp_relative(buf, sizeof(buf), x));
        log_debug("%s", buf);
        assert_se(streq(buf, "1 month 1 day ago"));

        x = now(CLOCK_REALTIME) - (1*USEC_PER_MONTH + 2*USEC_PER_DAY);
        assert_se(format_timestamp_relative(buf, sizeof(buf), x));
        log_debug("%s", buf);
        assert_se(streq(buf, "1 month 2 days ago"));

        x = now(CLOCK_REALTIME) - (2*USEC_PER_MONTH + 1*USEC_PER_DAY);
        assert_se(format_timestamp_relative(buf, sizeof(buf), x));
        log_debug("%s", buf);
        assert_se(streq(buf, "2 months 1 day ago"));

        x = now(CLOCK_REALTIME) - (2*USEC_PER_MONTH + 2*USEC_PER_DAY);
        assert_se(format_timestamp_relative(buf, sizeof(buf), x));
        log_debug("%s", buf);
        assert_se(streq(buf, "2 months 2 days ago"));

        /* Weeks and days */
        x = now(CLOCK_REALTIME) - (1*USEC_PER_WEEK + 1*USEC_PER_DAY);
        assert_se(format_timestamp_relative(buf, sizeof(buf), x));
        log_debug("%s", buf);
        assert_se(streq(buf, "1 week 1 day ago"));

        x = now(CLOCK_REALTIME) - (1*USEC_PER_WEEK + 2*USEC_PER_DAY);
        assert_se(format_timestamp_relative(buf, sizeof(buf), x));
        log_debug("%s", buf);
        assert_se(streq(buf, "1 week 2 days ago"));

        x = now(CLOCK_REALTIME) - (2*USEC_PER_WEEK + 1*USEC_PER_DAY);
        assert_se(format_timestamp_relative(buf, sizeof(buf), x));
        log_debug("%s", buf);
        assert_se(streq(buf, "2 weeks 1 day ago"));

        x = now(CLOCK_REALTIME) - (2*USEC_PER_WEEK + 2*USEC_PER_DAY);
        assert_se(format_timestamp_relative(buf, sizeof(buf), x));
        log_debug("%s", buf);
        assert_se(streq(buf, "2 weeks 2 days ago"));
}

static void test_format_timestamp_utc_one(usec_t val, const char *result) {
        char buf[FORMAT_TIMESTAMP_MAX];
        const char *t;

        t = format_timestamp_style(buf, sizeof(buf), val, TIMESTAMP_UTC);
        assert_se(streq_ptr(t, result));
}

TEST(format_timestamp_utc) {
        test_format_timestamp_utc_one(0, NULL);
        test_format_timestamp_utc_one(1, "Thu 1970-01-01 00:00:00 UTC");
        test_format_timestamp_utc_one(USEC_PER_SEC, "Thu 1970-01-01 00:00:01 UTC");

#if SIZEOF_TIME_T == 8
        test_format_timestamp_utc_one(USEC_TIMESTAMP_FORMATTABLE_MAX, "Thu 9999-12-30 23:59:59 UTC");
        test_format_timestamp_utc_one(USEC_TIMESTAMP_FORMATTABLE_MAX + 1, "--- XXXX-XX-XX XX:XX:XX");
#elif SIZEOF_TIME_T == 4
        test_format_timestamp_utc_one(USEC_TIMESTAMP_FORMATTABLE_MAX, "Tue 2038-01-19 03:14:07 UTC");
        test_format_timestamp_utc_one(USEC_TIMESTAMP_FORMATTABLE_MAX + 1, "--- XXXX-XX-XX XX:XX:XX");
#endif

        test_format_timestamp_utc_one(USEC_INFINITY, NULL);
}

TEST(deserialize_dual_timestamp) {
        int r;
        dual_timestamp t;

        r = deserialize_dual_timestamp("1234 5678", &t);
        assert_se(r == 0);
        assert_se(t.realtime == 1234);
        assert_se(t.monotonic == 5678);

        r = deserialize_dual_timestamp("1234x 5678", &t);
        assert_se(r == -EINVAL);

        r = deserialize_dual_timestamp("1234 5678y", &t);
        assert_se(r == -EINVAL);

        r = deserialize_dual_timestamp("-1234 5678", &t);
        assert_se(r == -EINVAL);

        r = deserialize_dual_timestamp("1234 -5678", &t);
        assert_se(r == -EINVAL);

        /* Check that output wasn't modified. */
        assert_se(t.realtime == 1234);
        assert_se(t.monotonic == 5678);

        r = deserialize_dual_timestamp("+123 567", &t);
        assert_se(r == 0);
        assert_se(t.realtime == 123);
        assert_se(t.monotonic == 567);

        /* Check that we get "infinity" on overflow. */
        r = deserialize_dual_timestamp("18446744073709551617 0", &t);
        assert_se(r == 0);
        assert_se(t.realtime == USEC_INFINITY);
        assert_se(t.monotonic == 0);
}

static void assert_similar(usec_t a, usec_t b) {
        usec_t d;

        if (a > b)
                d = a - b;
        else
                d = b - a;

        assert_se(d < 10*USEC_PER_SEC);
}

TEST(usec_shift_clock) {
        usec_t rt, mn, bt;

        rt = now(CLOCK_REALTIME);
        mn = now(CLOCK_MONOTONIC);
        bt = now(CLOCK_BOOTTIME);

        assert_se(usec_shift_clock(USEC_INFINITY, CLOCK_REALTIME, CLOCK_MONOTONIC) == USEC_INFINITY);

        assert_similar(usec_shift_clock(rt + USEC_PER_HOUR, CLOCK_REALTIME, CLOCK_MONOTONIC), mn + USEC_PER_HOUR);
        assert_similar(usec_shift_clock(rt + 2*USEC_PER_HOUR, CLOCK_REALTIME, CLOCK_BOOTTIME), bt + 2*USEC_PER_HOUR);
        assert_se(usec_shift_clock(rt + 3*USEC_PER_HOUR, CLOCK_REALTIME, CLOCK_REALTIME_ALARM) == rt + 3*USEC_PER_HOUR);

        assert_similar(usec_shift_clock(mn + 4*USEC_PER_HOUR, CLOCK_MONOTONIC, CLOCK_REALTIME_ALARM), rt + 4*USEC_PER_HOUR);
        assert_similar(usec_shift_clock(mn + 5*USEC_PER_HOUR, CLOCK_MONOTONIC, CLOCK_BOOTTIME), bt + 5*USEC_PER_HOUR);
        assert_se(usec_shift_clock(mn + 6*USEC_PER_HOUR, CLOCK_MONOTONIC, CLOCK_MONOTONIC) == mn + 6*USEC_PER_HOUR);

        assert_similar(usec_shift_clock(bt + 7*USEC_PER_HOUR, CLOCK_BOOTTIME, CLOCK_MONOTONIC), mn + 7*USEC_PER_HOUR);
        assert_similar(usec_shift_clock(bt + 8*USEC_PER_HOUR, CLOCK_BOOTTIME, CLOCK_REALTIME_ALARM), rt + 8*USEC_PER_HOUR);
        assert_se(usec_shift_clock(bt + 9*USEC_PER_HOUR, CLOCK_BOOTTIME, CLOCK_BOOTTIME) == bt + 9*USEC_PER_HOUR);

        if (mn > USEC_PER_MINUTE) {
                assert_similar(usec_shift_clock(rt - 30 * USEC_PER_SEC, CLOCK_REALTIME_ALARM, CLOCK_MONOTONIC), mn - 30 * USEC_PER_SEC);
                assert_similar(usec_shift_clock(rt - 50 * USEC_PER_SEC, CLOCK_REALTIME, CLOCK_BOOTTIME), bt - 50 * USEC_PER_SEC);
        }
}

TEST(in_utc_timezone) {
        const char *tz = getenv("TZ");

        assert_se(setenv("TZ", ":UTC", 1) >= 0);
        assert_se(in_utc_timezone());
        assert_se(streq(tzname[0], "UTC"));
        assert_se(streq(tzname[1], "UTC"));
        assert_se(timezone == 0);
        assert_se(daylight == 0);

        assert_se(setenv("TZ", ":Europe/Berlin", 1) >= 0);
        assert_se(!in_utc_timezone());
        assert_se(streq(tzname[0], "CET"));
        assert_se(streq(tzname[1], "CEST"));

        assert_se(set_unset_env("TZ", tz, true) == 0);
        tzset();
}

TEST(map_clock_usec) {
        usec_t nowr, x, y, z;

        x = nowr = now(CLOCK_REALTIME); /* right now */
        y = map_clock_usec(x, CLOCK_REALTIME, CLOCK_MONOTONIC);
        z = map_clock_usec(y, CLOCK_MONOTONIC, CLOCK_REALTIME);
        /* Converting forth and back will introduce inaccuracies, since we cannot query both clocks atomically, but it should be small. Even on the slowest CI smaller than 1h */

        assert_se((z > x ? z - x : x - z) < USEC_PER_HOUR);

        assert_se(nowr < USEC_INFINITY - USEC_PER_DAY*7); /* overflow check */
        x = nowr + USEC_PER_DAY*7; /* 1 week from now */
        y = map_clock_usec(x, CLOCK_REALTIME, CLOCK_MONOTONIC);
        assert_se(y > 0 && y < USEC_INFINITY);
        z = map_clock_usec(y, CLOCK_MONOTONIC, CLOCK_REALTIME);
        assert_se(z > 0 && z < USEC_INFINITY);
        assert_se((z > x ? z - x : x - z) < USEC_PER_HOUR);

        assert_se(nowr > USEC_PER_DAY * 7); /* underflow check */
        x = nowr - USEC_PER_DAY*7; /* 1 week ago */
        y = map_clock_usec(x, CLOCK_REALTIME, CLOCK_MONOTONIC);
        if (y != 0) { /* might underflow if machine is not up long enough for the monotonic clock to be beyond 1w */
                assert_se(y < USEC_INFINITY);
                z = map_clock_usec(y, CLOCK_MONOTONIC, CLOCK_REALTIME);
                assert_se(z > 0 && z < USEC_INFINITY);
                assert_se((z > x ? z - x : x - z) < USEC_PER_HOUR);
        }
}

static int intro(void) {
        log_info("realtime=" USEC_FMT "\n"
                 "monotonic=" USEC_FMT "\n"
                 "boottime=" USEC_FMT "\n",
                 now(CLOCK_REALTIME),
                 now(CLOCK_MONOTONIC),
                 now(CLOCK_BOOTTIME));

        /* Ensure time_t is signed */
        assert_cc((time_t) -1 < (time_t) 1);

        /* Ensure TIME_T_MAX works correctly */
        uintmax_t x = TIME_T_MAX;
        x++;
        assert_se((time_t) x < 0);

        return EXIT_SUCCESS;
}

DEFINE_TEST_MAIN_WITH_INTRO(LOG_INFO, intro);
