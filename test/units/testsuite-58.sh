#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
set -eux
set -o pipefail

if ! command -v systemd-repart >/dev/null; then
    echo "no systemd-repart" >/skipped
    exit 0
fi

# shellcheck source=test/units/assert.sh
. "$(dirname "$0")"/assert.sh

export SYSTEMD_LOG_LEVEL=debug
export PAGER=cat

seed=750b6cd5c4ae4012a15e7be3c29e6a47

if ! systemd-detect-virt --quiet --container; then
    udevadm control --log-level debug
fi

machine="$(uname -m)"
if [ "${machine}" = "x86_64" ]; then
    root_guid=4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709
    root_uuid=60F33797-1D71-4DCB-AA6F-20564F036CD0
    root_uuid2=73A4CCD2-EAF5-44DA-A366-F99188210FDC
    usr_guid=8484680C-9521-48C6-9C11-B0720656F69E
    usr_uuid=7E3369DD-D653-4513-ADF5-B993A9F20C16
    architecture="x86-64"
elif [ "${machine}" = "i386" ] || [ "${machine}" = "i686" ] || [ "${machine}" = "x86" ]; then
    root_guid=44479540-F297-41B2-9AF7-D131D5F0458A
    root_uuid=02B4253F-29A4-404E-8972-1669D3B03C87
    root_uuid2=268E0FD3-B468-4806-A823-E533FE9BB9CC
    usr_guid=75250D76-8CC6-458E-BD66-BD47CC81A812
    usr_uuid=7B42FFB0-B0E1-4395-B20B-C78F4A571648
    architecture="x86"
elif [ "${machine}" = "aarch64" ] || [ "${machine}" = "aarch64_be" ] || [ "${machine}" = "armv8b" ] || [ "${machine}" = "armv8l" ]; then
    root_guid=B921B045-1DF0-41C3-AF44-4C6F280D3FAE
    root_uuid=055D0227-53A6-4033-85C3-9A5973EFF483
    root_uuid2=F7DBBE48-8FD0-4833-8411-AA34E7C8E60A
    usr_guid=B0E01050-EE5F-4390-949A-9101B17104E9
    usr_uuid=FCE3C75E-D6A4-44C0-87F0-4C105183FB1F
    architecture="arm64"
elif [ "${machine}" = "arm" ]; then
    root_guid=69DAD710-2CE4-4E3C-B16C-21A1D49ABED3
    root_uuid=567DA89E-8DE2-4499-8D10-18F212DFF034
    root_uuid2=813ECFE5-4C89-4193-8A52-437493F2F96E
    usr_guid=7D0359A3-02B3-4F0A-865C-654403E70625
    usr_uuid=71E93DC2-5073-42CB-8A84-A354E64D8966
    architecture="arm"
elif [ "${machine}" = "loongarch64" ]; then
    root_guid=77055800-792C-4F94-B39A-98C91B762BB6
    root_uuid=D8EFC2D2-0133-41E4-BDCB-3B9F4CFDDDE8
    root_uuid2=36499F9E-0688-40C1-A746-EA8FD9543C56
    usr_guid=E611C702-575C-4CBE-9A46-434FA0BF7E3F
    usr_uuid=031FFA75-00BB-49B6-A70D-911D2D82A5B7
    architecture="loongarch64"
elif [ "${machine}" = "ia64" ]; then
    root_guid=993D8D3D-F80E-4225-855A-9DAF8ED7EA97
    root_uuid=DCF33449-0896-4EA9-BC24-7D58AEEF522D
    root_uuid2=C2A6CAB7-ABEA-4FBA-8C48-CB4C52E6CA38
    usr_guid=4301D2A6-4E3B-4B2A-BB94-9E0B2C4225EA
    usr_uuid=BC2BCCE7-80D6-449A-85CC-637424CE5241
    architecture="ia64"
elif [ "${machine}" = "s390x" ]; then
    root_guid=5EEAD9A9-FE09-4A1E-A1D7-520D00531306
    root_uuid=7EBE0C85-E27E-48EC-B164-F4807606232E
    root_uuid2=2A074E1C-2A19-4094-A0C2-24B1A5D52FCB
    usr_guid=8A4F5770-50AA-4ED3-874A-99B710DB6FEA
    usr_uuid=51171D30-35CF-4A49-B8B5-9478B9B796A5
    architecture="s390x"
elif [ "${machine}" = "ppc64le" ]; then
    root_guid=C31C45E6-3F39-412E-80FB-4809C4980599
    root_uuid=061E67A1-092F-482F-8150-B525D50D6654
    root_uuid2=A6687CEF-4E4F-44E7-90B3-CDA52EA81739
    usr_guid=15BB03AF-77E7-4D4A-B12B-C0D084F7491C
    usr_uuid=C0D0823B-8040-4C7C-A629-026248E297FB
    architecture="ppc64-le"
else
    echo "Unexpected uname -m: ${machine} in testsuite-58.sh, please fix me"
    exit 1
fi

test_basic() {
    local defs imgs output
    local loop volume

    defs="$(mktemp --directory "/tmp/test-repart.XXXXXXXXXX")"
    imgs="$(mktemp --directory "/var/tmp/test-repart.XXXXXXXXXX")"
    # shellcheck disable=SC2064
    trap "rm -rf '$defs' '$imgs'" RETURN

    # 1. create an empty image

    systemd-repart --empty=create \
                   --size=1G \
                   --seed="$seed" \
                   "$imgs/zzz"

    output=$(sfdisk -d "$imgs/zzz" | grep -v -e 'sector-size' -e '^$')

    assert_eq "$output" "label: gpt
label-id: 1D2CE291-7CCE-4F7D-BC83-FDB49AD74EBD
device: $imgs/zzz
unit: sectors
first-lba: 2048
last-lba: 2097118"

    # 2. Testing with root, root2, home, and swap

    cat >"$defs/root.conf" <<EOF
[Partition]
Type=root
EOF

    ln -s root.conf "$defs/root2.conf"

    cat >"$defs/home.conf" <<EOF
[Partition]
Type=home
Label=home-first
Label=home-always-too-long-xxxxxxxxxxxxxx-%v
EOF

    cat >"$defs/swap.conf" <<EOF
[Partition]
Type=swap
SizeMaxBytes=64M
PaddingMinBytes=92M
EOF

    systemd-repart --definitions="$defs" \
                   --dry-run=no \
                   --seed="$seed" \
                   "$imgs/zzz"

    output=$(sfdisk -d "$imgs/zzz" | grep -v -e 'sector-size' -e '^$')

    assert_eq "$output" "label: gpt
label-id: 1D2CE291-7CCE-4F7D-BC83-FDB49AD74EBD
device: $imgs/zzz
unit: sectors
first-lba: 2048
last-lba: 2097118
$imgs/zzz1 : start=        2048, size=      591856, type=933AC7E1-2EB4-4F13-B844-0E14E2AEF915, uuid=4980595D-D74A-483A-AA9E-9903879A0EE5, name=\"home-first\", attrs=\"GUID:59\"
$imgs/zzz2 : start=      593904, size=      591856, type=${root_guid}, uuid=${root_uuid}, name=\"root-${architecture}\", attrs=\"GUID:59\"
$imgs/zzz3 : start=     1185760, size=      591864, type=${root_guid}, uuid=${root_uuid2}, name=\"root-${architecture}-2\", attrs=\"GUID:59\"
$imgs/zzz4 : start=     1777624, size=      131072, type=0657FD6D-A4AB-43C4-84E5-0933C84B4F4F, uuid=78C92DB8-3D2B-4823-B0DC-792B78F66F1E, name=\"swap\""

    # 3. Testing with root, root2, home, swap, and another partition

    cat >"$defs/swap.conf" <<EOF
[Partition]
Type=swap
SizeMaxBytes=64M
EOF

    cat >"$defs/extra.conf" <<EOF
[Partition]
Type=linux-generic
Label=custom_label
UUID=a0a1a2a3a4a5a6a7a8a9aaabacadaeaf
EOF

    echo "Label=ignored_label" >>"$defs/home.conf"
    echo "UUID=b0b1b2b3b4b5b6b7b8b9babbbcbdbebf" >>"$defs/home.conf"

    systemd-repart --definitions="$defs" \
                   --dry-run=no \
                   --seed="$seed" \
                   "$imgs/zzz"

    output=$(sfdisk -d "$imgs/zzz" | grep -v -e 'sector-size' -e '^$')

    assert_eq "$output" "label: gpt
label-id: 1D2CE291-7CCE-4F7D-BC83-FDB49AD74EBD
device: $imgs/zzz
unit: sectors
first-lba: 2048
last-lba: 2097118
$imgs/zzz1 : start=        2048, size=      591856, type=933AC7E1-2EB4-4F13-B844-0E14E2AEF915, uuid=4980595D-D74A-483A-AA9E-9903879A0EE5, name=\"home-first\", attrs=\"GUID:59\"
$imgs/zzz2 : start=      593904, size=      591856, type=${root_guid}, uuid=${root_uuid}, name=\"root-${architecture}\", attrs=\"GUID:59\"
$imgs/zzz3 : start=     1185760, size=      591864, type=${root_guid}, uuid=${root_uuid2}, name=\"root-${architecture}-2\", attrs=\"GUID:59\"
$imgs/zzz4 : start=     1777624, size=      131072, type=0657FD6D-A4AB-43C4-84E5-0933C84B4F4F, uuid=78C92DB8-3D2B-4823-B0DC-792B78F66F1E, name=\"swap\"
$imgs/zzz5 : start=     1908696, size=      188416, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, uuid=A0A1A2A3-A4A5-A6A7-A8A9-AAABACADAEAF, name=\"custom_label\""

    # 4. Resizing to 2G

    systemd-repart --definitions="$defs" \
                   --size=2G \
                   --dry-run=no \
                   --seed="$seed" \
                   "$imgs/zzz"

    output=$(sfdisk -d "$imgs/zzz" | grep -v -e 'sector-size' -e '^$')

    assert_eq "$output" "label: gpt
label-id: 1D2CE291-7CCE-4F7D-BC83-FDB49AD74EBD
device: $imgs/zzz
unit: sectors
first-lba: 2048
last-lba: 4194270
$imgs/zzz1 : start=        2048, size=      591856, type=933AC7E1-2EB4-4F13-B844-0E14E2AEF915, uuid=4980595D-D74A-483A-AA9E-9903879A0EE5, name=\"home-first\", attrs=\"GUID:59\"
$imgs/zzz2 : start=      593904, size=      591856, type=${root_guid}, uuid=${root_uuid}, name=\"root-${architecture}\", attrs=\"GUID:59\"
$imgs/zzz3 : start=     1185760, size=      591864, type=${root_guid}, uuid=${root_uuid2}, name=\"root-${architecture}-2\", attrs=\"GUID:59\"
$imgs/zzz4 : start=     1777624, size=      131072, type=0657FD6D-A4AB-43C4-84E5-0933C84B4F4F, uuid=78C92DB8-3D2B-4823-B0DC-792B78F66F1E, name=\"swap\"
$imgs/zzz5 : start=     1908696, size=     2285568, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, uuid=A0A1A2A3-A4A5-A6A7-A8A9-AAABACADAEAF, name=\"custom_label\""

    # 5. Testing with root, root2, home, swap, another partition, and partition copy

    dd if=/dev/urandom of="$imgs/block-copy" bs=4096 count=10240

    cat >"$defs/extra2.conf" <<EOF
[Partition]
Type=linux-generic
Label=block-copy
UUID=2a1d97e1d0a346cca26eadc643926617
CopyBlocks=$imgs/block-copy
EOF

    systemd-repart --definitions="$defs" \
                   --size=3G \
                   --dry-run=no \
                   --seed="$seed" \
                   "$imgs/zzz"

    output=$(sfdisk -d "$imgs/zzz" | grep -v -e 'sector-size' -e '^$')

    assert_eq "$output" "label: gpt
label-id: 1D2CE291-7CCE-4F7D-BC83-FDB49AD74EBD
device: $imgs/zzz
unit: sectors
first-lba: 2048
last-lba: 6291422
$imgs/zzz1 : start=        2048, size=      591856, type=933AC7E1-2EB4-4F13-B844-0E14E2AEF915, uuid=4980595D-D74A-483A-AA9E-9903879A0EE5, name=\"home-first\", attrs=\"GUID:59\"
$imgs/zzz2 : start=      593904, size=      591856, type=${root_guid}, uuid=${root_uuid}, name=\"root-${architecture}\", attrs=\"GUID:59\"
$imgs/zzz3 : start=     1185760, size=      591864, type=${root_guid}, uuid=${root_uuid2}, name=\"root-${architecture}-2\", attrs=\"GUID:59\"
$imgs/zzz4 : start=     1777624, size=      131072, type=0657FD6D-A4AB-43C4-84E5-0933C84B4F4F, uuid=78C92DB8-3D2B-4823-B0DC-792B78F66F1E, name=\"swap\"
$imgs/zzz5 : start=     1908696, size=     2285568, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, uuid=A0A1A2A3-A4A5-A6A7-A8A9-AAABACADAEAF, name=\"custom_label\"
$imgs/zzz6 : start=     4194264, size=     2097152, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, uuid=2A1D97E1-D0A3-46CC-A26E-ADC643926617, name=\"block-copy\""

    cmp --bytes=$((4096*10240)) --ignore-initial=0:$((512*4194264)) "$imgs/block-copy" "$imgs/zzz"

    if systemd-detect-virt --quiet --container; then
        echo "Skipping encrypt tests in container."
        return
    fi

    # 6. Testing Format=/Encrypt=/CopyFiles=

    cat >"$defs/extra3.conf" <<EOF
[Partition]
Type=linux-generic
Label=luks-format-copy
UUID=7b93d1f2-595d-4ce3-b0b9-837fbd9e63b0
Format=ext4
Encrypt=yes
CopyFiles=$defs:/def
SizeMinBytes=48M
EOF

    systemd-repart --definitions="$defs" \
                   --size=auto \
                   --dry-run=no \
                   --seed="$seed" \
                   "$imgs/zzz"

    output=$(sfdisk -d "$imgs/zzz" | grep -v -e 'sector-size' -e '^$')

    assert_eq "$output" "label: gpt
label-id: 1D2CE291-7CCE-4F7D-BC83-FDB49AD74EBD
device: $imgs/zzz
unit: sectors
first-lba: 2048
last-lba: 6389726
$imgs/zzz1 : start=        2048, size=      591856, type=933AC7E1-2EB4-4F13-B844-0E14E2AEF915, uuid=4980595D-D74A-483A-AA9E-9903879A0EE5, name=\"home-first\", attrs=\"GUID:59\"
$imgs/zzz2 : start=      593904, size=      591856, type=${root_guid}, uuid=${root_uuid}, name=\"root-${architecture}\", attrs=\"GUID:59\"
$imgs/zzz3 : start=     1185760, size=      591864, type=${root_guid}, uuid=${root_uuid2}, name=\"root-${architecture}-2\", attrs=\"GUID:59\"
$imgs/zzz4 : start=     1777624, size=      131072, type=0657FD6D-A4AB-43C4-84E5-0933C84B4F4F, uuid=78C92DB8-3D2B-4823-B0DC-792B78F66F1E, name=\"swap\"
$imgs/zzz5 : start=     1908696, size=     2285568, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, uuid=A0A1A2A3-A4A5-A6A7-A8A9-AAABACADAEAF, name=\"custom_label\"
$imgs/zzz6 : start=     4194264, size=     2097152, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, uuid=2A1D97E1-D0A3-46CC-A26E-ADC643926617, name=\"block-copy\"
$imgs/zzz7 : start=     6291416, size=       98304, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, uuid=7B93D1F2-595D-4CE3-B0B9-837FBD9E63B0, name=\"luks-format-copy\""

    loop="$(losetup -P --show --find "$imgs/zzz")"
    udevadm wait --timeout 60 --settle "${loop:?}"

    volume="test-repart-$RANDOM"

    touch "$imgs/empty-password"
    cryptsetup open --type=luks2 --key-file="$imgs/empty-password" "${loop}p7" "$volume"
    mkdir -p "$imgs/mount"
    mount -t ext4 "/dev/mapper/$volume" "$imgs/mount"
    # Use deferred closing on the mapper and autoclear on the loop, so they are cleaned up on umount
    cryptsetup close --deferred "$volume"
    losetup -d "$loop"
    diff -r "$imgs/mount/def" "$defs" >/dev/null
    umount "$imgs/mount"
}

test_dropin() {
    local defs imgs output

    defs="$(mktemp --directory "/tmp/test-repart.XXXXXXXXXX")"
    imgs="$(mktemp --directory "/var/tmp/test-repart.XXXXXXXXXX")"
    # shellcheck disable=SC2064
    trap "rm -rf '$defs' '$imgs'" RETURN

    cat >"$defs/root.conf" <<EOF
[Partition]
Type=swap
SizeMaxBytes=64M
UUID=837c3d67-21b3-478e-be82-7e7f83bf96d3
EOF

    mkdir -p "$defs/root.conf.d"
    cat >"$defs/root.conf.d/override1.conf" <<EOF
[Partition]
Label=label1
SizeMaxBytes=32M
EOF

    cat >"$defs/root.conf.d/override2.conf" <<EOF
[Partition]
Label=label2
EOF

    output=$(systemd-repart --definitions="$defs" --empty=create --size=100M --json=pretty "$imgs/zzz")

    diff <(echo "$output") - <<EOF
[
	{
		"type" : "swap",
		"label" : "label2",
		"uuid" : "837c3d67-21b3-478e-be82-7e7f83bf96d3",
		"file" : "root.conf",
		"node" : "$imgs/zzz1",
		"offset" : 1048576,
		"old_size" : 0,
		"raw_size" : 33554432,
		"size" : "→ 32.0M",
		"old_padding" : 0,
		"raw_padding" : 0,
		"padding" : "→ 0B",
		"activity" : "create",
		"drop-in_files" : [
			"$defs/root.conf.d/override1.conf",
			"$defs/root.conf.d/override2.conf"
		]
	}
]
EOF
}

test_multiple_definitions() {
    local defs imgs output

    defs="$(mktemp --directory "/tmp/test-repart.XXXXXXXXXX")"
    imgs="$(mktemp --directory "/var/tmp/test-repart.XXXXXXXXXX")"
    # shellcheck disable=SC2064
    trap "rm -rf '$defs' '$imgs'" RETURN

    mkdir -p "$defs/1"

    cat >"$defs/1/root1.conf" <<EOF
[Partition]
Type=swap
SizeMaxBytes=32M
UUID=7b93d1f2-595d-4ce3-b0b9-837fbd9e63b0
Label=label1
EOF

    mkdir -p "$defs/2"

    cat >"$defs/2/root2.conf" <<EOF
[Partition]
Type=swap
SizeMaxBytes=32M
UUID=837c3d67-21b3-478e-be82-7e7f83bf96d3
Label=label2
EOF

    output=$(systemd-repart --definitions="$defs/1" --definitions="$defs/2" --empty=create --size=100M --json=pretty "$imgs/zzz")

    diff <(echo "$output") - <<EOF
[
	{
		"type" : "swap",
		"label" : "label1",
		"uuid" : "7b93d1f2-595d-4ce3-b0b9-837fbd9e63b0",
		"file" : "root1.conf",
		"node" : "$imgs/zzz1",
		"offset" : 1048576,
		"old_size" : 0,
		"raw_size" : 33554432,
		"size" : "→ 32.0M",
		"old_padding" : 0,
		"raw_padding" : 0,
		"padding" : "→ 0B",
		"activity" : "create"
	},
	{
		"type" : "swap",
		"label" : "label2",
		"uuid" : "837c3d67-21b3-478e-be82-7e7f83bf96d3",
		"file" : "root2.conf",
		"node" : "$imgs/zzz2",
		"offset" : 34603008,
		"old_size" : 0,
		"raw_size" : 33554432,
		"size" : "→ 32.0M",
		"old_padding" : 0,
		"raw_padding" : 0,
		"padding" : "→ 0B",
		"activity" : "create"
	}
]
EOF
}

test_copy_blocks() {
    local defs imgs output

    if systemd-detect-virt --quiet --container; then
        echo "Skipping copy blocks tests in container."
        return
    fi

    defs="$(mktemp --directory "/tmp/test-repart.XXXXXXXXXX")"
    imgs="$(mktemp --directory "/var/tmp/test-repart.XXXXXXXXXX")"
    # shellcheck disable=SC2064
    trap "rm -rf '$defs' '$imgs'" RETURN

    # First, create a disk image and verify its in order

    cat >"$defs/esp.conf" <<EOF
[Partition]
Type=esp
SizeMinBytes=10M
Format=vfat
EOF

    cat >"$defs/usr.conf" <<EOF
[Partition]
Type=usr-${architecture}
SizeMinBytes=10M
Format=ext4
ReadOnly=yes
EOF

    cat >"$defs/root.conf" <<EOF
[Partition]
Type=root-${architecture}
SizeMinBytes=10M
Format=ext4
MakeDirectories=/usr /efi
EOF

    systemd-repart --definitions="$defs" \
                   --empty=create \
                   --size=auto \
                   --seed="$seed" \
                   "$imgs/zzz"

    output=$(sfdisk --dump "$imgs/zzz")

    assert_in "$imgs/zzz1 : start=        2048, size=       20480, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, uuid=39107B09-615D-48FB-BA37-C663885FCE67, name=\"esp\"" "$output"
    assert_in "$imgs/zzz2 : start=       22528, size=       20480, type=${root_guid}, uuid=${root_uuid}, name=\"root-${architecture}\", attrs=\"GUID:59\"" "$output"
    assert_in "$imgs/zzz3 : start=       43008, size=       20480, type=${usr_guid}, uuid=${usr_uuid}, name=\"usr-${architecture}\", attrs=\"GUID:60\"" "$output"

    # Then, create another image with CopyBlocks=auto

    cat >"$defs/esp.conf" <<EOF
[Partition]
Type=esp
CopyBlocks=auto
EOF

    cat >"$defs/usr.conf" <<EOF
[Partition]
Type=usr-${architecture}
ReadOnly=yes
CopyBlocks=auto
EOF

    cat >"$defs/root.conf" <<EOF
[Partition]
Type=root-${architecture}
CopyBlocks=auto
EOF

    systemd-repart --definitions="$defs" \
                   --empty=create \
                   --size=auto \
                   --seed="$seed" \
                   --image="$imgs/zzz" \
                   "$imgs/yyy"

    cmp "$imgs/zzz" "$imgs/yyy"
}

test_unaligned_partition() {
    local defs imgs output

    defs="$(mktemp --directory "/tmp/test-repart.XXXXXXXXXX")"
    imgs="$(mktemp --directory "/var/tmp/test-repart.XXXXXXXXXX")"
    # shellcheck disable=SC2064
    trap "rm -rf '$defs' '$imgs'" RETURN

    # Operate on an image with unaligned partition.

    cat >"$defs/root.conf" <<EOF
[Partition]
Type=root-${architecture}
EOF

    truncate -s 10g "$imgs/unaligned"
    sfdisk "$imgs/unaligned" <<EOF
label: gpt

start=2048, size=69044
start=71092, size=3591848
EOF

    systemd-repart --definitions="$defs" \
                   --seed="$seed" \
                   --dry-run=no \
                   "$imgs/unaligned"

    output=$(sfdisk --dump "$imgs/unaligned")

    assert_in "$imgs/unaligned1 : start=        2048, size=       69044," "$output"
    assert_in "$imgs/unaligned2 : start=       71092, size=     3591848," "$output"
    assert_in "$imgs/unaligned3 : start=     3662944, size=    17308536, type=${root_guid}, uuid=${root_uuid}, name=\"root-${architecture}\", attrs=\"GUID:59\"" "$output"
}

test_issue_21817() {
    local defs imgs output

    # testcase for #21817

    defs="$(mktemp --directory "/tmp/test-repart.XXXXXXXXXX")"
    imgs="$(mktemp --directory "/var/tmp/test-repart.XXXXXXXXXX")"
    # shellcheck disable=SC2064
    trap "rm -rf '$defs' '$imgs'" RETURN

    cat >"$defs/test.conf" <<EOF
[Partition]
Type=root
EOF

    truncate -s 100m "$imgs/21817.img"
    sfdisk "$imgs/21817.img" <<EOF
label: gpt

size=50M, type=${root_guid}
,
EOF

    systemd-repart --pretty=yes \
                   --definitions "$imgs" \
                   --seed="$seed" \
                   --dry-run=no \
                   "$imgs/21817.img"

    output=$(sfdisk --dump "$imgs/21817.img")

    assert_in "$imgs/21817.img1 : start=        2048, size=      102400, type=${root_guid}," "$output"
    # Accept both unpadded (pre-v2.38 util-linux) and padded (v2.38+ util-linux) sizes
    assert_in "$imgs/21817.img2 : start=      104448, size=      (100319| 98304)," "$output"
}

test_issue_24553() {
    local defs imgs output

    # testcase for #24553

    defs="$(mktemp --directory "/tmp/test-repart.XXXXXXXXXX")"
    imgs="$(mktemp --directory "/var/tmp/test-repart.XXXXXXXXXX")"
    # shellcheck disable=SC2064
    trap "rm -rf '$defs' '$imgs'" RETURN

    cat >"$defs/root.conf" <<EOF
[Partition]
Type=root
SizeMinBytes=10G
SizeMaxBytes=120G
EOF

    cat >"$imgs/partscript" <<EOF
label: gpt
label-id: C9FFE979-A415-C449-B729-78C7AA664B10
unit: sectors
first-lba: 40

start=40, size=524288, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, uuid=F2E89C8A-DC5D-4C4C-A29C-6CFB643B74FD, name="ESP System Partition"
start=524328, size=14848000, type=${root_guid}, uuid=${root_uuid}, name="root-${architecture}"
EOF

    # 1. Operate on a small image compared with SizeMinBytes=.
    truncate -s 8g "$imgs/zzz"
    sfdisk "$imgs/zzz" <"$imgs/partscript"

    # This should fail, but not trigger assertions.
    assert_rc 1 systemd-repart --definitions="$defs" \
                               --seed="$seed" \
                               --dry-run=no \
                               "$imgs/zzz"

    output=$(sfdisk --dump "$imgs/zzz")
    assert_in "$imgs/zzz2 : start=      524328, size=    14848000, type=${root_guid}, uuid=${root_uuid}, name=\"root-${architecture}\"" "$output"

    # 2. Operate on an larger image compared with SizeMinBytes=.
    rm -f "$imgs/zzz"
    truncate -s 12g "$imgs/zzz"
    sfdisk "$imgs/zzz" <"$imgs/partscript"

    # This should succeed.
    systemd-repart --definitions="$defs" \
                   --seed="$seed" \
                   --dry-run=no \
                   "$imgs/zzz"

    output=$(sfdisk --dump "$imgs/zzz")
    assert_in "$imgs/zzz2 : start=      524328, size=    24641456, type=${root_guid}, uuid=${root_uuid}, name=\"root-${architecture}\"" "$output"

    # 3. Multiple partitions with Priority= (small disk)
    cat >"$defs/root.conf" <<EOF
[Partition]
Type=root
SizeMinBytes=10G
SizeMaxBytes=120G
Priority=100
EOF

    cat >"$defs/usr.conf" <<EOF
[Partition]
Type=usr
SizeMinBytes=10M
Priority=10
EOF

    rm -f "$imgs/zzz"
    truncate -s 8g "$imgs/zzz"
    sfdisk "$imgs/zzz" <"$imgs/partscript"

    # This should also succeed, but root is not extended.
    systemd-repart --definitions="$defs" \
                   --seed="$seed" \
                   --dry-run=no \
                   "$imgs/zzz"

    output=$(sfdisk --dump "$imgs/zzz")
    assert_in "$imgs/zzz2 : start=      524328, size=    14848000, type=${root_guid}, uuid=${root_uuid}, name=\"root-${architecture}\"" "$output"
    assert_in "$imgs/zzz3 : start=    15372328, size=     1404848, type=${usr_guid}, uuid=${usr_uuid}, name=\"usr-${architecture}\", attrs=\"GUID:59\"" "$output"

    # 4. Multiple partitions with Priority= (large disk)
    rm -f "$imgs/zzz"
    truncate -s 12g "$imgs/zzz"
    sfdisk "$imgs/zzz" <"$imgs/partscript"

    # This should also succeed, and root is extended.
    systemd-repart --definitions="$defs" \
                   --seed="$seed" \
                   --dry-run=no \
                   "$imgs/zzz"

    output=$(sfdisk --dump "$imgs/zzz")
    assert_in "$imgs/zzz2 : start=      524328, size=    20971520, type=${root_guid}, uuid=${root_uuid}, name=\"root-${architecture}\"" "$output"
    assert_in "$imgs/zzz3 : start=    21495848, size=     3669936, type=${usr_guid}, uuid=${usr_uuid}, name=\"usr-${architecture}\", attrs=\"GUID:59\"" "$output"
}

test_zero_uuid() {
    local defs imgs output

    defs="$(mktemp --directory "/tmp/test-repart.XXXXXXXXXX")"
    imgs="$(mktemp --directory "/var/tmp/test-repart.XXXXXXXXXX")"
    # shellcheck disable=SC2064
    trap "rm -rf '$defs' '$imgs'" RETURN

    # Test image with zero UUID.

    cat >"$defs/root.conf" <<EOF
[Partition]
Type=root-${architecture}
UUID=null
EOF

    systemd-repart --definitions="$defs" \
                   --seed="$seed" \
                   --dry-run=no \
                   --empty=create \
                   --size=auto \
                   "$imgs/zero"

    output=$(sfdisk --dump "$imgs/zero")

    assert_in "$imgs/zero1 : start=        2048, size=       20480, type=${root_guid}, uuid=00000000-0000-0000-0000-000000000000" "$output"
}

test_verity() {
    local defs imgs output

    if systemd-detect-virt --quiet --container; then
        echo "Skipping verity test in container."
        return
    fi

    defs="$(mktemp --directory "/tmp/test-repart.XXXXXXXXXX")"
    imgs="$(mktemp --directory "/var/tmp/test-repart.XXXXXXXXXX")"
    # shellcheck disable=SC2064
    trap "rm -rf '$defs' '$imgs'" RETURN

    cat >"$defs/verity-data.conf" <<EOF
[Partition]
Type=root-${architecture}
CopyFiles=${defs}
Verity=data
VerityMatchKey=root
EOF

    cat >"$defs/verity-hash.conf" <<EOF
[Partition]
Type=root-${architecture}-verity
Verity=hash
VerityMatchKey=root
EOF

    cat >"$defs/verity-sig.conf" <<EOF
[Partition]
Type=root-${architecture}-verity-sig
Verity=signature
VerityMatchKey=root
EOF

    # Unfortunately OpenSSL insists on reading some config file, hence provide one with mostly placeholder contents
    cat >> "$defs/verity.openssl.cnf" <<EOF
[ req ]
prompt = no
distinguished_name = req_distinguished_name

[ req_distinguished_name ]
C = DE
ST = Test State
L = Test Locality
O = Org Name
OU = Org Unit Name
CN = Common Name
emailAddress = test@email.com
EOF

    openssl req -config "$defs/verity.openssl.cnf" -new -x509 -newkey rsa:1024 -keyout "$defs/verity.key" -out "$defs/verity.crt" -days 365 -nodes

    mkdir -p /run/verity.d
    ln -s "$defs/verity.crt" /run/verity.d/ok.crt

    output=$(systemd-repart --definitions="$defs" \
                            --seed="$seed" \
                            --dry-run=no \
                            --empty=create \
                            --size=auto \
                            --json=pretty \
                            --private-key="$defs/verity.key" \
                            --certificate="$defs/verity.crt" \
                            "$imgs/verity")

    roothash=$(jq -r ".[] | select(.type == \"root-${architecture}-verity\") | .roothash" <<< "$output")

    # Check that we can dissect, mount and unmount a repart verity image.

    systemd-dissect "$imgs/verity" --root-hash "$roothash"
    systemd-dissect "$imgs/verity" --root-hash "$roothash" -M "$imgs/mnt"
    systemd-dissect -U "$imgs/mnt"
}

testcase_free_area_calculation() {
    local defs imgs output

    if ! command -v mksquashfs >/dev/null; then
        echo "Skipping free area calculation test without squashfs."
        return
    fi

    defs="$(mktemp --directory "/tmp/test-repart.defs.XXXXXXXXXX")"
    imgs="$(mktemp --directory "/var/tmp/test-repart.imgs.XXXXXXXXXX")"
    # shellcheck disable=SC2064
    trap "rm -rf '$defs' '$imgs'" RETURN
    chmod a+rx "$defs"

    # https://github.com/systemd/systemd/issues/28225
    echo "*** free area calculation ***"

    tee "$defs/00-ESP.conf" <<EOF
[Partition]
Type         = esp
Label        = ESP
Format       = vfat

SizeMinBytes = 128M
SizeMaxBytes = 128M

# Sufficient for testing
CopyFiles    = /etc:/
EOF

    tee "$defs/10-os.conf" <<EOF
[Partition]
Type           = root-${architecture}
Label          = test
Format         = squashfs

Minimize       = best
# Sufficient for testing
CopyFiles      = /etc/:/

VerityMatchKey = os
Verity         = data
EOF

    tee "$defs/11-os-verity.conf" <<EOF
[Partition]
Type           = root-${architecture}-verity
Label          = test

Minimize       = best

VerityMatchKey = os
Verity         = hash
EOF

    # Set sector size for VFAT to 512 bytes because there will not be enough FAT clusters otherwise
    output1=$(SYSTEMD_REPART_MKFS_OPTIONS_VFAT="-S 512" systemd-repart \
                                              --definitions="$defs" \
                                              --seed="$seed" \
                                              --dry-run=no \
                                              --empty=create \
                                              --size=auto \
                                              --sector-size=4096 \
                                              --defer-partitions=esp \
                                              --json=pretty \
                                              "$imgs/zzz")

    # The second invocation
    output2=$(SYSTEMD_REPART_MKFS_OPTIONS_VFAT="-S 512" systemd-repart \
                                              --definitions="$defs" \
                                              --seed="$seed" \
                                              --dry-run=no \
                                              --empty=allow \
                                              --size=auto \
                                              --sector-size=4096 \
                                              --defer-partitions=esp \
                                              --json=pretty \
                                              "$imgs/zzz")

    diff -u <(echo "$output1" | grep -E "(offset|raw_size|raw_padding)") <(echo "$output2" | grep -E "(offset|raw_size|raw_padding)")
}

test_sector() {
    local defs imgs output loop
    local start size ratio
    local sector="${1?}"

    if systemd-detect-virt --quiet --container; then
        echo "Skipping sector size tests in container."
        return
    fi

    defs="$(mktemp --directory "/tmp/test-repart.XXXXXXXXXX")"
    imgs="$(mktemp --directory "/var/tmp/test-repart.XXXXXXXXXX")"
    # shellcheck disable=SC2064
    trap "rm -rf '$defs' '$imgs'" RETURN

    cat > "$defs/a.conf" <<EOF
[Partition]
Type=root
SizeMaxBytes=15M
SizeMinBytes=15M
EOF
    cat > "$defs/b.conf" <<EOF
[Partition]
Type=linux-generic
Weight=250
EOF

    cat > "$defs/c.conf" <<EOF
[Partition]
Type=linux-generic
Weight=750
EOF

    truncate -s 100m "$imgs/$sector.img"
    loop=$(losetup -b "$sector" -P --show -f "$imgs/$sector.img" )
    udevadm wait --timeout 60 --settle "${loop:?}"
    systemd-repart --pretty=yes \
                   --definitions="$defs" \
                   --seed="$seed" \
                   --empty=require \
                   --dry-run=no \
                   "$loop"

    sfdisk --verify "$loop"
    output=$(sfdisk --dump "$loop")
    losetup -d "$loop"

    ratio=$(( sector / 512 ))
    start=$(( 2048 / ratio ))
    size=$(( 30720 / ratio ))
    assert_in "${loop}p1 : start= *${start}, size= *${size}, type=${root_guid}, uuid=${root_uuid}, name=\"root-${architecture}\", attrs=\"GUID:59\"" "$output"
    start=$(( start + size ))
    size=$(( 42992 / ratio ))
    assert_in "${loop}p2 : start= *${start}, size= *${size}, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, uuid=DF71F5E3-080A-4D16-824B-18591B881380, name=\"linux-generic\"" "$output"
    start=$(( start + size ))
    size=$(( 129000 / ratio ))
    assert_in "${loop}p3 : start= *${start}, size= *${size}, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, uuid=DB081670-07AE-48CA-9F5E-813D5E40B976, name=\"linux-generic-2\"" "$output"
}

test_basic
test_dropin
test_multiple_definitions
test_copy_blocks
test_unaligned_partition
test_issue_21817
test_issue_24553
test_zero_uuid
test_verity

# Valid block sizes on the Linux block layer are >= 512 and <= PAGE_SIZE, and
# must be powers of 2. Which leaves exactly four different ones to test on
# typical hardware
test_sector 512
test_sector 1024
test_sector 2048
test_sector 4096

echo OK >/testok

exit 0
