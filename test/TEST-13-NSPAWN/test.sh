#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
set -e

TEST_DESCRIPTION="systemd-nspawn tests"
IMAGE_NAME="nspawn"
TEST_NO_NSPAWN=1

# shellcheck source=test/test-functions
. "${TEST_BASE_DIR:?}/test-functions"

test_append_files() {
    local workspace="${1:?}"
    local container="$workspace/testsuite-13-container-template"

    # Create a dummy container "template" with a minimal toolset, which we can
    # then use as a base for our nspawn/machinectl tests
    initdir="$container" setup_basic_dirs
    initdir="$container" image_install \
        bash \
        env \
        cat \
        hostname \
        grep \
        ip \
        ls \
        md5sum \
        mountpoint \
        nc \
        ps \
        seq \
        sleep \
        stat \
        touch \
        true

    cp /etc/os-release "$container/usr/lib/os-release"
    cat >"$container/sbin/init" <<EOF
#!/bin/bash
echo "Hello from dummy init, beautiful day, innit?"
ip link
EOF
    chmod +x "$container/sbin/init"
}

do_test "$@"
