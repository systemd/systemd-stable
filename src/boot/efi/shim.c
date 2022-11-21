/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Port to systemd-boot
 * Copyright © 2017 Max Resch <resch.max@gmail.com>
 *
 * Security Policy Handling
 * Copyright © 2012 <James.Bottomley@HansenPartnership.com>
 * https://github.com/mjg59/efitools
 */

#include <efi.h>
#include <efilib.h>

#include "missing_efi.h"
#include "util.h"
#include "secure-boot.h"
#include "shim.h"

#if defined(__x86_64__) || defined(__i386__)
#define __sysv_abi__ __attribute__((sysv_abi))
#else
#define __sysv_abi__
#endif

struct ShimLock {
        EFI_STATUS __sysv_abi__ (*shim_verify) (const void *buffer, uint32_t size);

        /* context is actually a struct for the PE header, but it isn't needed so void is sufficient just do define the interface
         * see shim.c/shim.h and PeHeader.h in the github shim repo */
        EFI_STATUS __sysv_abi__ (*generate_hash) (void *data, uint32_t datasize, void *context, uint8_t *sha256hash, uint8_t *sha1hash);

        EFI_STATUS __sysv_abi__ (*read_header) (void *data, uint32_t datasize, void *context);
};

#define SHIM_LOCK_GUID \
        &(const EFI_GUID) { 0x605dab50, 0xe046, 0x4300, { 0xab, 0xb6, 0x3d, 0xd8, 0x10, 0xdd, 0x8b, 0x23 } }

bool shim_loaded(void) {
        struct ShimLock *shim_lock;

        return BS->LocateProtocol((EFI_GUID*) SHIM_LOCK_GUID, NULL, (void**) &shim_lock) == EFI_SUCCESS;
}

static bool shim_validate(
                const void *ctx, const EFI_DEVICE_PATH *device_path, const void *file_buffer, size_t file_size) {

        EFI_STATUS err;
        _cleanup_free_ char *file_buffer_owned = NULL;

        if (!file_buffer) {
                if (!device_path)
                        return false;

                EFI_HANDLE device_handle;
                EFI_DEVICE_PATH *file_dp = (EFI_DEVICE_PATH *) device_path;
                err = BS->LocateDevicePath(&FileSystemProtocol, &file_dp, &device_handle);
                if (err != EFI_SUCCESS)
                        return false;

                _cleanup_(file_closep) EFI_FILE *root = NULL;
                err = open_volume(device_handle, &root);
                if (err != EFI_SUCCESS)
                        return false;

                _cleanup_free_ char16_t *dp_str = NULL;
                err = device_path_to_str(file_dp, &dp_str);
                if (err != EFI_SUCCESS)
                        return false;

                err = file_read(root, dp_str, 0, 0, &file_buffer_owned, &file_size);
                if (err != EFI_SUCCESS)
                        return false;

                file_buffer = file_buffer_owned;
        }

        struct ShimLock *shim_lock;
        err = BS->LocateProtocol((EFI_GUID *) SHIM_LOCK_GUID, NULL, (void **) &shim_lock);
        if (err != EFI_SUCCESS)
                return false;

        return shim_lock->shim_verify(file_buffer, file_size) == EFI_SUCCESS;
}

EFI_STATUS shim_load_image(EFI_HANDLE parent, const EFI_DEVICE_PATH *device_path, EFI_HANDLE *ret_image) {
        assert(device_path);
        assert(ret_image);

        bool have_shim = shim_loaded();

        if (have_shim)
                install_security_override(shim_validate, NULL);

        EFI_STATUS ret = BS->LoadImage(
                        /*BootPolicy=*/false, parent, (EFI_DEVICE_PATH *) device_path, NULL, 0, ret_image);

        if (have_shim)
                uninstall_security_override();

        return ret;
}
