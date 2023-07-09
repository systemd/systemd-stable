/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <sched.h>
#include <stdio.h>
#include <string.h>

#include "errno-util.h"
#include "log.h"
#include "loopback-setup.h"
#include "tests.h"

TEST_RET(loopback_setup) {
        int r;

        if (unshare(CLONE_NEWUSER | CLONE_NEWNET) < 0) {
                if (ERRNO_IS_PRIVILEGE(errno) || ERRNO_IS_NOT_SUPPORTED(errno)) {
                        log_notice("Skipping test, lacking privileges or namespaces not supported");
                        return EXIT_TEST_SKIP;
                }
                return log_error_errno(errno, "Failed to create user+network namespace: %m");
        }

        r = loopback_setup();
        if (r < 0)
                return log_error_errno(r, "loopback: %m");

        log_info("> ipv6 main");
        system("ip -6 route show table main");
        log_info("> ipv6 local");
        system("ip -6 route show table local");
        log_info("> ipv4 main");
        system("ip -4 route show table main");
        log_info("> ipv4 local");
        system("ip -4 route show table local");

        return EXIT_SUCCESS;
}

static int intro(void) {
        log_show_color(true);
        return EXIT_SUCCESS;
}

DEFINE_TEST_MAIN_WITH_INTRO(LOG_INFO, intro);
