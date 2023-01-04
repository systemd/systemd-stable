/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <linux/loop.h>

#ifndef LOOP_CONFIGURE
struct loop_config {
        __u32 fd;
        __u32 block_size;
        struct loop_info64 info;
        __u64 __reserved[8];
};

#define LOOP_CONFIGURE 0x4C0A
#endif

#ifndef LO_FLAGS_DIRECT_IO
#define LO_FLAGS_DIRECT_IO 16
#define LOOP_SET_DIRECT_IO 0x4C08
#endif

#ifndef LOOP_SET_STATUS_SETTABLE_FLAGS
#define LOOP_SET_STATUS_SETTABLE_FLAGS (LO_FLAGS_AUTOCLEAR | LO_FLAGS_PARTSCAN | LO_FLAGS_DIRECT_IO)
#endif
