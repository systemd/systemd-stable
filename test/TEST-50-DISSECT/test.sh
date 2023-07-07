#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# -*- mode: shell-script; indent-tabs-mode: nil; sh-basic-offset: 4; -*-
# ex: ts=8 sw=4 sts=4 et filetype=sh
set -e

TEST_DESCRIPTION="test systemd-dissect"
IMAGE_NAME="dissect"
TEST_NO_NSPAWN=1
TEST_INSTALL_VERITY_MINIMAL=1

# shellcheck source=test/test-functions
. "${TEST_BASE_DIR:?}/test-functions"

test_require_bin mksquashfs veritysetup sfdisk

# Need loop devices for systemd-dissect
test_append_files() {
    (
        instmods loop =block
        instmods squashfs =squashfs
        instmods dm_verity =md
        instmods overlay =overlayfs
        install_dmevent
        generate_module_dependencies
        inst_binary wc
        if command -v openssl >/dev/null 2>&1; then
            inst_binary openssl
        fi
        install_verity_minimal
    )
}

do_test "$@"
