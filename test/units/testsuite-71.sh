#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later

set -eux
set -o pipefail

# shellcheck source=test/units/assert.sh
. "$(dirname "$0")"/assert.sh

restore_hostname() {
    if [[ -e /tmp/hostname.bak ]]; then
        mv /tmp/hostname.bak /etc/hostname
    else
        rm -f /etc/hostname
    fi
}

test_hostname() {
    local orig=

    if [[ -f /etc/hostname ]]; then
        cp /etc/hostname /tmp/hostname.bak
        orig=$(cat /etc/hostname)
    fi

    trap restore_hostname RETURN

    # should activate daemon and work
    if [[ -n "$orig" ]]; then
        assert_in "Static hostname: $orig" "$(hostnamectl)"
    fi
    assert_in "Kernel: $(uname -s) $(uname -r)" "$(hostnamectl)"

    # change hostname
    assert_rc 0 hostnamectl set-hostname testhost
    assert_eq "$(cat /etc/hostname)" "testhost"
    assert_in "Static hostname: testhost" "$(hostnamectl)"

    if [[ -n "$orig" ]]; then
        # reset to original
        assert_rc 0 hostnamectl set-hostname "$orig"
        assert_eq "$(cat /etc/hostname)" "$orig"
        assert_in "Static hostname: $orig" "$(hostnamectl)"
    fi
}

restore_machine_info() {
    if [[ -e /tmp/machine-info.bak ]]; then
        mv /tmp/machine-info.bak /etc/machine-info
    else
        rm -f /etc/machine-info
    fi
}

get_chassis() (
    # shellcheck source=/dev/null
    . /etc/machine-info

    echo "$CHASSIS"
)

test_chassis() {
    local i

    if [[ -f /etc/machine-info ]]; then
        cp /etc/machine-info /tmp/machine-info.bak
    fi

    trap restore_machine_info RETURN

    # Invalid chassis type is refused
    assert_rc 1 hostnamectl chassis hoge

    # Valid chassis types
    for i in vm container desktop laptop convertible server tablet handset watch embedded; do
        hostnamectl chassis "$i"
        assert_eq "$(hostnamectl chassis)" "$i"
        assert_eq "$(get_chassis)" "$i"
    done

    systemctl stop systemd-hostnamed.service
    rm -f /etc/machine-info

    # fallback chassis type
    if systemd-detect-virt --quiet --container; then
        assert_eq "$(hostnamectl chassis)" container
    elif systemd-detect-virt --quiet --vm; then
        assert_eq "$(hostnamectl chassis)" vm
    fi
}

restore_sysfs_dmi() {
    umount /sys/class/dmi/id
    rm -rf /run/systemd/system/systemd-hostnamed.service.d
    systemctl daemon-reload
    systemctl stop systemd-hostnamed
}

test_firmware_date() {
    # No DMI on s390x or ppc
    if [[ ! -d /sys/class/dmi/id ]]; then
        echo "/sys/class/dmi/id not found, skipping firmware date tests."
        return 0
    fi

    trap restore_sysfs_dmi RETURN

    # Ignore /sys being mounted as tmpfs
    mkdir -p /run/systemd/system/systemd-hostnamed.service.d/
    cat >/run/systemd/system/systemd-hostnamed.service.d/override.conf <<EOF
[Service]
Environment="SYSTEMD_DEVICE_VERIFY_SYSFS=0"
Environment="SYSTEMD_HOSTNAME_FORCE_DMI=1"
EOF
    systemctl daemon-reload

    mount -t tmpfs none /sys/class/dmi/id
    echo '1' >/sys/class/dmi/id/uevent

    echo '09/08/2000' >/sys/class/dmi/id/bios_date
    systemctl stop systemd-hostnamed
    assert_in '2000-09-08' "$(hostnamectl)"

    echo '2022' >/sys/class/dmi/id/bios_date
    systemctl stop systemd-hostnamed
    assert_not_in 'Firmware Date' "$(hostnamectl)"

    echo 'garbage' >/sys/class/dmi/id/bios_date
    systemctl stop systemd-hostnamed
    assert_not_in 'Firmware Date' "$(hostnamectl)"
}

: >/failed

test_hostname
test_chassis
test_firmware_date

touch /testok
rm /failed
