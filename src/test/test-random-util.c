/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <math.h>

#include "hexdecoct.h"
#include "log.h"
#include "memory-util.h"
#include "random-util.h"
#include "terminal-util.h"
#include "tests.h"

static void test_genuine_random_bytes_one(RandomFlags flags) {
        uint8_t buf[16] = {};

        log_info("/* %s(%d) */", __func__, flags);

        for (size_t i = 1; i < sizeof buf; i++) {
                assert_se(genuine_random_bytes(buf, i, flags) == 0);
                if (i + 1 < sizeof buf)
                        assert_se(buf[i] == 0);

                hexdump(stdout, buf, i);
        }
}

TEST(genuine_random_bytes) {
        test_genuine_random_bytes_one(0);
        test_genuine_random_bytes_one(RANDOM_BLOCK);
        test_genuine_random_bytes_one(RANDOM_ALLOW_RDRAND);
}

TEST(pseudo_random_bytes) {
        uint8_t buf[16] = {};

        for (size_t i = 1; i < sizeof buf; i++) {
                pseudo_random_bytes(buf, i);
                if (i + 1 < sizeof buf)
                        assert_se(buf[i] == 0);

                hexdump(stdout, buf, i);
        }
}

TEST(rdrand) {
        int r;

        for (unsigned i = 0; i < 10; i++) {
                unsigned long x = 0;

                r = rdrand(&x);
                if (r < 0) {
                        log_error_errno(r, "RDRAND failed: %m");
                        return;
                }

                printf("%lx\n", x);
        }
}

#define TOTAL 100000

static void test_random_u64_range_one(unsigned mod) {
        log_info("/* %s(%u) */", __func__, mod);

        unsigned max = 0, count[mod];
        zero(count);

        for (unsigned i = 0; i < TOTAL; i++) {
                uint64_t x;

                x = random_u64_range(mod);

                count[x]++;
                max = MAX(max, count[x]);
        }

        /* Print histogram: vertical axis — value, horizontal axis — count.
         *
         * The expected value is always TOTAL/mod, because the distribution should be flat. The expected
         * variance is TOTAL×p×(1-p), where p==1/mod, and standard deviation the root of the variance.
         * Assert that the deviation from the expected value is less than 6 standard deviations.
         */
        unsigned scale = 2 * max / (columns() < 20 ? 80 : columns() - 20);
        double exp = (double) TOTAL / mod;

        for (size_t i = 0; i < mod; i++) {
                double dev = (count[i] - exp) / sqrt(exp * (mod > 1 ? mod - 1 : 1) / mod);
                log_debug("%02zu: %5u (%+.3f)%*s",
                          i, count[i], dev,
                          count[i] / scale, "x");

                assert_se(fabs(dev) < 6); /* 6 sigma is excessive, but this check should be enough to
                                           * identify catastrophic failure while minimizing false
                                           * positives. */
        }
}

TEST(random_u64_range) {
        for (unsigned mod = 1; mod < 29; mod++)
                test_random_u64_range_one(mod);
}

DEFINE_TEST_MAIN(LOG_DEBUG);
