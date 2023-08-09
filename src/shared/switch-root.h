/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stdbool.h>

typedef enum SwitchRootFlags {
        SWITCH_ROOT_DESTROY_OLD_ROOT      = 1 << 0, /* rm -rf old root when switching – under the condition
                                                     * that it is backed by non-persistent tmpfs/ramfs/… */
        SWITCH_ROOT_DONT_SYNC             = 1 << 1, /* don't call sync() immediately before switching root */
        SWITCH_ROOT_SKIP_RECURSIVE_RUN    = 1 << 2, /* move /run without MS_REC */
        SWITCH_ROOT_SKIP_RECURSIVE_UMOUNT = 1 << 3, /* do not umount recursively on move */
} SwitchRootFlags;

int switch_root(const char *new_root, const char *old_root_after, SwitchRootFlags flags);
