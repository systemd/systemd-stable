#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
set -e

TEST_DESCRIPTION="Tests for auxiliary utilities"

# shellcheck source=test/test-functions
. "${TEST_BASE_DIR:?}/test-functions"

# (Hopefully) a temporary workaround for https://github.com/systemd/systemd/issues/30573
KERNEL_APPEND="${KERNEL_APPEND:-} SYSTEMD_DEFAULT_MOUNT_RATE_LIMIT_BURST=100"

do_test "$@"
