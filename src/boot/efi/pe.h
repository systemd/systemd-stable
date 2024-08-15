/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "efi.h"

EFI_STATUS pe_memory_locate_sections(
                const void *base,
                const char * const sections[],
                size_t *addrs,
                size_t *sizes);

EFI_STATUS pe_file_locate_sections(
                EFI_FILE *dir,
                const char16_t *path,
                const char * const sections[],
                size_t *offsets,
                size_t *sizes);

EFI_STATUS pe_kernel_info(const void *base, uint32_t *ret_compat_address, size_t *ret_size_in_memory);
