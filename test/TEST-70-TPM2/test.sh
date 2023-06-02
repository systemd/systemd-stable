#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
set -e

TEST_DESCRIPTION="cryptenroll/cryptsetup with TPM2 devices"
IMAGE_NAME="tpm2"
TEST_NO_NSPAWN=1
TEST_SETUP_SWTPM=1
TEST_REQUIRE_INSTALL_TESTS=0

# shellcheck source=test/test-functions
. "${TEST_BASE_DIR:?}/test-functions"

command -v swtpm >/dev/null 2>&1 || exit 0
command -v tpm2_pcrextend >/dev/null 2>&1 || exit 0

test_append_files() {
        local workspace="${1:?}"

        instmods tpm tpm_tis tpm_ibmvtpm
        install_dmevent
        generate_module_dependencies
        inst_binary tpm2_pcrextend
        inst_binary openssl
}

do_test "$@"
