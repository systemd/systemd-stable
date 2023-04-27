/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "alloc-util.h"
#include "coredump-util.h"
#include "macro.h"
#include "tests.h"

TEST(coredump_filter_to_from_string) {
        for (CoredumpFilter i = 0; i < _COREDUMP_FILTER_MAX; i++) {
                const char *n;

                assert_se(n = coredump_filter_to_string(i));
                log_info("0x%x\t%s", 1u << i, n);
                assert_se(coredump_filter_from_string(n) == i);

                uint64_t f;
                assert_se(coredump_filter_mask_from_string(n, &f) == 0);
                assert_se(f == 1u << i);
        }
}

TEST(coredump_filter_mask_from_string) {
        uint64_t f;
        assert_se(coredump_filter_mask_from_string("default", &f) == 0);
        assert_se(f == COREDUMP_FILTER_MASK_DEFAULT);
        assert_se(coredump_filter_mask_from_string("all", &f) == 0);
        assert_se(f == COREDUMP_FILTER_MASK_ALL);

        assert_se(coredump_filter_mask_from_string("  default\tdefault\tdefault  ", &f) == 0);
        assert_se(f == COREDUMP_FILTER_MASK_DEFAULT);

        assert_se(coredump_filter_mask_from_string("defaulta", &f) < 0);
        assert_se(coredump_filter_mask_from_string("default defaulta default", &f) < 0);
        assert_se(coredump_filter_mask_from_string("default default defaulta", &f) < 0);

        assert_se(coredump_filter_mask_from_string("private-anonymous default", &f) == 0);
        assert_se(f == COREDUMP_FILTER_MASK_DEFAULT);

        assert_se(coredump_filter_mask_from_string("shared-file-backed shared-dax", &f) == 0);
        assert_se(f == (1 << COREDUMP_FILTER_SHARED_FILE_BACKED |
                        1 << COREDUMP_FILTER_SHARED_DAX));

        assert_se(coredump_filter_mask_from_string("private-file-backed private-dax 0xF", &f) == 0);
        assert_se(f == (1 << COREDUMP_FILTER_PRIVATE_FILE_BACKED |
                        1 << COREDUMP_FILTER_PRIVATE_DAX |
                        0xF));

        assert_se(coredump_filter_mask_from_string("11", &f) == 0);
        assert_se(f == 0x11);

        assert_se(coredump_filter_mask_from_string("0x1101", &f) == 0);
        assert_se(f == 0x1101);

        assert_se(coredump_filter_mask_from_string("0", &f) == 0);
        assert_se(f == 0);

        assert_se(coredump_filter_mask_from_string("all", &f) == 0);
        assert_se(FLAGS_SET(f, (1 << COREDUMP_FILTER_PRIVATE_ANONYMOUS |
                                1 << COREDUMP_FILTER_SHARED_ANONYMOUS |
                                1 << COREDUMP_FILTER_PRIVATE_FILE_BACKED |
                                1 << COREDUMP_FILTER_SHARED_FILE_BACKED |
                                1 << COREDUMP_FILTER_ELF_HEADERS |
                                1 << COREDUMP_FILTER_PRIVATE_HUGE |
                                1 << COREDUMP_FILTER_SHARED_HUGE |
                                1 << COREDUMP_FILTER_PRIVATE_DAX |
                                1 << COREDUMP_FILTER_SHARED_DAX)));
}

DEFINE_TEST_MAIN(LOG_INFO);
