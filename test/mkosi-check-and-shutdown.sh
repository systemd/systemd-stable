#!/bin/bash -eux
# SPDX-License-Identifier: LGPL-2.1-or-later

systemctl --failed --no-legend | tee /failed-services

# Check that secure boot keys were properly enrolled.
# TODO: re-enable once secureboot can be enabled on nested kvm on hyperv without crashing qemu
# if ! systemd-detect-virt --container; then
    # cmp /sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c <(printf '\6\0\0\0\1')
    # cmp /sys/firmware/efi/efivars/SetupMode-8be4df61-93ca-11d2-aa0d-00e098032b8c <(printf '\6\0\0\0\0')
# fi

# Exit with non-zero EC if the /failed-services file is not empty (we have -e set)
[[ ! -s /failed-services ]]

: >/testok
