/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#include "alloc-util.h"
#include "architecture.h"
#include "base-filesystem.h"
#include "errno-util.h"
#include "fd-util.h"
#include "log.h"
#include "macro.h"
#include "nulstr-util.h"
#include "path-util.h"
#include "string-util.h"
#include "umask-util.h"
#include "user-util.h"

typedef struct BaseFilesystem {
        const char *dir;      /* directory or symlink to create */
        mode_t mode;
        const char *target;   /* if non-NULL create as symlink to this target */
        const char *exists;   /* conditionalize this entry on existence of this file */
        bool ignore_failure;
} BaseFilesystem;

static const BaseFilesystem table[] = {
        { "bin",      0, "usr/bin\0",                  NULL },
        { "lib",      0, "usr/lib\0",                  NULL },
        { "root",  0750, NULL,                         NULL, true },
        { "sbin",     0, "usr/sbin\0",                 NULL },
        { "usr",   0755, NULL,                         NULL },
        { "var",   0755, NULL,                         NULL },
        { "etc",   0755, NULL,                         NULL },
        { "proc",  0555, NULL,                         NULL, true },
        { "sys",   0555, NULL,                         NULL, true },
        { "dev",   0555, NULL,                         NULL, true },
        { "run",   0555, NULL,                         NULL, true },
        /* We don't add /tmp/ here for now (even though it's necessary for regular operation), because we
         * want to support both cases where /tmp/ is a mount of its own (in which case we probably should set
         * the mode to 1555, to indicate that no one should write to it, not even root) and when it's part of
         * the rootfs (in which case we should set mode 1777), and we simply don't know what's right. */

        /* Various architecture ABIs define the path to the dynamic loader via the /lib64/ subdirectory of
         * the root directory. When booting from an otherwise empty root file system (where only /usr/ has
         * been mounted into) it is thus necessary to create a symlink pointing to the right subdirectory of
         * /usr/ first — otherwise we couldn't invoke any dynamic binary. Let's detect this case here, and
         * create the symlink as needed should it be missing. We prefer doing this consistently with Debian's
         * multiarch logic, but support Fedora-style and Arch-style multilib too. */
#if defined(__aarch64__)
        /* aarch64 ELF ABI actually says dynamic loader is in /lib/, but Fedora puts it in /lib64/ anyway and
         * just symlinks /lib/ld-linux-aarch64.so.1 to ../lib64/ld-linux-aarch64.so.1. For this to work
         * correctly, /lib64/ must be symlinked to /usr/lib64/. */
        { "lib64",    0, "usr/lib/"LIB_ARCH_TUPLE"\0"
                         "usr/lib64\0"
                         "usr/lib\0",                "ld-linux-aarch64.so.1" },
#  define KNOW_LIB64_DIRS 1
#elif defined(__alpha__)
#elif defined(__arc__) || defined(__tilegx__)
#elif defined(__arm__)
        /* No /lib64 on arm. The linker is /lib/ld-linux-armhf.so.3. */
#  define KNOW_LIB64_DIRS 1
#elif defined(__i386__) || defined(__x86_64__)
        { "lib64",    0, "usr/lib/"LIB_ARCH_TUPLE"\0"
                         "usr/lib64\0"
                         "usr/lib\0",                "ld-linux-x86-64.so.2" },
#  define KNOW_LIB64_DIRS 1
#elif defined(__ia64__)
#elif defined(__loongarch_lp64)
#  define KNOW_LIB64_DIRS 1
#  if defined(__loongarch_double_float)
        { "lib64",    0, "usr/lib/"LIB_ARCH_TUPLE"\0"
                         "usr/lib64\0"
                         "usr/lib\0",                "ld-linux-loongarch-lp64d.so.1" },
#  elif defined(__loongarch_single_float)
        { "lib64",    0, "usr/lib/"LIB_ARCH_TUPLE"\0"
                         "usr/lib64\0"
                         "usr/lib\0",                "ld-linux-loongarch-lp64f.so.1" },
#  elif defined(__loongarch_soft_float)
        { "lib64",    0, "usr/lib/"LIB_ARCH_TUPLE"\0"
                         "usr/lib64\0"
                         "usr/lib\0",                "ld-linux-loongarch-lp64s.so.1" },
#  else
#    error "Unknown LoongArch ABI"
#  endif
#elif defined(__m68k__)
        /* No link needed. */
#  define KNOW_LIB64_DIRS 1
#elif defined(_MIPS_SIM)
#  if _MIPS_SIM == _MIPS_SIM_ABI32
#  elif _MIPS_SIM == _MIPS_SIM_NABI32
#  elif _MIPS_SIM == _MIPS_SIM_ABI64
#  else
#    error "Unknown MIPS ABI"
#  endif
#elif defined(__powerpc__)
#  if defined(__PPC64__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        { "lib64",    0, "usr/lib/"LIB_ARCH_TUPLE"\0"
                         "usr/lib64\0"
                         "usr/lib\0",                "ld64.so.2" },
#    define KNOW_LIB64_DIRS 1
#  elif defined(__powerpc64__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        /* powerpc64-linux-gnu */
#  else
        /* powerpc-linux-gnu */
#  endif
#elif defined(__riscv)
#  if __riscv_xlen == 32
#  elif __riscv_xlen == 64
        /* Same situation as for aarch64 */
        { "lib64",    0, "usr/lib/"LIB_ARCH_TUPLE"\0"
                         "usr/lib64\0"
                         "usr/lib\0",                "ld-linux-riscv64-lp64d.so.1" },
#    define KNOW_LIB64_DIRS 1
#  else
#    error "Unknown RISC-V ABI"
#  endif
#elif defined(__s390__)
        /* s390-linux-gnu */
#elif defined(__s390x__)
        { "lib64",    0, "usr/lib/"LIB_ARCH_TUPLE"\0"
                         "usr/lib64\0"
                         "usr/lib\0",                "ld-lsb-s390x.so.3" },
#    define KNOW_LIB64_DIRS 1
#elif defined(__sparc__)
#endif
        /* gcc doesn't allow pragma to be used within constructs, hence log about this separately below */
};

#ifndef KNOW_LIB64_DIRS
#  pragma message "Please add an entry above specifying whether your architecture uses /lib64/, /lib32/, or no such links."
#endif

int base_filesystem_create_fd(int fd, const char *root, uid_t uid, gid_t gid) {
        int r;

        assert(fd >= 0);
        assert(root);

        /* The "root" parameter is decoration only – it's only used as part of log messages */

        for (size_t i = 0; i < ELEMENTSOF(table); i++) {
                if (faccessat(fd, table[i].dir, F_OK, AT_SYMLINK_NOFOLLOW) >= 0)
                        continue;

                if (table[i].target) { /* Create as symlink? */
                        const char *target = NULL;

                        /* check if one of the targets exists */
                        NULSTR_FOREACH(s, table[i].target) {
                                if (faccessat(fd, s, F_OK, AT_SYMLINK_NOFOLLOW) < 0)
                                        continue;

                                /* check if a specific file exists at the target path */
                                if (table[i].exists) {
                                        _cleanup_free_ char *p = NULL;

                                        p = path_join(s, table[i].exists);
                                        if (!p)
                                                return log_oom();

                                        if (faccessat(fd, p, F_OK, AT_SYMLINK_NOFOLLOW) < 0)
                                                continue;
                                }

                                target = s;
                                break;
                        }

                        if (!target)
                                continue;

                        r = RET_NERRNO(symlinkat(target, fd, table[i].dir));
                } else {
                        /* Create as directory. */
                        WITH_UMASK(0000)
                                r = RET_NERRNO(mkdirat(fd, table[i].dir, table[i].mode));
                }
                if (r < 0) {
                        bool ignore = IN_SET(r, -EEXIST, -EROFS) || table[i].ignore_failure;
                        log_full_errno(ignore ? LOG_DEBUG : LOG_ERR, r,
                                       "Failed to create %s/%s: %m", root, table[i].dir);
                        if (ignore)
                                continue;

                        return r;
                }

                if (uid_is_valid(uid) || gid_is_valid(gid))
                        if (fchownat(fd, table[i].dir, uid, gid, AT_SYMLINK_NOFOLLOW) < 0)
                                return log_error_errno(errno, "Failed to chown %s/%s: %m", root, table[i].dir);
        }

        return 0;
}

int base_filesystem_create(const char *root, uid_t uid, gid_t gid) {
        _cleanup_close_ int fd = -EBADF;

        fd = open(ASSERT_PTR(root), O_DIRECTORY|O_CLOEXEC);
        if (fd < 0)
                return log_error_errno(errno, "Failed to open root file system: %m");

        return base_filesystem_create_fd(fd, root, uid, gid);
}
