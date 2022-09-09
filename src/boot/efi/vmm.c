/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <efi.h>
#include <efilib.h>
#include <stdbool.h>

#include "drivers.h"
#include "efi-string.h"
#include "string-util-fundamental.h"
#include "util.h"

#define QEMU_KERNEL_LOADER_FS_MEDIA_GUID                                \
        { 0x1428f772, 0xb64a, 0x441e, {0xb8, 0xc3, 0x9e, 0xbd, 0xd7, 0xf8, 0x93, 0xc7 }}

#define VMM_BOOT_ORDER_GUID \
        { 0x668f4529, 0x63d0, 0x4bb5, {0xb6, 0x5d, 0x6f, 0xbb, 0x9d, 0x36, 0xa4, 0x4a }}

/* detect direct boot */
bool is_direct_boot(EFI_HANDLE device) {
        EFI_STATUS err;
        VENDOR_DEVICE_PATH *dp;

        err = BS->HandleProtocol(device, &DevicePathProtocol, (void **) &dp);
        if (err != EFI_SUCCESS)
                return false;

        /* 'qemu -kernel systemd-bootx64.efi' */
        if (dp->Header.Type == MEDIA_DEVICE_PATH &&
            dp->Header.SubType == MEDIA_VENDOR_DP &&
            memcmp(&dp->Guid, &(EFI_GUID)QEMU_KERNEL_LOADER_FS_MEDIA_GUID, sizeof(EFI_GUID)) == 0)
                return true;

        /* loaded from firmware volume (sd-boot added to ovmf) */
        if (dp->Header.Type == MEDIA_DEVICE_PATH &&
            dp->Header.SubType == MEDIA_PIWG_FW_VOL_DP)
                return true;

        return false;
}
