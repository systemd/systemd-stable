#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# -*- mode: shell-script; indent-tabs-mode: nil; sh-basic-offset: 4; -*-
# ex: ts=8 sw=4 sts=4 et filetype=sh
set -eux
set -o pipefail

ARGS=()
state_directory=/var/lib/private/
if [[ -v ASAN_OPTIONS || -v UBSAN_OPTIONS ]]; then
    # If we're running under sanitizers, we need to use a less restrictive
    # profile, otherwise LSan syscall would get blocked by seccomp
    ARGS+=(--profile=trusted)
    # With the trusted profile DynamicUser is disabled, so the storage is not in private/
    state_directory=/var/lib/
fi
# Bump the timeout if we're running with plain QEMU
[[ "$(systemd-detect-virt -v)" == "qemu" ]] && TIMEOUT=90 || TIMEOUT=30

systemd-dissect --no-pager /usr/share/minimal_0.raw | grep -q '✓ portable service'
systemd-dissect --no-pager /usr/share/minimal_1.raw | grep -q '✓ portable service'
systemd-dissect --no-pager /usr/share/app0.raw | grep -q '✓ extension for portable service'
systemd-dissect --no-pager /usr/share/app1.raw | grep -q '✓ extension for portable service'

export SYSTEMD_LOG_LEVEL=debug
mkdir -p /run/systemd/system/systemd-portabled.service.d/
cat <<EOF >/run/systemd/system/systemd-portabled.service.d/override.conf
[Service]
Environment=SYSTEMD_LOG_LEVEL=debug
EOF

portablectl "${ARGS[@]}" attach --now --runtime /usr/share/minimal_0.raw minimal-app0

systemctl is-active minimal-app0.service
systemctl is-active minimal-app0-foo.service
systemctl is-active minimal-app0-bar.service && exit 1

portablectl "${ARGS[@]}" reattach --now --runtime /usr/share/minimal_1.raw minimal-app0

systemctl is-active minimal-app0.service
systemctl is-active minimal-app0-bar.service
systemctl is-active minimal-app0-foo.service && exit 1

portablectl list | grep -q -F "minimal_1"
busctl tree org.freedesktop.portable1 --no-pager | grep -q -F '/org/freedesktop/portable1/image/minimal_5f1'

portablectl detach --now --runtime /usr/share/minimal_1.raw minimal-app0

portablectl list | grep -q -F "No images."
busctl tree org.freedesktop.portable1 --no-pager | grep -q -F '/org/freedesktop/portable1/image/minimal_5f1' && exit 1

# portablectl also works with directory paths rather than images

unsquashfs -dest /tmp/minimal_0 /usr/share/minimal_0.raw
unsquashfs -dest /tmp/minimal_1 /usr/share/minimal_1.raw

portablectl "${ARGS[@]}" attach --copy=symlink --now --runtime /tmp/minimal_0 minimal-app0

systemctl is-active minimal-app0.service
systemctl is-active minimal-app0-foo.service
systemctl is-active minimal-app0-bar.service && exit 1

portablectl "${ARGS[@]}" reattach --now --enable --runtime /tmp/minimal_1 minimal-app0

systemctl is-active minimal-app0.service
systemctl is-active minimal-app0-bar.service
systemctl is-active minimal-app0-foo.service && exit 1

portablectl list | grep -q -F "minimal_1"
busctl tree org.freedesktop.portable1 --no-pager | grep -q -F '/org/freedesktop/portable1/image/minimal_5f1'

portablectl detach --now --enable --runtime /tmp/minimal_1 minimal-app0

portablectl list | grep -q -F "No images."
busctl tree org.freedesktop.portable1 --no-pager | grep -q -F '/org/freedesktop/portable1/image/minimal_5f1' && exit 1

portablectl "${ARGS[@]}" attach --now --runtime --extension /usr/share/app0.raw /usr/share/minimal_0.raw app0

systemctl is-active app0.service
status="$(portablectl is-attached --extension app0 minimal_0)"
[[ "${status}" == "running-runtime" ]]

portablectl "${ARGS[@]}" reattach --now --runtime --extension /usr/share/app0.raw /usr/share/minimal_1.raw app0

systemctl is-active app0.service
status="$(portablectl is-attached --extension app0 minimal_1)"
[[ "${status}" == "running-runtime" ]]

portablectl detach --now --runtime --extension /usr/share/app0.raw /usr/share/minimal_1.raw app0

portablectl "${ARGS[@]}" attach --now --runtime --extension /usr/share/app1.raw /usr/share/minimal_0.raw app1

systemctl is-active app1.service
status="$(portablectl is-attached --extension app1 minimal_0)"
[[ "${status}" == "running-runtime" ]]

# Ensure that adding or removing a version to the image doesn't break reattaching
cp /usr/share/app1.raw /tmp/app1_2.raw
portablectl "${ARGS[@]}" reattach --now --runtime --extension /tmp/app1_2.raw /usr/share/minimal_1.raw app1

systemctl is-active app1.service
status="$(portablectl is-attached --extension app1_2 minimal_1)"
[[ "${status}" == "running-runtime" ]]

portablectl "${ARGS[@]}" reattach --now --runtime --extension /usr/share/app1.raw /usr/share/minimal_1.raw app1

systemctl is-active app1.service
status="$(portablectl is-attached --extension app1 minimal_1)"
[[ "${status}" == "running-runtime" ]]

portablectl detach --now --runtime --extension /usr/share/app1.raw /usr/share/minimal_1.raw app1

# Ensure that the combination of read-only images, state directory and dynamic user works, and that
# state is retained. Check after detaching, as on slow systems (eg: sanitizers) it might take a while
# after the service is attached before the file appears.
grep -q -F bar "${state_directory}/app0/foo"
grep -q -F baz "${state_directory}/app1/foo"

# portablectl also works with directory paths rather than images

mkdir /tmp/rootdir /tmp/app0 /tmp/app1 /tmp/overlay /tmp/os-release-fix /tmp/os-release-fix/etc
mount /usr/share/app0.raw /tmp/app0
mount /usr/share/app1.raw /tmp/app1
mount /usr/share/minimal_0.raw /tmp/rootdir

# Fix up os-release to drop the valid PORTABLE_SERVICES field (because we are
# bypassing the sysext logic in portabled here it will otherwise not see the
# extensions additional valid prefix)
grep -v "^PORTABLE_PREFIXES=" /tmp/rootdir/etc/os-release > /tmp/os-release-fix/etc/os-release

mount -t overlay overlay -o lowerdir=/tmp/os-release-fix:/tmp/app1:/tmp/rootdir /tmp/overlay

grep . /tmp/overlay/usr/lib/extension-release.d/*
grep . /tmp/overlay/etc/os-release

portablectl "${ARGS[@]}" attach --copy=symlink --now --runtime /tmp/overlay app1

systemctl is-active app1.service

portablectl detach --now --runtime overlay app1

umount /tmp/overlay

portablectl "${ARGS[@]}" attach --copy=symlink --now --runtime --extension /tmp/app0 --extension /tmp/app1 /tmp/rootdir app0 app1

systemctl is-active app0.service
systemctl is-active app1.service

portablectl inspect --cat --extension app0 --extension app1 rootdir app0 app1 | grep -q -f /tmp/rootdir/usr/lib/os-release
portablectl inspect --cat --extension app0 --extension app1 rootdir app0 app1 | grep -q -f /tmp/app0/usr/lib/extension-release.d/extension-release.app0
portablectl inspect --cat --extension app0 --extension app1 rootdir app0 app1 | grep -q -f /tmp/app1/usr/lib/extension-release.d/extension-release.app2
portablectl inspect --cat --extension app0 --extension app1 rootdir app0 app1 | grep -q -f /tmp/app1/usr/lib/systemd/system/app1.service
portablectl inspect --cat --extension app0 --extension app1 rootdir app0 app1 | grep -q -f /tmp/app0/usr/lib/systemd/system/app0.service

portablectl detach --now --runtime --extension /tmp/app0 --extension /tmp/app1 /tmp/rootdir app0 app1

umount /tmp/rootdir
umount /tmp/app0
umount /tmp/app1

# Lack of ID field in os-release should be rejected, but it caused a crash in the past instead
mkdir -p /tmp/emptyroot/usr/lib
mkdir -p /tmp/emptyext/usr/lib/extension-release.d
touch /tmp/emptyroot/usr/lib/os-release
touch /tmp/emptyext/usr/lib/extension-release.d/extension-release.emptyext

# Remote peer disconnected -> portabled crashed
res="$(! portablectl attach --extension /tmp/emptyext /tmp/emptyroot 2> >(grep "Remote peer disconnected"))"
test -z "${res}"

echo OK >/testok

exit 0
