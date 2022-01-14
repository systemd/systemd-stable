/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <unistd.h>

#include "alloc-util.h"
#include "btrfs-util.h"
#include "cgroup-util.h"
#include "dirent-util.h"
#include "fd-util.h"
#include "log.h"
#include "macro.h"
#include "mountpoint-util.h"
#include "path-util.h"
#include "rm-rf.h"
#include "stat-util.h"
#include "string-util.h"

static bool is_physical_fs(const struct statfs *sfs) {
        return !is_temporary_fs(sfs) && !is_cgroup_fs(sfs);
}

static int patch_dirfd_mode(
                int dfd,
                mode_t *ret_old_mode) {

        struct stat st;

        assert(dfd >= 0);
        assert(ret_old_mode);

        if (fstat(dfd, &st) < 0)
                return -errno;
        if (!S_ISDIR(st.st_mode))
                return -ENOTDIR;
        if (FLAGS_SET(st.st_mode, 0700)) /* Already set? */
                return -EACCES; /* original error */
        if (st.st_uid != geteuid())  /* this only works if the UID matches ours */
                return -EACCES;

        if (fchmod(dfd, (st.st_mode | 0700) & 07777) < 0)
                return -errno;

        *ret_old_mode = st.st_mode;
        return 0;
}

static int unlinkat_harder(
                int dfd,
                const char *filename,
                int unlink_flags,
                RemoveFlags remove_flags) {

        mode_t old_mode;
        int r;

        /* Like unlinkat(), but tries harder: if we get EACCESS we'll try to set the r/w/x bits on the
         * directory. This is useful if we run unprivileged and have some files where the w bit is
         * missing. */

        if (unlinkat(dfd, filename, unlink_flags) >= 0)
                return 0;
        if (errno != EACCES || !FLAGS_SET(remove_flags, REMOVE_CHMOD))
                return -errno;

        r = patch_dirfd_mode(dfd, &old_mode);
        if (r < 0)
                return r;

        if (unlinkat(dfd, filename, unlink_flags) < 0) {
                r = -errno;
                /* Try to restore the original access mode if this didn't work */
                (void) fchmod(dfd, old_mode);
                return r;
        }

        /* If this worked, we won't reset the old mode, since we'll need it for other entries too, and we
         * should destroy the whole thing */
        return 0;
}

static int fstatat_harder(
                int dfd,
                const char *filename,
                struct stat *ret,
                int fstatat_flags,
                RemoveFlags remove_flags) {

        mode_t old_mode;
        int r;

        /* Like unlink_harder() but does the same for fstatat() */

        if (fstatat(dfd, filename, ret, fstatat_flags) >= 0)
                return 0;
        if (errno != EACCES || !FLAGS_SET(remove_flags, REMOVE_CHMOD))
                return -errno;

        r = patch_dirfd_mode(dfd, &old_mode);
        if (r < 0)
                return r;

        if (fstatat(dfd, filename, ret, fstatat_flags) < 0) {
                r = -errno;
                (void) fchmod(dfd, old_mode);
                return r;
        }

        return 0;
}

int rm_rf_children(int fd, RemoveFlags flags, struct stat *root_dev) {
        _cleanup_closedir_ DIR *d = NULL;
        struct dirent *de;
        int ret = 0, r;
        struct statfs sfs;

        assert(fd >= 0);

        /* This returns the first error we run into, but nevertheless tries to go on. This closes the passed
         * fd, in all cases, including on failure.. */

        if (!(flags & REMOVE_PHYSICAL)) {

                r = fstatfs(fd, &sfs);
                if (r < 0) {
                        safe_close(fd);
                        return -errno;
                }

                if (is_physical_fs(&sfs)) {
                        /* We refuse to clean physical file systems with this call,
                         * unless explicitly requested. This is extra paranoia just
                         * to be sure we never ever remove non-state data. */
                        _cleanup_free_ char *path = NULL;

                        (void) fd_get_path(fd, &path);
                        log_error("Attempted to remove disk file system under \"%s\", and we can't allow that.",
                                  strna(path));

                        safe_close(fd);
                        return -EPERM;
                }
        }

        d = fdopendir(fd);
        if (!d) {
                safe_close(fd);
                return errno == ENOENT ? 0 : -errno;
        }

        FOREACH_DIRENT_ALL(de, d, return -errno) {
                bool is_dir;
                struct stat st;

                if (dot_or_dot_dot(de->d_name))
                        continue;

                if (de->d_type == DT_UNKNOWN ||
                    (de->d_type == DT_DIR && (root_dev || (flags & REMOVE_SUBVOLUME)))) {
                        r = fstatat_harder(fd, de->d_name, &st, AT_SYMLINK_NOFOLLOW, flags);
                        if (r < 0) {
                                if (ret == 0 && r != -ENOENT)
                                        ret = r;
                                continue;
                        }

                        is_dir = S_ISDIR(st.st_mode);
                } else
                        is_dir = de->d_type == DT_DIR;

                if (is_dir) {
                        _cleanup_close_ int subdir_fd = -1;

                        /* if root_dev is set, remove subdirectories only if device is same */
                        if (root_dev && st.st_dev != root_dev->st_dev)
                                continue;

                        subdir_fd = openat(fd, de->d_name, O_RDONLY|O_NONBLOCK|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW|O_NOATIME);
                        if (subdir_fd < 0) {
                                if (ret == 0 && errno != ENOENT)
                                        ret = -errno;
                                continue;
                        }

                        /* Stop at mount points */
                        r = fd_is_mount_point(fd, de->d_name, 0);
                        if (r < 0) {
                                if (ret == 0 && r != -ENOENT)
                                        ret = r;

                                continue;
                        }
                        if (r > 0)
                                continue;

                        if ((flags & REMOVE_SUBVOLUME) && btrfs_might_be_subvol(&st)) {

                                /* This could be a subvolume, try to remove it */

                                r = btrfs_subvol_remove_fd(fd, de->d_name, BTRFS_REMOVE_RECURSIVE|BTRFS_REMOVE_QUOTA);
                                if (r < 0) {
                                        if (!IN_SET(r, -ENOTTY, -EINVAL)) {
                                                if (ret == 0)
                                                        ret = r;

                                                continue;
                                        }

                                        /* ENOTTY, then it wasn't a btrfs subvolume, continue below. */
                                } else
                                        /* It was a subvolume, continue. */
                                        continue;
                        }

                        /* We pass REMOVE_PHYSICAL here, to avoid doing the fstatfs() to check the file
                         * system type again for each directory */
                        r = rm_rf_children(TAKE_FD(subdir_fd), flags | REMOVE_PHYSICAL, root_dev);
                        if (r < 0 && ret == 0)
                                ret = r;

                        r = unlinkat_harder(fd, de->d_name, AT_REMOVEDIR, flags);
                        if (r < 0 && r != -ENOENT && ret == 0)
                                ret = r;

                } else if (!(flags & REMOVE_ONLY_DIRECTORIES)) {

                        r = unlinkat_harder(fd, de->d_name, 0, flags);
                        if (r < 0 && r != -ENOENT && ret == 0)
                                ret = r;
                }
        }
        return ret;
}

int rm_rf(const char *path, RemoveFlags flags) {
        int fd, r;
        struct statfs s;

        assert(path);

        /* For now, don't support dropping subvols when also only dropping directories, since we can't do
         * this race-freely. */
        if (FLAGS_SET(flags, REMOVE_ONLY_DIRECTORIES|REMOVE_SUBVOLUME))
                return -EINVAL;

        /* We refuse to clean the root file system with this call. This is extra paranoia to never cause a
         * really seriously broken system. */
        if (path_equal_or_files_same(path, "/", AT_SYMLINK_NOFOLLOW))
                return log_error_errno(SYNTHETIC_ERRNO(EPERM),
                                       "Attempted to remove entire root file system (\"%s\"), and we can't allow that.",
                                       path);

        if (FLAGS_SET(flags, REMOVE_SUBVOLUME | REMOVE_ROOT | REMOVE_PHYSICAL)) {
                /* Try to remove as subvolume first */
                r = btrfs_subvol_remove(path, BTRFS_REMOVE_RECURSIVE|BTRFS_REMOVE_QUOTA);
                if (r >= 0)
                        return r;

                if (FLAGS_SET(flags, REMOVE_MISSING_OK) && r == -ENOENT)
                        return 0;

                if (!IN_SET(r, -ENOTTY, -EINVAL, -ENOTDIR))
                        return r;

                /* Not btrfs or not a subvolume */
        }

        fd = open(path, O_RDONLY|O_NONBLOCK|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW|O_NOATIME);
        if (fd < 0) {
                if (FLAGS_SET(flags, REMOVE_MISSING_OK) && errno == ENOENT)
                        return 0;

                if (!IN_SET(errno, ENOTDIR, ELOOP))
                        return -errno;

                if (FLAGS_SET(flags, REMOVE_ONLY_DIRECTORIES))
                        return 0;

                if (FLAGS_SET(flags, REMOVE_ROOT)) {

                        if (!FLAGS_SET(flags, REMOVE_PHYSICAL)) {
                                if (statfs(path, &s) < 0)
                                        return -errno;

                                if (is_physical_fs(&s))
                                        return log_error_errno(SYNTHETIC_ERRNO(EPERM),
                                                               "Attempted to remove files from a disk file system under \"%s\", refusing.",
                                                               path);
                        }

                        if (unlink(path) < 0) {
                                if (FLAGS_SET(flags, REMOVE_MISSING_OK) && errno == ENOENT)
                                        return 0;

                                return -errno;
                        }
                }

                return 0;
        }

        r = rm_rf_children(fd, flags, NULL);

        if (FLAGS_SET(flags, REMOVE_ROOT) &&
            rmdir(path) < 0 &&
            r >= 0 &&
            (!FLAGS_SET(flags, REMOVE_MISSING_OK) || errno != ENOENT))
                r = -errno;

        return r;
}
