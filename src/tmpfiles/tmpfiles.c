/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <getopt.h>
#include <limits.h>
#include <linux/fs.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/xattr.h>
#include <sysexits.h>
#include <time.h>
#include <unistd.h>

#include "sd-path.h"

#include "acl-util.h"
#include "alloc-util.h"
#include "btrfs-util.h"
#include "capability-util.h"
#include "chattr-util.h"
#include "conf-files.h"
#include "copy.h"
#include "def.h"
#include "dirent-util.h"
#include "dissect-image.h"
#include "env-util.h"
#include "escape.h"
#include "fd-util.h"
#include "fileio.h"
#include "format-util.h"
#include "fs-util.h"
#include "glob-util.h"
#include "io-util.h"
#include "label.h"
#include "log.h"
#include "macro.h"
#include "main-func.h"
#include "missing_stat.h"
#include "missing_syscall.h"
#include "mkdir.h"
#include "mount-util.h"
#include "mountpoint-util.h"
#include "offline-passwd.h"
#include "pager.h"
#include "parse-argument.h"
#include "parse-util.h"
#include "path-lookup.h"
#include "path-util.h"
#include "pretty-print.h"
#include "rlimit-util.h"
#include "rm-rf.h"
#include "selinux-util.h"
#include "set.h"
#include "sort-util.h"
#include "specifier.h"
#include "stat-util.h"
#include "stdio-util.h"
#include "string-table.h"
#include "string-util.h"
#include "strv.h"
#include "terminal-util.h"
#include "umask-util.h"
#include "user-util.h"

/* This reads all files listed in /etc/tmpfiles.d/?*.conf and creates
 * them in the file system. This is intended to be used to create
 * properly owned directories beneath /tmp, /var/tmp, /run, which are
 * volatile and hence need to be recreated on bootup. */

typedef enum OperationMask {
        OPERATION_CREATE = 1 << 0,
        OPERATION_REMOVE = 1 << 1,
        OPERATION_CLEAN  = 1 << 2,
} OperationMask;

typedef enum ItemType {
        /* These ones take file names */
        CREATE_FILE = 'f',
        TRUNCATE_FILE = 'F', /* deprecated: use f+ */
        CREATE_DIRECTORY = 'd',
        TRUNCATE_DIRECTORY = 'D',
        CREATE_SUBVOLUME = 'v',
        CREATE_SUBVOLUME_INHERIT_QUOTA = 'q',
        CREATE_SUBVOLUME_NEW_QUOTA = 'Q',
        CREATE_FIFO = 'p',
        CREATE_SYMLINK = 'L',
        CREATE_CHAR_DEVICE = 'c',
        CREATE_BLOCK_DEVICE = 'b',
        COPY_FILES = 'C',

        /* These ones take globs */
        WRITE_FILE = 'w',
        EMPTY_DIRECTORY = 'e',
        SET_XATTR = 't',
        RECURSIVE_SET_XATTR = 'T',
        SET_ACL = 'a',
        RECURSIVE_SET_ACL = 'A',
        SET_ATTRIBUTE = 'h',
        RECURSIVE_SET_ATTRIBUTE = 'H',
        IGNORE_PATH = 'x',
        IGNORE_DIRECTORY_PATH = 'X',
        REMOVE_PATH = 'r',
        RECURSIVE_REMOVE_PATH = 'R',
        RELABEL_PATH = 'z',
        RECURSIVE_RELABEL_PATH = 'Z',
        ADJUST_MODE = 'm', /* legacy, 'z' is identical to this */
} ItemType;

typedef enum AgeBy {
        AGE_BY_ATIME = 1 << 0,
        AGE_BY_BTIME = 1 << 1,
        AGE_BY_CTIME = 1 << 2,
        AGE_BY_MTIME = 1 << 3,

        /* All file timestamp types are checked by default. */
        AGE_BY_DEFAULT_FILE = AGE_BY_ATIME | AGE_BY_BTIME | AGE_BY_CTIME | AGE_BY_MTIME,
        AGE_BY_DEFAULT_DIR  = AGE_BY_ATIME | AGE_BY_BTIME | AGE_BY_MTIME
} AgeBy;

typedef struct Item {
        ItemType type;

        char *path;
        char *argument;
        char **xattrs;
#if HAVE_ACL
        acl_t acl_access;
        acl_t acl_default;
#endif
        uid_t uid;
        gid_t gid;
        mode_t mode;
        usec_t age;
        AgeBy age_by_file, age_by_dir;

        dev_t major_minor;
        unsigned attribute_value;
        unsigned attribute_mask;

        bool uid_set:1;
        bool gid_set:1;
        bool mode_set:1;
        bool age_set:1;
        bool mask_perms:1;
        bool attribute_set:1;

        bool keep_first_level:1;

        bool append_or_force:1;

        bool allow_failure:1;

        bool try_replace:1;

        OperationMask done;
} Item;

typedef struct ItemArray {
        Item *items;
        size_t n_items;

        struct ItemArray *parent;
        Set *children;
} ItemArray;

typedef enum DirectoryType {
        DIRECTORY_RUNTIME,
        DIRECTORY_STATE,
        DIRECTORY_CACHE,
        DIRECTORY_LOGS,
        _DIRECTORY_TYPE_MAX,
} DirectoryType;

static bool arg_cat_config = false;
static bool arg_user = false;
static OperationMask arg_operation = 0;
static bool arg_boot = false;
static PagerFlags arg_pager_flags = 0;

static char **arg_include_prefixes = NULL;
static char **arg_exclude_prefixes = NULL;
static char *arg_root = NULL;
static char *arg_image = NULL;
static char *arg_replace = NULL;

#define MAX_DEPTH 256

static OrderedHashmap *items = NULL, *globs = NULL;
static Set *unix_sockets = NULL;

STATIC_DESTRUCTOR_REGISTER(items, ordered_hashmap_freep);
STATIC_DESTRUCTOR_REGISTER(globs, ordered_hashmap_freep);
STATIC_DESTRUCTOR_REGISTER(unix_sockets, set_free_freep);
STATIC_DESTRUCTOR_REGISTER(arg_include_prefixes, freep);
STATIC_DESTRUCTOR_REGISTER(arg_exclude_prefixes, freep);
STATIC_DESTRUCTOR_REGISTER(arg_root, freep);
STATIC_DESTRUCTOR_REGISTER(arg_image, freep);

static int specifier_machine_id_safe(char specifier, const void *data, const char *root, const void *userdata, char **ret);
static int specifier_directory(char specifier, const void *data, const char *root, const void *userdata, char **ret);

static const Specifier specifier_table[] = {
        { 'a', specifier_architecture,    NULL },
        { 'b', specifier_boot_id,         NULL },
        { 'B', specifier_os_build_id,     NULL },
        { 'H', specifier_host_name,       NULL },
        { 'l', specifier_short_host_name, NULL },
        { 'm', specifier_machine_id_safe, NULL },
        { 'o', specifier_os_id,           NULL },
        { 'v', specifier_kernel_release,  NULL },
        { 'w', specifier_os_version_id,   NULL },
        { 'W', specifier_os_variant_id,   NULL },

        { 'h', specifier_user_home,       NULL },

        { 'C', specifier_directory,       UINT_TO_PTR(DIRECTORY_CACHE) },
        { 'L', specifier_directory,       UINT_TO_PTR(DIRECTORY_LOGS) },
        { 'S', specifier_directory,       UINT_TO_PTR(DIRECTORY_STATE) },
        { 't', specifier_directory,       UINT_TO_PTR(DIRECTORY_RUNTIME) },

        COMMON_CREDS_SPECIFIERS,

        COMMON_TMP_SPECIFIERS,
        {}
};

static int specifier_machine_id_safe(char specifier, const void *data, const char *root, const void *userdata, char **ret) {
        int r;

        /* If /etc/machine_id is missing or empty (e.g. in a chroot environment) return a recognizable error
         * so that the caller can skip the rule gracefully. */

        r = specifier_machine_id(specifier, data, root, userdata, ret);
        if (IN_SET(r, -ENOENT, -ENOMEDIUM))
                return -ENXIO;

        return r;
}

static int specifier_directory(char specifier, const void *data, const char *root, const void *userdata, char **ret) {
        struct table_entry {
                uint64_t type;
                const char *suffix;
        };

        static const struct table_entry paths_system[] = {
                [DIRECTORY_RUNTIME] = { SD_PATH_SYSTEM_RUNTIME            },
                [DIRECTORY_STATE] =   { SD_PATH_SYSTEM_STATE_PRIVATE      },
                [DIRECTORY_CACHE] =   { SD_PATH_SYSTEM_STATE_CACHE        },
                [DIRECTORY_LOGS] =    { SD_PATH_SYSTEM_STATE_LOGS         },
        };

        static const struct table_entry paths_user[] = {
                [DIRECTORY_RUNTIME] = { SD_PATH_USER_RUNTIME              },
                [DIRECTORY_STATE] =   { SD_PATH_USER_CONFIGURATION        },
                [DIRECTORY_CACHE] =   { SD_PATH_USER_STATE_CACHE          },
                [DIRECTORY_LOGS] =    { SD_PATH_USER_CONFIGURATION, "log" },
        };

        const struct table_entry *paths;
        _cleanup_free_ char *p = NULL;
        unsigned i;
        int r;

        assert_cc(ELEMENTSOF(paths_system) == ELEMENTSOF(paths_user));
        paths = arg_user ? paths_user : paths_system;

        i = PTR_TO_UINT(data);
        assert(i < ELEMENTSOF(paths_system));

        r = sd_path_lookup(paths[i].type, paths[i].suffix, &p);
        if (r < 0)
                return r;

        if (arg_root) {
                _cleanup_free_ char *j = NULL;

                j = path_join(arg_root, p);
                if (!j)
                        return -ENOMEM;

                *ret = TAKE_PTR(j);
        } else
                *ret = TAKE_PTR(p);

        return 0;
}

static int log_unresolvable_specifier(const char *filename, unsigned line) {
        static bool notified = false;

        /* In system mode, this is called when /etc is not fully initialized (e.g.
         * in a chroot environment) where some specifiers are unresolvable. In user
         * mode, this is called when some variables are not defined. These cases are
         * not considered as an error so log at LOG_NOTICE only for the first time
         * and then downgrade this to LOG_DEBUG for the rest. */

        log_syntax(NULL,
                   notified ? LOG_DEBUG : LOG_NOTICE,
                   filename, line, 0,
                   "Failed to resolve specifier: %s, skipping",
                   arg_user ? "Required $XDG_... variable not defined" : "uninitialized /etc detected");

        if (!notified)
                log_notice("All rules containing unresolvable specifiers will be skipped.");

        notified = true;
        return 0;
}

static int user_config_paths(char*** ret) {
        _cleanup_strv_free_ char **config_dirs = NULL, **data_dirs = NULL;
        _cleanup_free_ char *persistent_config = NULL, *runtime_config = NULL, *data_home = NULL;
        _cleanup_strv_free_ char **res = NULL;
        int r;

        r = xdg_user_dirs(&config_dirs, &data_dirs);
        if (r < 0)
                return r;

        r = xdg_user_config_dir(&persistent_config, "/user-tmpfiles.d");
        if (r < 0 && r != -ENXIO)
                return r;

        r = xdg_user_runtime_dir(&runtime_config, "/user-tmpfiles.d");
        if (r < 0 && r != -ENXIO)
                return r;

        r = xdg_user_data_dir(&data_home, "/user-tmpfiles.d");
        if (r < 0 && r != -ENXIO)
                return r;

        r = strv_extend_strv_concat(&res, config_dirs, "/user-tmpfiles.d");
        if (r < 0)
                return r;

        r = strv_extend(&res, persistent_config);
        if (r < 0)
                return r;

        r = strv_extend(&res, runtime_config);
        if (r < 0)
                return r;

        r = strv_extend(&res, data_home);
        if (r < 0)
                return r;

        r = strv_extend_strv_concat(&res, data_dirs, "/user-tmpfiles.d");
        if (r < 0)
                return r;

        r = path_strv_make_absolute_cwd(res);
        if (r < 0)
                return r;

        *ret = TAKE_PTR(res);
        return 0;
}

static bool needs_glob(ItemType t) {
        return IN_SET(t,
                      WRITE_FILE,
                      IGNORE_PATH,
                      IGNORE_DIRECTORY_PATH,
                      REMOVE_PATH,
                      RECURSIVE_REMOVE_PATH,
                      EMPTY_DIRECTORY,
                      ADJUST_MODE,
                      RELABEL_PATH,
                      RECURSIVE_RELABEL_PATH,
                      SET_XATTR,
                      RECURSIVE_SET_XATTR,
                      SET_ACL,
                      RECURSIVE_SET_ACL,
                      SET_ATTRIBUTE,
                      RECURSIVE_SET_ATTRIBUTE);
}

static bool takes_ownership(ItemType t) {
        return IN_SET(t,
                      CREATE_FILE,
                      TRUNCATE_FILE,
                      CREATE_DIRECTORY,
                      EMPTY_DIRECTORY,
                      TRUNCATE_DIRECTORY,
                      CREATE_SUBVOLUME,
                      CREATE_SUBVOLUME_INHERIT_QUOTA,
                      CREATE_SUBVOLUME_NEW_QUOTA,
                      CREATE_FIFO,
                      CREATE_SYMLINK,
                      CREATE_CHAR_DEVICE,
                      CREATE_BLOCK_DEVICE,
                      COPY_FILES,
                      WRITE_FILE,
                      IGNORE_PATH,
                      IGNORE_DIRECTORY_PATH,
                      REMOVE_PATH,
                      RECURSIVE_REMOVE_PATH);
}

static struct Item* find_glob(OrderedHashmap *h, const char *match) {
        ItemArray *j;

        ORDERED_HASHMAP_FOREACH(j, h) {
                size_t n;

                for (n = 0; n < j->n_items; n++) {
                        Item *item = j->items + n;

                        if (fnmatch(item->path, match, FNM_PATHNAME|FNM_PERIOD) == 0)
                                return item;
                }
        }

        return NULL;
}

static int load_unix_sockets(void) {
        _cleanup_set_free_free_ Set *sockets = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        if (unix_sockets)
                return 0;

        /* We maintain a cache of the sockets we found in /proc/net/unix to speed things up a little. */

        sockets = set_new(&path_hash_ops);
        if (!sockets)
                return log_oom();

        f = fopen("/proc/net/unix", "re");
        if (!f)
                return log_full_errno(errno == ENOENT ? LOG_DEBUG : LOG_WARNING, errno,
                                      "Failed to open /proc/net/unix, ignoring: %m");

        /* Skip header */
        r = read_line(f, LONG_LINE_MAX, NULL);
        if (r < 0)
                return log_warning_errno(r, "Failed to skip /proc/net/unix header line: %m");
        if (r == 0)
                return log_warning_errno(SYNTHETIC_ERRNO(EIO), "Premature end of file reading /proc/net/unix.");

        for (;;) {
                _cleanup_free_ char *line = NULL, *s = NULL;
                char *p;

                r = read_line(f, LONG_LINE_MAX, &line);
                if (r < 0)
                        return log_warning_errno(r, "Failed to read /proc/net/unix line, ignoring: %m");
                if (r == 0) /* EOF */
                        break;

                p = strchr(line, ':');
                if (!p)
                        continue;

                if (strlen(p) < 37)
                        continue;

                p += 37;
                p += strspn(p, WHITESPACE);
                p += strcspn(p, WHITESPACE); /* skip one more word */
                p += strspn(p, WHITESPACE);

                if (*p != '/')
                        continue;

                s = strdup(p);
                if (!s)
                        return log_oom();

                path_simplify(s);

                r = set_consume(sockets, s);
                if (r == -EEXIST)
                        continue;
                if (r < 0)
                        return log_warning_errno(r, "Failed to add AF_UNIX socket to set, ignoring: %m");

                TAKE_PTR(s);
        }

        unix_sockets = TAKE_PTR(sockets);
        return 1;
}

static bool unix_socket_alive(const char *fn) {
        assert(fn);

        if (load_unix_sockets() < 0)
                return true;     /* We don't know, so assume yes */

        return !!set_get(unix_sockets, (char*) fn);
}

static DIR* xopendirat_nomod(int dirfd, const char *path) {
        DIR *dir;

        dir = xopendirat(dirfd, path, O_NOFOLLOW|O_NOATIME);
        if (dir)
                return dir;

        log_debug_errno(errno, "Cannot open %sdirectory \"%s\": %m", dirfd == AT_FDCWD ? "" : "sub", path);
        if (errno != EPERM)
                return NULL;

        dir = xopendirat(dirfd, path, O_NOFOLLOW);
        if (!dir)
                log_debug_errno(errno, "Cannot open %sdirectory \"%s\": %m", dirfd == AT_FDCWD ? "" : "sub", path);

        return dir;
}

static DIR* opendir_nomod(const char *path) {
        return xopendirat_nomod(AT_FDCWD, path);
}

static inline nsec_t load_statx_timestamp_nsec(const struct statx_timestamp *ts) {
        assert(ts);

        if (ts->tv_sec < 0)
                return NSEC_INFINITY;

        if ((nsec_t) ts->tv_sec >= (UINT64_MAX - ts->tv_nsec) / NSEC_PER_SEC)
                return NSEC_INFINITY;

        return ts->tv_sec * NSEC_PER_SEC + ts->tv_nsec;
}

static bool needs_cleanup(
                nsec_t atime,
                nsec_t btime,
                nsec_t ctime,
                nsec_t mtime,
                nsec_t cutoff,
                const char *sub_path,
                AgeBy age_by,
                bool is_dir) {

        if (FLAGS_SET(age_by, AGE_BY_MTIME) && mtime != NSEC_INFINITY && mtime >= cutoff) {
                char a[FORMAT_TIMESTAMP_MAX];
                /* Follows spelling in stat(1). */
                log_debug("%s \"%s\": modify time %s is too new.",
                          is_dir ? "Directory" : "File",
                          sub_path,
                          format_timestamp_style(a, sizeof(a), mtime / NSEC_PER_USEC, TIMESTAMP_US));

                return false;
        }

        if (FLAGS_SET(age_by, AGE_BY_ATIME) && atime != NSEC_INFINITY && atime >= cutoff) {
                char a[FORMAT_TIMESTAMP_MAX];
                log_debug("%s \"%s\": access time %s is too new.",
                          is_dir ? "Directory" : "File",
                          sub_path,
                          format_timestamp_style(a, sizeof(a), atime / NSEC_PER_USEC, TIMESTAMP_US));

                return false;
        }

        /*
         * Note: Unless explicitly specified by the user, "ctime" is ignored
         * by default for directories, because we change it when deleting.
         */
        if (FLAGS_SET(age_by, AGE_BY_CTIME) && ctime != NSEC_INFINITY && ctime >= cutoff) {
                char a[FORMAT_TIMESTAMP_MAX];
                log_debug("%s \"%s\": change time %s is too new.",
                          is_dir ? "Directory" : "File",
                          sub_path,
                          format_timestamp_style(a, sizeof(a), ctime / NSEC_PER_USEC, TIMESTAMP_US));

                return false;
        }

        if (FLAGS_SET(age_by, AGE_BY_BTIME) && btime != NSEC_INFINITY && btime >= cutoff) {
                char a[FORMAT_TIMESTAMP_MAX];
                log_debug("%s \"%s\": birth time %s is too new.",
                          is_dir ? "Directory" : "File",
                          sub_path,
                          format_timestamp_style(a, sizeof(a), btime / NSEC_PER_USEC, TIMESTAMP_US));

                return false;
        }

        return true;
}

static int dir_cleanup(
                Item *i,
                const char *p,
                DIR *d,
                nsec_t self_atime_nsec,
                nsec_t self_mtime_nsec,
                nsec_t cutoff_nsec,
                dev_t rootdev_major,
                dev_t rootdev_minor,
                bool mountpoint,
                int maxdepth,
                bool keep_this_level,
                AgeBy age_by_file,
                AgeBy age_by_dir) {

        bool deleted = false;
        struct dirent *dent;
        int r = 0;

        FOREACH_DIRENT_ALL(dent, d, break) {
                _cleanup_free_ char *sub_path = NULL;
                nsec_t atime_nsec, mtime_nsec, ctime_nsec, btime_nsec;

                if (dot_or_dot_dot(dent->d_name))
                        continue;

                /* If statx() is supported, use it. It's preferable over fstatat() since it tells us
                 * explicitly where we are looking at a mount point, for free as side information. Determining
                 * the same information without statx() is hard, see the complexity of path_is_mount_point(),
                 * and also much slower as it requires a number of syscalls instead of just one. Hence, when
                 * we have modern statx() we use it instead of fstat() and do proper mount point checks,
                 * while on older kernels's well do traditional st_dev based detection of mount points.
                 *
                 * Using statx() for detecting mount points also has the benfit that we handle weird file
                 * systems such as overlayfs better where each file is originating from a different
                 * st_dev. */

                STRUCT_STATX_DEFINE(sx);

                r = statx_fallback(
                                dirfd(d), dent->d_name,
                                AT_SYMLINK_NOFOLLOW|AT_NO_AUTOMOUNT,
                                STATX_TYPE|STATX_MODE|STATX_UID|STATX_ATIME|STATX_MTIME|STATX_CTIME|STATX_BTIME,
                                &sx);
                if (r == -ENOENT)
                        continue;
                if (r < 0) {
                        /* FUSE, NFS mounts, SELinux might return EACCES */
                        r = log_full_errno(errno == EACCES ? LOG_DEBUG : LOG_ERR, errno,
                                           "statx(%s/%s) failed: %m", p, dent->d_name);
                        continue;
                }

                if (FLAGS_SET(sx.stx_attributes_mask, STATX_ATTR_MOUNT_ROOT)) {
                        /* Yay, we have the mount point API, use it */
                        if (FLAGS_SET(sx.stx_attributes, STATX_ATTR_MOUNT_ROOT)) {
                                log_debug("Ignoring \"%s/%s\": different mount points.", p, dent->d_name);
                                continue;
                        }
                } else {
                        /* So we might have statx() but the STATX_ATTR_MOUNT_ROOT flag is not supported, fall
                         * back to traditional stx_dev checking. */
                        if (sx.stx_dev_major != rootdev_major ||
                            sx.stx_dev_minor != rootdev_minor) {
                                log_debug("Ignoring \"%s/%s\": different filesystem.", p, dent->d_name);
                                continue;
                        }

                        /* Try to detect bind mounts of the same filesystem instance; they do not differ in device
                         * major/minors. This type of query is not supported on all kernels or filesystem types
                         * though. */
                        if (S_ISDIR(sx.stx_mode)) {
                                int q;

                                q = fd_is_mount_point(dirfd(d), dent->d_name, 0);
                                if (q < 0)
                                        log_debug_errno(q, "Failed to determine whether \"%s/%s\" is a mount point, ignoring: %m", p, dent->d_name);
                                else if (q > 0) {
                                        log_debug("Ignoring \"%s/%s\": different mount of the same filesystem.", p, dent->d_name);
                                        continue;
                                }
                        }
                }

                atime_nsec = FLAGS_SET(sx.stx_mask, STATX_ATIME) ? load_statx_timestamp_nsec(&sx.stx_atime) : 0;
                mtime_nsec = FLAGS_SET(sx.stx_mask, STATX_MTIME) ? load_statx_timestamp_nsec(&sx.stx_mtime) : 0;
                ctime_nsec = FLAGS_SET(sx.stx_mask, STATX_CTIME) ? load_statx_timestamp_nsec(&sx.stx_ctime) : 0;
                btime_nsec = FLAGS_SET(sx.stx_mask, STATX_BTIME) ? load_statx_timestamp_nsec(&sx.stx_btime) : 0;

                sub_path = path_join(p, dent->d_name);
                if (!sub_path) {
                        r = log_oom();
                        goto finish;
                }

                /* Is there an item configured for this path? */
                if (ordered_hashmap_get(items, sub_path)) {
                        log_debug("Ignoring \"%s\": a separate entry exists.", sub_path);
                        continue;
                }

                if (find_glob(globs, sub_path)) {
                        log_debug("Ignoring \"%s\": a separate glob exists.", sub_path);
                        continue;
                }

                if (S_ISDIR(sx.stx_mode)) {
                        _cleanup_closedir_ DIR *sub_dir = NULL;

                        if (mountpoint &&
                            streq(dent->d_name, "lost+found") &&
                            sx.stx_uid == 0) {
                                log_debug("Ignoring directory \"%s\".", sub_path);
                                continue;
                        }

                        if (maxdepth <= 0)
                                log_warning("Reached max depth on \"%s\".", sub_path);
                        else {
                                int q;

                                sub_dir = xopendirat_nomod(dirfd(d), dent->d_name);
                                if (!sub_dir) {
                                        if (errno != ENOENT)
                                                r = log_warning_errno(errno, "Opening directory \"%s\" failed, ignoring: %m", sub_path);

                                        continue;
                                }

                                if (flock(dirfd(sub_dir), LOCK_EX|LOCK_NB) < 0) {
                                        log_debug_errno(errno, "Couldn't acquire shared BSD lock on directory \"%s\", skipping: %m", p);
                                        continue;
                                }

                                q = dir_cleanup(i,
                                                sub_path, sub_dir,
                                                atime_nsec, mtime_nsec, cutoff_nsec,
                                                rootdev_major, rootdev_minor,
                                                false, maxdepth-1, false,
                                                age_by_file, age_by_dir);
                                if (q < 0)
                                        r = q;
                        }

                        /* Note: if you are wondering why we don't support the sticky bit for excluding
                         * directories from cleaning like we do it for other file system objects: well, the
                         * sticky bit already has a meaning for directories, so we don't want to overload
                         * that. */

                        if (keep_this_level) {
                                log_debug("Keeping directory \"%s\".", sub_path);
                                continue;
                        }

                        /*
                         * Check the file timestamps of an entry against the
                         * given cutoff time; delete if it is older.
                         */
                        if (!needs_cleanup(atime_nsec, btime_nsec, ctime_nsec, mtime_nsec,
                                           cutoff_nsec, sub_path, age_by_dir, true))
                                continue;

                        log_debug("Removing directory \"%s\".", sub_path);
                        if (unlinkat(dirfd(d), dent->d_name, AT_REMOVEDIR) < 0)
                                if (!IN_SET(errno, ENOENT, ENOTEMPTY))
                                        r = log_warning_errno(errno, "Failed to remove directory \"%s\", ignoring: %m", sub_path);

                } else {
                        /* Skip files for which the sticky bit is set. These are semantics we define, and are
                         * unknown elsewhere. See XDG_RUNTIME_DIR specification for details. */
                        if (sx.stx_mode & S_ISVTX) {
                                log_debug("Skipping \"%s\": sticky bit set.", sub_path);
                                continue;
                        }

                        if (mountpoint &&
                            S_ISREG(sx.stx_mode) &&
                            sx.stx_uid == 0 &&
                            STR_IN_SET(dent->d_name,
                                       ".journal",
                                       "aquota.user",
                                       "aquota.group")) {
                                log_debug("Skipping \"%s\".", sub_path);
                                continue;
                        }

                        /* Ignore sockets that are listed in /proc/net/unix */
                        if (S_ISSOCK(sx.stx_mode) && unix_socket_alive(sub_path)) {
                                log_debug("Skipping \"%s\": live socket.", sub_path);
                                continue;
                        }

                        /* Ignore device nodes */
                        if (S_ISCHR(sx.stx_mode) || S_ISBLK(sx.stx_mode)) {
                                log_debug("Skipping \"%s\": a device.", sub_path);
                                continue;
                        }

                        /* Keep files on this level around if this is requested */
                        if (keep_this_level) {
                                log_debug("Keeping \"%s\".", sub_path);
                                continue;
                        }

                        if (!needs_cleanup(atime_nsec, btime_nsec, ctime_nsec, mtime_nsec,
                                           cutoff_nsec, sub_path, age_by_file, false))
                                continue;

                        log_debug("Removing \"%s\".", sub_path);
                        if (unlinkat(dirfd(d), dent->d_name, 0) < 0)
                                if (errno != ENOENT)
                                        r = log_warning_errno(errno, "Failed to remove \"%s\", ignoring: %m", sub_path);

                        deleted = true;
                }
        }

finish:
        if (deleted) {
                char a[FORMAT_TIMESTAMP_MAX], m[FORMAT_TIMESTAMP_MAX];
                struct timespec ts[2];

                log_debug("Restoring access and modification time on \"%s\": %s, %s",
                          p,
                          format_timestamp_style(a, sizeof(a), self_atime_nsec / NSEC_PER_USEC, TIMESTAMP_US),
                          format_timestamp_style(m, sizeof(m), self_mtime_nsec / NSEC_PER_USEC, TIMESTAMP_US));

                timespec_store_nsec(ts + 0, self_atime_nsec);
                timespec_store_nsec(ts + 1, self_mtime_nsec);

                /* Restore original directory timestamps */
                if (futimens(dirfd(d), ts) < 0)
                        log_warning_errno(errno, "Failed to revert timestamps of '%s', ignoring: %m", p);
        }

        return r;
}

static bool dangerous_hardlinks(void) {
        _cleanup_free_ char *value = NULL;
        static int cached = -1;
        int r;

        /* Check whether the fs.protected_hardlinks sysctl is on. If we can't determine it we assume its off, as that's
         * what the upstream default is. */

        if (cached >= 0)
                return cached;

        r = read_one_line_file("/proc/sys/fs/protected_hardlinks", &value);
        if (r < 0) {
                log_debug_errno(r, "Failed to read fs.protected_hardlinks sysctl: %m");
                return true;
        }

        r = parse_boolean(value);
        if (r < 0) {
                log_debug_errno(r, "Failed to parse fs.protected_hardlinks sysctl: %m");
                return true;
        }

        cached = r == 0;
        return cached;
}

static bool hardlink_vulnerable(const struct stat *st) {
        assert(st);

        return !S_ISDIR(st->st_mode) && st->st_nlink > 1 && dangerous_hardlinks();
}

static mode_t process_mask_perms(mode_t mode, mode_t current) {

        if ((current & 0111) == 0)
                mode &= ~0111;
        if ((current & 0222) == 0)
                mode &= ~0222;
        if ((current & 0444) == 0)
                mode &= ~0444;
        if (!S_ISDIR(current))
                mode &= ~07000; /* remove sticky/sgid/suid bit, unless directory */

        return mode;
}

static int fd_set_perms(Item *i, int fd, const char *path, const struct stat *st) {
        struct stat stbuf;
        mode_t new_mode;
        bool do_chown;
        int r;

        assert(i);
        assert(fd >= 0);
        assert(path);

        if (!i->mode_set && !i->uid_set && !i->gid_set)
                goto shortcut;

        if (!st) {
                if (fstat(fd, &stbuf) < 0)
                        return log_error_errno(errno, "fstat(%s) failed: %m", path);
                st = &stbuf;
        }

        if (hardlink_vulnerable(st))
                return log_error_errno(SYNTHETIC_ERRNO(EPERM),
                                       "Refusing to set permissions on hardlinked file %s while the fs.protected_hardlinks sysctl is turned off.",
                                       path);

        /* Do we need a chown()? */
        do_chown =
                (i->uid_set && i->uid != st->st_uid) ||
                (i->gid_set && i->gid != st->st_gid);

        /* Calculate the mode to apply */
        new_mode = i->mode_set ? (i->mask_perms ?
                                  process_mask_perms(i->mode, st->st_mode) :
                                  i->mode) :
                                 (st->st_mode & 07777);

        if (i->mode_set && do_chown) {
                /* Before we issue the chmod() let's reduce the access mode to the common bits of the old and
                 * the new mode. That way there's no time window where the file exists under the old owner
                 * with more than the old access modes — and not under the new owner with more than the new
                 * access modes either. */

                if (S_ISLNK(st->st_mode))
                        log_debug("Skipping temporary mode fix for symlink %s.", path);
                else {
                        mode_t m = new_mode & st->st_mode; /* Mask new mode by old mode */

                        if (((m ^ st->st_mode) & 07777) == 0)
                                log_debug("\"%s\" matches temporary mode %o already.", path, m);
                        else {
                                log_debug("Temporarily changing \"%s\" to mode %o.", path, m);
                                r = fchmod_opath(fd, m);
                                if (r < 0)
                                        return log_error_errno(r, "fchmod() of %s failed: %m", path);
                        }
                }
        }

        if (do_chown) {
                log_debug("Changing \"%s\" to owner "UID_FMT":"GID_FMT,
                          path,
                          i->uid_set ? i->uid : UID_INVALID,
                          i->gid_set ? i->gid : GID_INVALID);

                if (fchownat(fd,
                             "",
                             i->uid_set ? i->uid : UID_INVALID,
                             i->gid_set ? i->gid : GID_INVALID,
                             AT_EMPTY_PATH) < 0)
                        return log_error_errno(errno, "fchownat() of %s failed: %m", path);
        }

        /* Now, apply the final mode. We do this in two cases: when the user set a mode explicitly, or after a
         * chown(), since chown()'s mangle the access mode in regards to sgid/suid in some conditions. */
        if (i->mode_set || do_chown) {
                if (S_ISLNK(st->st_mode))
                        log_debug("Skipping mode fix for symlink %s.", path);
                else {
                       /* Check if the chmod() is unnecessary. Note that if we did a chown() before we always
                        * chmod() here again, since it might have mangled the bits. */
                        if (!do_chown && ((new_mode ^ st->st_mode) & 07777) == 0)
                                log_debug("\"%s\" matches mode %o already.", path, new_mode);
                        else {
                                log_debug("Changing \"%s\" to mode %o.", path, new_mode);
                                r = fchmod_opath(fd, new_mode);
                                if (r < 0)
                                        return log_error_errno(r, "fchmod() of %s failed: %m", path);
                        }
                }
        }

shortcut:
        return label_fix(path, 0);
}

static int path_open_parent_safe(const char *path) {
        _cleanup_free_ char *dn = NULL;
        int r, fd;

        if (path_equal(path, "/") || !path_is_normalized(path))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "Failed to open parent of '%s': invalid path.",
                                       path);

        dn = dirname_malloc(path);
        if (!dn)
                return log_oom();

        r = chase_symlinks(dn, arg_root, CHASE_SAFE|CHASE_WARN, NULL, &fd);
        if (r < 0 && r != -ENOLINK)
                return log_error_errno(r, "Failed to validate path %s: %m", path);

        return r < 0 ? r : fd;
}

static int path_open_safe(const char *path) {
        int r, fd;

        /* path_open_safe() returns a file descriptor opened with O_PATH after
         * verifying that the path doesn't contain unsafe transitions, except
         * for its final component as the function does not follow symlink. */

        assert(path);

        if (!path_is_normalized(path))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "Failed to open invalid path '%s'.",
                                       path);

        r = chase_symlinks(path, arg_root, CHASE_SAFE|CHASE_WARN|CHASE_NOFOLLOW, NULL, &fd);
        if (r < 0 && r != -ENOLINK)
                return log_error_errno(r, "Failed to validate path %s: %m", path);

        return r < 0 ? r : fd;
}

static int path_set_perms(Item *i, const char *path) {
        _cleanup_close_ int fd = -1;

        assert(i);
        assert(path);

        fd = path_open_safe(path);
        if (fd < 0)
                return fd;

        return fd_set_perms(i, fd, path, NULL);
}

static int parse_xattrs_from_arg(Item *i) {
        const char *p;
        int r;

        assert(i);
        assert(i->argument);

        p = i->argument;

        for (;;) {
                _cleanup_free_ char *name = NULL, *value = NULL, *xattr = NULL;

                r = extract_first_word(&p, &xattr, NULL, EXTRACT_UNQUOTE|EXTRACT_CUNESCAPE);
                if (r < 0)
                        log_warning_errno(r, "Failed to parse extended attribute '%s', ignoring: %m", p);
                if (r <= 0)
                        break;

                r = split_pair(xattr, "=", &name, &value);
                if (r < 0) {
                        log_warning_errno(r, "Failed to parse extended attribute, ignoring: %s", xattr);
                        continue;
                }

                if (isempty(name) || isempty(value)) {
                        log_warning("Malformed extended attribute found, ignoring: %s", xattr);
                        continue;
                }

                if (strv_push_pair(&i->xattrs, name, value) < 0)
                        return log_oom();

                name = value = NULL;
        }

        return 0;
}

static int fd_set_xattrs(Item *i, int fd, const char *path, const struct stat *st) {
        char procfs_path[STRLEN("/proc/self/fd/") + DECIMAL_STR_MAX(int)];
        char **name, **value;

        assert(i);
        assert(fd >= 0);
        assert(path);

        xsprintf(procfs_path, "/proc/self/fd/%i", fd);

        STRV_FOREACH_PAIR(name, value, i->xattrs) {
                log_debug("Setting extended attribute '%s=%s' on %s.", *name, *value, path);
                if (setxattr(procfs_path, *name, *value, strlen(*value), 0) < 0)
                        return log_error_errno(errno, "Setting extended attribute %s=%s on %s failed: %m",
                                               *name, *value, path);
        }
        return 0;
}

static int path_set_xattrs(Item *i, const char *path) {
        _cleanup_close_ int fd = -1;

        assert(i);
        assert(path);

        fd = path_open_safe(path);
        if (fd < 0)
                return fd;

        return fd_set_xattrs(i, fd, path, NULL);
}

static int parse_acls_from_arg(Item *item) {
#if HAVE_ACL
        int r;

        assert(item);

        /* If append_or_force (= modify) is set, we will not modify the acl
         * afterwards, so the mask can be added now if necessary. */

        r = parse_acl(item->argument, &item->acl_access, &item->acl_default, !item->append_or_force);
        if (r < 0)
                log_warning_errno(r, "Failed to parse ACL \"%s\": %m. Ignoring", item->argument);
#else
        log_warning("ACLs are not supported. Ignoring.");
#endif

        return 0;
}

#if HAVE_ACL
static int path_set_acl(const char *path, const char *pretty, acl_type_t type, acl_t acl, bool modify) {
        _cleanup_(acl_free_charpp) char *t = NULL;
        _cleanup_(acl_freep) acl_t dup = NULL;
        int r;

        /* Returns 0 for success, positive error if already warned,
         * negative error otherwise. */

        if (modify) {
                r = acls_for_file(path, type, acl, &dup);
                if (r < 0)
                        return r;

                r = calc_acl_mask_if_needed(&dup);
                if (r < 0)
                        return r;
        } else {
                dup = acl_dup(acl);
                if (!dup)
                        return -errno;

                /* the mask was already added earlier if needed */
        }

        r = add_base_acls_if_needed(&dup, path);
        if (r < 0)
                return r;

        t = acl_to_any_text(dup, NULL, ',', TEXT_ABBREVIATE);
        log_debug("Setting %s ACL %s on %s.",
                  type == ACL_TYPE_ACCESS ? "access" : "default",
                  strna(t), pretty);

        r = acl_set_file(path, type, dup);
        if (r < 0) {
                if (ERRNO_IS_NOT_SUPPORTED(errno))
                        /* No error if filesystem doesn't support ACLs. Return negative. */
                        return -errno;
                else
                        /* Return positive to indicate we already warned */
                        return -log_error_errno(errno,
                                                "Setting %s ACL \"%s\" on %s failed: %m",
                                                type == ACL_TYPE_ACCESS ? "access" : "default",
                                                strna(t), pretty);
        }
        return 0;
}
#endif

static int fd_set_acls(Item *item, int fd, const char *path, const struct stat *st) {
        int r = 0;
#if HAVE_ACL
        char procfs_path[STRLEN("/proc/self/fd/") + DECIMAL_STR_MAX(int)];
        struct stat stbuf;

        assert(item);
        assert(fd >= 0);
        assert(path);

        if (!st) {
                if (fstat(fd, &stbuf) < 0)
                        return log_error_errno(errno, "fstat(%s) failed: %m", path);
                st = &stbuf;
        }

        if (hardlink_vulnerable(st))
                return log_error_errno(SYNTHETIC_ERRNO(EPERM),
                                       "Refusing to set ACLs on hardlinked file %s while the fs.protected_hardlinks sysctl is turned off.",
                                       path);

        if (S_ISLNK(st->st_mode)) {
                log_debug("Skipping ACL fix for symlink %s.", path);
                return 0;
        }

        xsprintf(procfs_path, "/proc/self/fd/%i", fd);

        if (item->acl_access)
                r = path_set_acl(procfs_path, path, ACL_TYPE_ACCESS, item->acl_access, item->append_or_force);

        /* set only default acls to folders */
        if (r == 0 && item->acl_default && S_ISDIR(st->st_mode))
                r = path_set_acl(procfs_path, path, ACL_TYPE_DEFAULT, item->acl_default, item->append_or_force);

        if (ERRNO_IS_NOT_SUPPORTED(r)) {
                log_debug_errno(r, "ACLs not supported by file system at %s", path);
                return 0;
        }

        if (r > 0)
                return -r; /* already warned */

        /* The above procfs paths don't work if /proc is not mounted. */
        if (r == -ENOENT && proc_mounted() == 0)
                r = -ENOSYS;

        if (r < 0)
                return log_error_errno(r, "ACL operation on \"%s\" failed: %m", path);
#endif
        return r;
}

static int path_set_acls(Item *item, const char *path) {
        int r = 0;
#if HAVE_ACL
        _cleanup_close_ int fd = -1;

        assert(item);
        assert(path);

        fd = path_open_safe(path);
        if (fd < 0)
                return fd;

        r = fd_set_acls(item, fd, path, NULL);
#endif
        return r;
}

static int parse_attribute_from_arg(Item *item) {

        static const struct {
                char character;
                unsigned value;
        } attributes[] = {
                { 'A', FS_NOATIME_FL },      /* do not update atime */
                { 'S', FS_SYNC_FL },         /* Synchronous updates */
                { 'D', FS_DIRSYNC_FL },      /* dirsync behaviour (directories only) */
                { 'a', FS_APPEND_FL },       /* writes to file may only append */
                { 'c', FS_COMPR_FL },        /* Compress file */
                { 'd', FS_NODUMP_FL },       /* do not dump file */
                { 'e', FS_EXTENT_FL },       /* Extents */
                { 'i', FS_IMMUTABLE_FL },    /* Immutable file */
                { 'j', FS_JOURNAL_DATA_FL }, /* Reserved for ext3 */
                { 's', FS_SECRM_FL },        /* Secure deletion */
                { 'u', FS_UNRM_FL },         /* Undelete */
                { 't', FS_NOTAIL_FL },       /* file tail should not be merged */
                { 'T', FS_TOPDIR_FL },       /* Top of directory hierarchies */
                { 'C', FS_NOCOW_FL },        /* Do not cow file */
                { 'P', FS_PROJINHERIT_FL },  /* Inherit the quota project ID */
        };

        enum {
                MODE_ADD,
                MODE_DEL,
                MODE_SET
        } mode = MODE_ADD;

        unsigned value = 0, mask = 0;
        const char *p;

        assert(item);

        p = item->argument;
        if (p) {
                if (*p == '+') {
                        mode = MODE_ADD;
                        p++;
                } else if (*p == '-') {
                        mode = MODE_DEL;
                        p++;
                } else  if (*p == '=') {
                        mode = MODE_SET;
                        p++;
                }
        }

        if (isempty(p) && mode != MODE_SET)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "Setting file attribute on '%s' needs an attribute specification.",
                                       item->path);

        for (; p && *p ; p++) {
                unsigned i, v;

                for (i = 0; i < ELEMENTSOF(attributes); i++)
                        if (*p == attributes[i].character)
                                break;

                if (i >= ELEMENTSOF(attributes))
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                               "Unknown file attribute '%c' on '%s'.",
                                               *p, item->path);

                v = attributes[i].value;

                SET_FLAG(value, v, IN_SET(mode, MODE_ADD, MODE_SET));

                mask |= v;
        }

        if (mode == MODE_SET)
                mask |= CHATTR_ALL_FL;

        assert(mask != 0);

        item->attribute_mask = mask;
        item->attribute_value = value;
        item->attribute_set = true;

        return 0;
}

static int fd_set_attribute(Item *item, int fd, const char *path, const struct stat *st) {
        _cleanup_close_ int procfs_fd = -1;
        struct stat stbuf;
        unsigned f;
        int r;

        assert(item);
        assert(fd >= 0);
        assert(path);

        if (!item->attribute_set || item->attribute_mask == 0)
                return 0;

        if (!st) {
                if (fstat(fd, &stbuf) < 0)
                        return log_error_errno(errno, "fstat(%s) failed: %m", path);
                st = &stbuf;
        }

        /* Issuing the file attribute ioctls on device nodes is not
         * safe, as that will be delivered to the drivers, not the
         * file system containing the device node. */
        if (!S_ISREG(st->st_mode) && !S_ISDIR(st->st_mode))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "Setting file flags is only supported on regular files and directories, cannot set on '%s'.",
                                       path);

        f = item->attribute_value & item->attribute_mask;

        /* Mask away directory-specific flags */
        if (!S_ISDIR(st->st_mode))
                f &= ~FS_DIRSYNC_FL;

        procfs_fd = fd_reopen(fd, O_RDONLY|O_CLOEXEC|O_NOATIME);
        if (procfs_fd < 0)
                return log_error_errno(procfs_fd, "Failed to re-open '%s': %m", path);

        unsigned previous, current;
        r = chattr_full(NULL, procfs_fd, f, item->attribute_mask, &previous, &current, true);
        if (r == -ENOANO)
                log_warning("Cannot set file attributes for '%s', maybe due to incompatibility in specified attributes, "
                            "previous=0x%08x, current=0x%08x, expected=0x%08x, ignoring.",
                            path, previous, current, (previous & ~item->attribute_mask) | (f & item->attribute_mask));
        else if (r < 0)
                log_full_errno(ERRNO_IS_NOT_SUPPORTED(r) ? LOG_DEBUG : LOG_WARNING, r,
                               "Cannot set file attributes for '%s', value=0x%08x, mask=0x%08x, ignoring: %m",
                               path, item->attribute_value, item->attribute_mask);

        return 0;
}

static int path_set_attribute(Item *item, const char *path) {
        _cleanup_close_ int fd = -1;

        if (!item->attribute_set || item->attribute_mask == 0)
                return 0;

        fd = path_open_safe(path);
        if (fd < 0)
                return fd;

        return fd_set_attribute(item, fd, path, NULL);
}

static int write_one_file(Item *i, const char *path) {
        _cleanup_close_ int fd = -1, dir_fd = -1;
        char *bn;
        int r;

        assert(i);
        assert(path);
        assert(i->argument);
        assert(i->type == WRITE_FILE);

        /* Validate the path and keep the fd on the directory for opening the
         * file so we're sure that it can't be changed behind our back. */
        dir_fd = path_open_parent_safe(path);
        if (dir_fd < 0)
                return dir_fd;

        bn = basename(path);

        /* Follows symlinks */
        fd = openat(dir_fd, bn,
                    O_NONBLOCK|O_CLOEXEC|O_WRONLY|O_NOCTTY|(i->append_or_force ? O_APPEND : 0),
                    i->mode);
        if (fd < 0) {
                if (errno == ENOENT) {
                        log_debug_errno(errno, "Not writing missing file \"%s\": %m", path);
                        return 0;
                }

                if (i->allow_failure)
                        return log_debug_errno(errno, "Failed to open file \"%s\", ignoring: %m", path);

                return log_error_errno(errno, "Failed to open file \"%s\": %m", path);
        }

        /* 'w' is allowed to write into any kind of files. */
        log_debug("Writing to \"%s\".", path);

        r = loop_write(fd, i->argument, strlen(i->argument), false);
        if (r < 0)
                return log_error_errno(r, "Failed to write file \"%s\": %m", path);

        return fd_set_perms(i, fd, path, NULL);
}

static int create_file(Item *i, const char *path) {
        _cleanup_close_ int fd = -1, dir_fd = -1;
        struct stat stbuf, *st = NULL;
        int r = 0;
        char *bn;

        assert(i);
        assert(path);
        assert(i->type == CREATE_FILE);

        /* 'f' operates on regular files exclusively. */

        /* Validate the path and keep the fd on the directory for opening the
         * file so we're sure that it can't be changed behind our back. */
        dir_fd = path_open_parent_safe(path);
        if (dir_fd < 0)
                return dir_fd;

        bn = basename(path);

        RUN_WITH_UMASK(0000) {
                mac_selinux_create_file_prepare(path, S_IFREG);
                fd = openat(dir_fd, bn, O_CREAT|O_EXCL|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC|O_WRONLY|O_NOCTTY, i->mode);
                mac_selinux_create_file_clear();
        }

        if (fd < 0) {
                /* Even on a read-only filesystem, open(2) returns EEXIST if the
                 * file already exists. It returns EROFS only if it needs to
                 * create the file. */
                if (errno != EEXIST)
                        return log_error_errno(errno, "Failed to create file %s: %m", path);

                /* Re-open the file. At that point it must exist since open(2)
                 * failed with EEXIST. We still need to check if the perms/mode
                 * need to be changed. For read-only filesystems, we let
                 * fd_set_perms() report the error if the perms need to be
                 * modified. */
                fd = openat(dir_fd, bn, O_NOFOLLOW|O_CLOEXEC|O_PATH, i->mode);
                if (fd < 0)
                        return log_error_errno(errno, "Failed to re-open file %s: %m", path);

                if (fstat(fd, &stbuf) < 0)
                        return log_error_errno(errno, "stat(%s) failed: %m", path);

                if (!S_ISREG(stbuf.st_mode))
                        return log_error_errno(SYNTHETIC_ERRNO(EEXIST),
                                               "%s exists and is not a regular file.",
                                               path);

                st = &stbuf;
        } else {

                log_debug("\"%s\" has been created.", path);

                if (i->argument) {
                        log_debug("Writing to \"%s\".", path);

                        r = loop_write(fd, i->argument, strlen(i->argument), false);
                        if (r < 0)
                                return log_error_errno(r, "Failed to write file \"%s\": %m", path);
                }
        }

        return fd_set_perms(i, fd, path, st);
}

static int truncate_file(Item *i, const char *path) {
        _cleanup_close_ int fd = -1, dir_fd = -1;
        struct stat stbuf, *st = NULL;
        bool erofs = false;
        int r = 0;
        char *bn;

        assert(i);
        assert(path);
        assert(i->type == TRUNCATE_FILE || (i->type == CREATE_FILE && i->append_or_force));

        /* We want to operate on regular file exclusively especially since
         * O_TRUNC is unspecified if the file is neither a regular file nor a
         * fifo nor a terminal device. Therefore we first open the file and make
         * sure it's a regular one before truncating it. */

        /* Validate the path and keep the fd on the directory for opening the
         * file so we're sure that it can't be changed behind our back. */
        dir_fd = path_open_parent_safe(path);
        if (dir_fd < 0)
                return dir_fd;

        bn = basename(path);

        RUN_WITH_UMASK(0000) {
                mac_selinux_create_file_prepare(path, S_IFREG);
                fd = openat(dir_fd, bn, O_CREAT|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC|O_WRONLY|O_NOCTTY, i->mode);
                mac_selinux_create_file_clear();
        }

        if (fd < 0) {
                if (errno != EROFS)
                        return log_error_errno(errno, "Failed to open/create file %s: %m", path);

                /* On a read-only filesystem, we don't want to fail if the
                 * target is already empty and the perms are set. So we still
                 * proceed with the sanity checks and let the remaining
                 * operations fail with EROFS if they try to modify the target
                 * file. */

                fd = openat(dir_fd, bn, O_NOFOLLOW|O_CLOEXEC|O_PATH, i->mode);
                if (fd < 0) {
                        if (errno == ENOENT)
                                return log_error_errno(SYNTHETIC_ERRNO(EROFS),
                                                       "Cannot create file %s on a read-only file system.",
                                                       path);

                        return log_error_errno(errno, "Failed to re-open file %s: %m", path);
                }

                erofs = true;
        }

        if (fstat(fd, &stbuf) < 0)
                return log_error_errno(errno, "stat(%s) failed: %m", path);

        if (!S_ISREG(stbuf.st_mode))
                return log_error_errno(SYNTHETIC_ERRNO(EEXIST),
                                       "%s exists and is not a regular file.",
                                       path);

        if (stbuf.st_size > 0) {
                if (ftruncate(fd, 0) < 0) {
                        r = erofs ? -EROFS : -errno;
                        return log_error_errno(r, "Failed to truncate file %s: %m", path);
                }
        } else
                st = &stbuf;

        log_debug("\"%s\" has been created.", path);

        if (i->argument) {
                log_debug("Writing to \"%s\".", path);

                r = loop_write(fd, i->argument, strlen(i->argument), false);
                if (r < 0) {
                        r = erofs ? -EROFS : r;
                        return log_error_errno(r, "Failed to write file %s: %m", path);
                }
        }

        return fd_set_perms(i, fd, path, st);
}

static int copy_files(Item *i) {
        _cleanup_close_ int dfd = -1, fd = -1;
        char *bn;
        int r;

        log_debug("Copying tree \"%s\" to \"%s\".", i->argument, i->path);

        bn = basename(i->path);

        /* Validate the path and use the returned directory fd for copying the
         * target so we're sure that the path can't be changed behind our
         * back. */
        dfd = path_open_parent_safe(i->path);
        if (dfd < 0)
                return dfd;

        r = copy_tree_at(AT_FDCWD, i->argument,
                         dfd, bn,
                         i->uid_set ? i->uid : UID_INVALID,
                         i->gid_set ? i->gid : GID_INVALID,
                         COPY_REFLINK | COPY_MERGE_EMPTY | COPY_MAC_CREATE | COPY_HARDLINKS);
        if (r < 0) {
                struct stat a, b;

                /* If the target already exists on read-only filesystems, trying
                 * to create the target will not fail with EEXIST but with
                 * EROFS. */
                if (r == -EROFS && faccessat(dfd, bn, F_OK, AT_SYMLINK_NOFOLLOW) == 0)
                        r = -EEXIST;

                if (r != -EEXIST)
                        return log_error_errno(r, "Failed to copy files to %s: %m", i->path);

                if (stat(i->argument, &a) < 0)
                        return log_error_errno(errno, "stat(%s) failed: %m", i->argument);

                if (fstatat(dfd, bn, &b, AT_SYMLINK_NOFOLLOW) < 0)
                        return log_error_errno(errno, "stat(%s) failed: %m", i->path);

                if ((a.st_mode ^ b.st_mode) & S_IFMT) {
                        log_debug("Can't copy to %s, file exists already and is of different type", i->path);
                        return 0;
                }
        }

        fd = openat(dfd, bn, O_NOFOLLOW|O_CLOEXEC|O_PATH);
        if (fd < 0)
                return log_error_errno(errno, "Failed to openat(%s): %m", i->path);

        return fd_set_perms(i, fd, i->path, NULL);
}

typedef enum {
        CREATION_NORMAL,
        CREATION_EXISTING,
        CREATION_FORCE,
        _CREATION_MODE_MAX,
        _CREATION_MODE_INVALID = -EINVAL,
} CreationMode;

static const char *const creation_mode_verb_table[_CREATION_MODE_MAX] = {
        [CREATION_NORMAL] = "Created",
        [CREATION_EXISTING] = "Found existing",
        [CREATION_FORCE] = "Created replacement",
};

DEFINE_PRIVATE_STRING_TABLE_LOOKUP_TO_STRING(creation_mode_verb, CreationMode);

static int create_directory_or_subvolume(const char *path, mode_t mode, bool subvol, CreationMode *creation) {
        _cleanup_close_ int pfd = -1;
        CreationMode c;
        int r;

        assert(path);

        if (!creation)
                creation = &c;

        pfd = path_open_parent_safe(path);
        if (pfd < 0)
                return pfd;

        if (subvol) {
                r = getenv_bool("SYSTEMD_TMPFILES_FORCE_SUBVOL");
                if (r < 0) {
                        if (r != -ENXIO) /* env var is unset */
                                log_warning_errno(r, "Cannot parse value of $SYSTEMD_TMPFILES_FORCE_SUBVOL, ignoring.");
                        r = btrfs_is_subvol(empty_to_root(arg_root)) > 0;
                }
                if (!r)
                        /* Don't create a subvolume unless the root directory is one, too. We do this under
                         * the assumption that if the root directory is just a plain directory (i.e. very
                         * light-weight), we shouldn't try to split it up into subvolumes (i.e. more
                         * heavy-weight). Thus, chroot() environments and suchlike will get a full brtfs
                         * subvolume set up below their tree only if they specifically set up a btrfs
                         * subvolume for the root dir too. */

                        subvol = false;
                else {
                        RUN_WITH_UMASK((~mode) & 0777)
                                r = btrfs_subvol_make_fd(pfd, basename(path));
                }
        } else
                r = 0;

        if (!subvol || r == -ENOTTY)
                RUN_WITH_UMASK(0000)
                        r = mkdirat_label(pfd, basename(path), mode);

        if (r < 0) {
                int k;

                if (!IN_SET(r, -EEXIST, -EROFS))
                        return log_error_errno(r, "Failed to create directory or subvolume \"%s\": %m", path);

                k = is_dir_full(pfd, basename(path), /* follow= */ false);
                if (k == -ENOENT && r == -EROFS)
                        return log_error_errno(r, "%s does not exist and cannot be created as the file system is read-only.", path);
                if (k < 0)
                        return log_error_errno(k, "Failed to check if %s exists: %m", path);
                if (!k)
                        return log_warning_errno(SYNTHETIC_ERRNO(EEXIST),
                                                 "\"%s\" already exists and is not a directory.", path);

                *creation = CREATION_EXISTING;
        } else
                *creation = CREATION_NORMAL;

        log_debug("%s directory \"%s\".", creation_mode_verb_to_string(*creation), path);

        r = openat(pfd, basename(path), O_NOCTTY|O_CLOEXEC|O_DIRECTORY);
        if (r < 0)
                return log_error_errno(errno, "Failed to open directory '%s': %m", basename(path));

        return r;
}

static int create_directory(Item *i, const char *path) {
        _cleanup_close_ int fd = -1;

        assert(i);
        assert(IN_SET(i->type, CREATE_DIRECTORY, TRUNCATE_DIRECTORY));

        fd = create_directory_or_subvolume(path, i->mode, false, NULL);
        if (fd == -EEXIST)
                return 0;
        if (fd < 0)
                return fd;

        return fd_set_perms(i, fd, path, NULL);
}

static int create_subvolume(Item *i, const char *path) {
        _cleanup_close_ int fd = -1;
        CreationMode creation;
        int r, q = 0;

        assert(i);
        assert(IN_SET(i->type, CREATE_SUBVOLUME, CREATE_SUBVOLUME_NEW_QUOTA, CREATE_SUBVOLUME_INHERIT_QUOTA));

        fd = create_directory_or_subvolume(path, i->mode, true, &creation);
        if (fd == -EEXIST)
                return 0;
        if (fd < 0)
                return fd;

        if (creation == CREATION_NORMAL &&
            IN_SET(i->type, CREATE_SUBVOLUME_NEW_QUOTA, CREATE_SUBVOLUME_INHERIT_QUOTA)) {
                r = btrfs_subvol_auto_qgroup_fd(fd, 0, i->type == CREATE_SUBVOLUME_NEW_QUOTA);
                if (r == -ENOTTY)
                        log_debug_errno(r, "Couldn't adjust quota for subvolume \"%s\" (unsupported fs or dir not a subvolume): %m", i->path);
                else if (r == -EROFS)
                        log_debug_errno(r, "Couldn't adjust quota for subvolume \"%s\" (fs is read-only).", i->path);
                else if (r == -ENOTCONN)
                        log_debug_errno(r, "Couldn't adjust quota for subvolume \"%s\" (quota support is disabled).", i->path);
                else if (r < 0)
                        q = log_error_errno(r, "Failed to adjust quota for subvolume \"%s\": %m", i->path);
                else if (r > 0)
                        log_debug("Adjusted quota for subvolume \"%s\".", i->path);
                else if (r == 0)
                        log_debug("Quota for subvolume \"%s\" already in place, no change made.", i->path);
        }

        r = fd_set_perms(i, fd, path, NULL);
        if (q < 0) /* prefer the quota change error from above */
                return q;

        return r;
}

static int empty_directory(Item *i, const char *path) {
        int r;

        assert(i);
        assert(i->type == EMPTY_DIRECTORY);

        r = is_dir(path, false);
        if (r == -ENOENT) {
                /* Option "e" operates only on existing objects. Do not
                 * print errors about non-existent files or directories */
                log_debug("Skipping missing directory: %s", path);
                return 0;
        }
        if (r < 0)
                return log_error_errno(r, "is_dir() failed on path %s: %m", path);
        if (r == 0) {
                log_warning("\"%s\" already exists and is not a directory.", path);
                return 0;
        }

        return path_set_perms(i, path);
}

static int create_device(Item *i, mode_t file_type) {
        _cleanup_close_ int dfd = -1, fd = -1;
        CreationMode creation;
        char *bn;
        int r;

        assert(i);
        assert(IN_SET(file_type, S_IFBLK, S_IFCHR));

        bn = basename(i->path);

        /* Validate the path and use the returned directory fd for copying the
         * target so we're sure that the path can't be changed behind our
         * back. */
        dfd = path_open_parent_safe(i->path);
        if (dfd < 0)
                return dfd;

        RUN_WITH_UMASK(0000) {
                mac_selinux_create_file_prepare(i->path, file_type);
                r = mknodat(dfd, bn, i->mode | file_type, i->major_minor);
                mac_selinux_create_file_clear();
        }

        if (r < 0) {
                struct stat st;

                if (errno == EPERM) {
                        log_debug("We lack permissions, possibly because of cgroup configuration; "
                                  "skipping creation of device node %s.", i->path);
                        return 0;
                }

                if (errno != EEXIST)
                        return log_error_errno(errno, "Failed to create device node %s: %m", i->path);

                if (fstatat(dfd, bn, &st, 0) < 0)
                        return log_error_errno(errno, "stat(%s) failed: %m", i->path);

                if ((st.st_mode & S_IFMT) != file_type) {

                        if (i->append_or_force) {

                                RUN_WITH_UMASK(0000) {
                                        mac_selinux_create_file_prepare(i->path, file_type);
                                        /* FIXME: need to introduce mknodat_atomic() */
                                        r = mknod_atomic(i->path, i->mode | file_type, i->major_minor);
                                        mac_selinux_create_file_clear();
                                }

                                if (r < 0)
                                        return log_error_errno(r, "Failed to create device node \"%s\": %m", i->path);
                                creation = CREATION_FORCE;
                        } else {
                                log_warning("\"%s\" already exists is not a device node.", i->path);
                                return 0;
                        }
                } else
                        creation = CREATION_EXISTING;
        } else
                creation = CREATION_NORMAL;

        log_debug("%s %s device node \"%s\" %u:%u.",
                  creation_mode_verb_to_string(creation),
                  i->type == CREATE_BLOCK_DEVICE ? "block" : "char",
                  i->path, major(i->mode), minor(i->mode));

        fd = openat(dfd, bn, O_NOFOLLOW|O_CLOEXEC|O_PATH);
        if (fd < 0)
                return log_error_errno(errno, "Failed to openat(%s): %m", i->path);

        return fd_set_perms(i, fd, i->path, NULL);
}

static int create_fifo(Item *i, const char *path) {
        _cleanup_close_ int pfd = -1, fd = -1;
        CreationMode creation;
        struct stat st;
        char *bn;
        int r;

        pfd = path_open_parent_safe(path);
        if (pfd < 0)
                return pfd;

        bn = basename(path);

        RUN_WITH_UMASK(0000) {
                mac_selinux_create_file_prepare(path, S_IFIFO);
                r = mkfifoat(pfd, bn, i->mode);
                mac_selinux_create_file_clear();
        }

        if (r < 0) {
                if (errno != EEXIST)
                        return log_error_errno(errno, "Failed to create fifo %s: %m", path);

                if (fstatat(pfd, bn, &st, AT_SYMLINK_NOFOLLOW) < 0)
                        return log_error_errno(errno, "stat(%s) failed: %m", path);

                if (!S_ISFIFO(st.st_mode)) {

                        if (i->append_or_force) {
                                RUN_WITH_UMASK(0000) {
                                        mac_selinux_create_file_prepare(path, S_IFIFO);
                                        r = mkfifoat_atomic(pfd, bn, i->mode);
                                        mac_selinux_create_file_clear();
                                }

                                if (r < 0)
                                        return log_error_errno(r, "Failed to create fifo %s: %m", path);
                                creation = CREATION_FORCE;
                        } else {
                                log_warning("\"%s\" already exists and is not a fifo.", path);
                                return 0;
                        }
                } else
                        creation = CREATION_EXISTING;
        } else
                creation = CREATION_NORMAL;

        log_debug("%s fifo \"%s\".", creation_mode_verb_to_string(creation), path);

        fd = openat(pfd, bn, O_NOFOLLOW|O_CLOEXEC|O_PATH);
        if (fd < 0)
                return log_error_errno(errno, "Failed to openat(%s): %m", path);

        return fd_set_perms(i, fd, i->path, NULL);
}

typedef int (*action_t)(Item *i, const char *path);
typedef int (*fdaction_t)(Item *i, int fd, const char *path, const struct stat *st);

static int item_do(Item *i, int fd, const char *path, fdaction_t action) {
        struct stat st;
        int r = 0, q;

        assert(i);
        assert(path);
        assert(fd >= 0);

        if (fstat(fd, &st) < 0) {
                r = log_error_errno(errno, "fstat() on file failed: %m");
                goto finish;
        }

        /* This returns the first error we run into, but nevertheless
         * tries to go on */
        r = action(i, fd, path, &st);

        if (S_ISDIR(st.st_mode)) {
                char procfs_path[STRLEN("/proc/self/fd/") + DECIMAL_STR_MAX(int)];
                _cleanup_closedir_ DIR *d = NULL;
                struct dirent *de;

                /* The passed 'fd' was opened with O_PATH. We need to convert
                 * it into a 'regular' fd before reading the directory content. */
                xsprintf(procfs_path, "/proc/self/fd/%i", fd);

                d = opendir(procfs_path);
                if (!d) {
                        log_error_errno(errno, "Failed to opendir() '%s': %m", procfs_path);
                        if (r == 0)
                                r = -errno;
                        goto finish;
                }

                FOREACH_DIRENT_ALL(de, d, q = -errno; goto finish) {
                        int de_fd;

                        if (dot_or_dot_dot(de->d_name))
                                continue;

                        de_fd = openat(fd, de->d_name, O_NOFOLLOW|O_CLOEXEC|O_PATH);
                        if (de_fd < 0)
                                q = log_error_errno(errno, "Failed to open() file '%s': %m", de->d_name);
                        else {
                                _cleanup_free_ char *de_path = NULL;

                                de_path = path_join(path, de->d_name);
                                if (!de_path)
                                        q = log_oom();
                                else
                                        /* Pass ownership of dirent fd over */
                                        q = item_do(i, de_fd, de_path, action);
                        }

                        if (q < 0 && r == 0)
                                r = q;
                }
        }
finish:
        safe_close(fd);
        return r;
}

static int glob_item(Item *i, action_t action) {
        _cleanup_globfree_ glob_t g = {
                .gl_opendir = (void *(*)(const char *)) opendir_nomod,
        };
        int r = 0, k;
        char **fn;

        k = safe_glob(i->path, GLOB_NOSORT|GLOB_BRACE, &g);
        if (k < 0 && k != -ENOENT)
                return log_error_errno(k, "glob(%s) failed: %m", i->path);

        STRV_FOREACH(fn, g.gl_pathv) {
                k = action(i, *fn);
                if (k < 0 && r == 0)
                        r = k;
        }

        return r;
}

static int glob_item_recursively(Item *i, fdaction_t action) {
        _cleanup_globfree_ glob_t g = {
                .gl_opendir = (void *(*)(const char *)) opendir_nomod,
        };
        int r = 0, k;
        char **fn;

        k = safe_glob(i->path, GLOB_NOSORT|GLOB_BRACE, &g);
        if (k < 0 && k != -ENOENT)
                return log_error_errno(k, "glob(%s) failed: %m", i->path);

        STRV_FOREACH(fn, g.gl_pathv) {
                _cleanup_close_ int fd = -1;

                /* Make sure we won't trigger/follow file object (such as
                 * device nodes, automounts, ...) pointed out by 'fn' with
                 * O_PATH. Note, when O_PATH is used, flags other than
                 * O_CLOEXEC, O_DIRECTORY, and O_NOFOLLOW are ignored. */

                fd = open(*fn, O_CLOEXEC|O_NOFOLLOW|O_PATH);
                if (fd < 0) {
                        log_error_errno(errno, "Opening '%s' failed: %m", *fn);
                        if (r == 0)
                                r = -errno;
                        continue;
                }

                k = item_do(i, fd, *fn, action);
                if (k < 0 && r == 0)
                        r = k;

                /* we passed fd ownership to the previous call */
                fd = -1;
        }

        return r;
}

static int rm_if_wrong_type_safe(
                mode_t mode,
                int parent_fd,
                const struct stat *parent_st, /* Only used if follow_links below is true. */
                const char *name,
                int flags) {
        _cleanup_free_ char *parent_name = NULL;
        bool follow_links = !FLAGS_SET(flags, AT_SYMLINK_NOFOLLOW);
        struct stat st;
        int r;

        assert(name);
        assert((mode & ~S_IFMT) == 0);
        assert(!follow_links || parent_st);
        assert((flags & ~AT_SYMLINK_NOFOLLOW) == 0);

        if (!filename_is_valid(name))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "\"%s\" is not a valid filename.", name);

        r = fstatat_harder(parent_fd, name, &st, flags, REMOVE_CHMOD | REMOVE_CHMOD_RESTORE);
        if (r < 0) {
                (void) fd_get_path(parent_fd, &parent_name);
                return log_full_errno(r == -ENOENT? LOG_DEBUG : LOG_ERR, r,
                              "Failed to stat \"%s\" at \"%s\": %m", name, strna(parent_name));
        }

        /* Fail before removing anything if this is an unsafe transition. */
        if (follow_links && unsafe_transition(parent_st, &st)) {
                (void) fd_get_path(parent_fd, &parent_name);
                return log_error_errno(SYNTHETIC_ERRNO(ENOLINK),
                                "Unsafe transition from \"%s\" to \"%s\".", parent_name, name);
        }

        if ((st.st_mode & S_IFMT) == mode)
                return 0;

        (void) fd_get_path(parent_fd, &parent_name);
        log_notice("Wrong file type 0x%x; rm -rf \"%s/%s\"", st.st_mode & S_IFMT, strna(parent_name), name);

        /* If the target of the symlink was the wrong type, the link needs to be removed instead of the
         * target, so make sure it is identified as a link and not a directory. */
        if (follow_links) {
                r = fstatat_harder(parent_fd, name, &st, AT_SYMLINK_NOFOLLOW, REMOVE_CHMOD | REMOVE_CHMOD_RESTORE);
                if (r < 0)
                        return log_error_errno(r, "Failed to stat \"%s\" at \"%s\": %m", name, strna(parent_name));
        }

        /* Do not remove mount points. */
        r = fd_is_mount_point(parent_fd, name, follow_links ? AT_SYMLINK_FOLLOW : 0);
        if (r < 0)
                (void) log_warning_errno(r, "Failed to check if  \"%s/%s\" is a mount point: %m; Continuing",
                                strna(parent_name), name);
        else if (r > 0)
                return log_error_errno(SYNTHETIC_ERRNO(EBUSY),
                                "Not removing  \"%s/%s\" because it is a mount point.", strna(parent_name), name);

        if ((st.st_mode & S_IFMT) == S_IFDIR) {
                _cleanup_close_ int child_fd = -1;

                child_fd = openat(parent_fd, name, O_NOCTTY | O_CLOEXEC | O_DIRECTORY);
                if (child_fd < 0)
                        return log_error_errno(errno, "Failed to open \"%s\" at \"%s\": %m", name, strna(parent_name));

                r = rm_rf_children(TAKE_FD(child_fd), REMOVE_ROOT|REMOVE_SUBVOLUME|REMOVE_PHYSICAL, &st);
                if (r < 0)
                        return log_error_errno(r, "Failed to remove contents of \"%s\" at \"%s\": %m", name, strna(parent_name));

                r = unlinkat_harder(parent_fd, name, AT_REMOVEDIR, REMOVE_CHMOD | REMOVE_CHMOD_RESTORE);
        } else
                r = unlinkat_harder(parent_fd, name, 0, REMOVE_CHMOD | REMOVE_CHMOD_RESTORE);
        if (r < 0)
                return log_error_errno(r, "Failed to remove \"%s\" at \"%s\": %m", name, strna(parent_name));

        /* This is covered by the log_notice "Wrong file type..." It is logged earlier because it gives
         * context to other error messages that might follow. */
        return -ENOENT;
}

/* If child_mode is non-zero, rm_if_wrong_type_safe will be executed for the last path component. */
static int mkdir_parents_rm_if_wrong_type(mode_t child_mode, const char *path) {
        _cleanup_close_ int parent_fd = -1;
        struct stat parent_st;
        size_t path_len;
        int r;

        assert(path);
        assert((child_mode & ~S_IFMT) == 0);

        path_len = strlen(path);

        if (!is_path(path))
                /* rm_if_wrong_type_safe already logs errors. */
                return child_mode != 0 ? rm_if_wrong_type_safe(child_mode, AT_FDCWD, NULL, path, AT_SYMLINK_NOFOLLOW) : 0;

        if (child_mode != 0 && endswith(path, "/"))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                "Trailing path separators are only allowed if child_mode is not set; got \"%s\"", path);

        /* Get the parent_fd and stat. */
        parent_fd = openat(AT_FDCWD, path_is_absolute(path) ? "/" : ".", O_NOCTTY | O_CLOEXEC | O_DIRECTORY);
        if (parent_fd < 0)
                return log_error_errno(errno, "Failed to open root: %m");

        if (fstat(parent_fd, &parent_st) < 0)
                return log_error_errno(errno, "Failed to stat root: %m");

        /* Check every parent directory in the path, except the last component */
        for (const char *e = path;;) {
                _cleanup_close_ int next_fd = -1;
                char t[path_len + 1];
                const char *s;

                /* Find the start of the next path component. */
                s = e + strspn(e, "/");
                /* Find the end of the next path component. */
                e = s + strcspn(s, "/");

                /* Copy the path component to t so it can be a null terminated string. */
                *((char*) mempcpy(t, s, e - s)) = 0;

                /* Is this the last component? If so, then check the type */
                if (*e == 0)
                        return child_mode != 0 ? rm_if_wrong_type_safe(child_mode, parent_fd, &parent_st, t, AT_SYMLINK_NOFOLLOW) : 0;

                r = rm_if_wrong_type_safe(S_IFDIR, parent_fd, &parent_st, t, 0);
                /* Remove dangling symlinks. */
                if (r == -ENOENT)
                        r = rm_if_wrong_type_safe(S_IFDIR, parent_fd, &parent_st, t, AT_SYMLINK_NOFOLLOW);
                if (r == -ENOENT) {
                        RUN_WITH_UMASK(0000)
                                r = mkdirat_label(parent_fd, t, 0755);
                        if (r < 0) {
                                _cleanup_free_ char *parent_name = NULL;

                                (void) fd_get_path(parent_fd, &parent_name);
                                return log_error_errno(r, "Failed to mkdir \"%s\" at \"%s\": %m", t, strnull(parent_name));
                        }
                } else if (r < 0)
                        /* rm_if_wrong_type_safe already logs errors. */
                        return r;

                next_fd = openat(parent_fd, t, O_NOCTTY | O_CLOEXEC | O_DIRECTORY);
                if (next_fd < 0) {
                        _cleanup_free_ char *parent_name = NULL;

                        r = -errno;
                        (void) fd_get_path(parent_fd, &parent_name);
                        return log_error_errno(r, "Failed to open \"%s\" at \"%s\": %m", t, strnull(parent_name));
                }
                if (fstat(next_fd, &parent_st) < 0) {
                        _cleanup_free_ char *parent_name = NULL;

                        r = -errno;
                        (void) fd_get_path(parent_fd, &parent_name);
                        return log_error_errno(r, "Failed to stat \"%s\" at \"%s\": %m", t, strnull(parent_name));
                }

                CLOSE_AND_REPLACE(parent_fd, next_fd);
        }
}

static int mkdir_parents_item(Item *i, mode_t child_mode) {
        int r;
        if (i->try_replace) {
                r = mkdir_parents_rm_if_wrong_type(child_mode, i->path);
                if (r < 0 && r != -ENOENT)
                        return r;
        } else
                RUN_WITH_UMASK(0000)
                        (void) mkdir_parents_label(i->path, 0755);

        return 0;
}

static int create_item(Item *i) {
        CreationMode creation;
        int r = 0;

        assert(i);

        log_debug("Running create action for entry %c %s", (char) i->type, i->path);

        switch (i->type) {

        case IGNORE_PATH:
        case IGNORE_DIRECTORY_PATH:
        case REMOVE_PATH:
        case RECURSIVE_REMOVE_PATH:
                return 0;

        case TRUNCATE_FILE:
        case CREATE_FILE:
                r = mkdir_parents_item(i, S_IFREG);
                if (r < 0)
                        return r;

                if ((i->type == CREATE_FILE && i->append_or_force) || i->type == TRUNCATE_FILE)
                        r = truncate_file(i, i->path);
                else
                        r = create_file(i, i->path);

                if (r < 0)
                        return r;
                break;

        case COPY_FILES:
                r = mkdir_parents_item(i, 0);
                if (r < 0)
                        return r;

                r = copy_files(i);
                if (r < 0)
                        return r;
                break;

        case WRITE_FILE:
                r = glob_item(i, write_one_file);
                if (r < 0)
                        return r;

                break;

        case CREATE_DIRECTORY:
        case TRUNCATE_DIRECTORY:
                r = mkdir_parents_item(i, S_IFDIR);
                if (r < 0)
                        return r;

                r = create_directory(i, i->path);
                if (r < 0)
                        return r;
                break;

        case CREATE_SUBVOLUME:
        case CREATE_SUBVOLUME_INHERIT_QUOTA:
        case CREATE_SUBVOLUME_NEW_QUOTA:
                r = mkdir_parents_item(i, S_IFDIR);
                if (r < 0)
                        return r;

                r = create_subvolume(i, i->path);
                if (r < 0)
                        return r;
                break;

        case EMPTY_DIRECTORY:
                r = glob_item(i, empty_directory);
                if (r < 0)
                        return r;
                break;

        case CREATE_FIFO:
                r = mkdir_parents_item(i, S_IFIFO);
                if (r < 0)
                        return r;

                r = create_fifo(i, i->path);
                if (r < 0)
                        return r;
                break;

        case CREATE_SYMLINK: {
                r = mkdir_parents_item(i, S_IFLNK);
                if (r < 0)
                        return r;

                mac_selinux_create_file_prepare(i->path, S_IFLNK);
                r = symlink(i->argument, i->path);
                mac_selinux_create_file_clear();

                if (r < 0) {
                        _cleanup_free_ char *x = NULL;

                        if (errno != EEXIST)
                                return log_error_errno(errno, "symlink(%s, %s) failed: %m", i->argument, i->path);

                        r = readlink_malloc(i->path, &x);
                        if (r < 0 || !streq(i->argument, x)) {

                                if (i->append_or_force) {
                                        mac_selinux_create_file_prepare(i->path, S_IFLNK);
                                        r = symlink_atomic(i->argument, i->path);
                                        mac_selinux_create_file_clear();

                                        if (IN_SET(r, -EISDIR, -EEXIST, -ENOTEMPTY)) {
                                                r = rm_rf(i->path, REMOVE_ROOT|REMOVE_PHYSICAL);
                                                if (r < 0)
                                                        return log_error_errno(r, "rm -fr %s failed: %m", i->path);

                                                mac_selinux_create_file_prepare(i->path, S_IFLNK);
                                                r = symlink(i->argument, i->path) < 0 ? -errno : 0;
                                                mac_selinux_create_file_clear();
                                        }
                                        if (r < 0)
                                                return log_error_errno(r, "symlink(%s, %s) failed: %m", i->argument, i->path);

                                        creation = CREATION_FORCE;
                                } else {
                                        log_debug("\"%s\" is not a symlink or does not point to the correct path.", i->path);
                                        return 0;
                                }
                        } else
                                creation = CREATION_EXISTING;
                } else

                        creation = CREATION_NORMAL;
                log_debug("%s symlink \"%s\".", creation_mode_verb_to_string(creation), i->path);
                break;
        }

        case CREATE_BLOCK_DEVICE:
        case CREATE_CHAR_DEVICE:
                if (have_effective_cap(CAP_MKNOD) == 0) {
                        /* In a container we lack CAP_MKNOD. We shouldn't attempt to create the device node in that
                         * case to avoid noise, and we don't support virtualized devices in containers anyway. */

                        log_debug("We lack CAP_MKNOD, skipping creation of device node %s.", i->path);
                        return 0;
                }

                r = mkdir_parents_item(i, i->type == CREATE_BLOCK_DEVICE ? S_IFBLK : S_IFCHR);
                if (r < 0)
                        return r;

                r = create_device(i, i->type == CREATE_BLOCK_DEVICE ? S_IFBLK : S_IFCHR);
                if (r < 0)
                        return r;

                break;

        case ADJUST_MODE:
        case RELABEL_PATH:
                r = glob_item(i, path_set_perms);
                if (r < 0)
                        return r;
                break;

        case RECURSIVE_RELABEL_PATH:
                r = glob_item_recursively(i, fd_set_perms);
                if (r < 0)
                        return r;
                break;

        case SET_XATTR:
                r = glob_item(i, path_set_xattrs);
                if (r < 0)
                        return r;
                break;

        case RECURSIVE_SET_XATTR:
                r = glob_item_recursively(i, fd_set_xattrs);
                if (r < 0)
                        return r;
                break;

        case SET_ACL:
                r = glob_item(i, path_set_acls);
                if (r < 0)
                        return r;
                break;

        case RECURSIVE_SET_ACL:
                r = glob_item_recursively(i, fd_set_acls);
                if (r < 0)
                        return r;
                break;

        case SET_ATTRIBUTE:
                r = glob_item(i, path_set_attribute);
                if (r < 0)
                        return r;
                break;

        case RECURSIVE_SET_ATTRIBUTE:
                r = glob_item_recursively(i, fd_set_attribute);
                if (r < 0)
                        return r;
                break;
        }

        return 0;
}

static int remove_item_instance(Item *i, const char *instance) {
        int r;

        assert(i);

        switch (i->type) {

        case REMOVE_PATH:
                if (remove(instance) < 0 && errno != ENOENT)
                        return log_error_errno(errno, "rm(%s): %m", instance);

                break;

        case RECURSIVE_REMOVE_PATH:
                /* FIXME: we probably should use dir_cleanup() here instead of rm_rf() so that 'x' is honoured. */
                log_debug("rm -rf \"%s\"", instance);
                r = rm_rf(instance, REMOVE_ROOT|REMOVE_SUBVOLUME|REMOVE_PHYSICAL);
                if (r < 0 && r != -ENOENT)
                        return log_error_errno(r, "rm_rf(%s): %m", instance);

                break;

        default:
                assert_not_reached("wut?");
        }

        return 0;
}

static int remove_item(Item *i) {
        int r;

        assert(i);

        log_debug("Running remove action for entry %c %s", (char) i->type, i->path);

        switch (i->type) {

        case TRUNCATE_DIRECTORY:
                /* FIXME: we probably should use dir_cleanup() here instead of rm_rf() so that 'x' is honoured. */
                log_debug("rm -rf \"%s\"", i->path);
                r = rm_rf(i->path, REMOVE_PHYSICAL);
                if (r < 0 && r != -ENOENT)
                        return log_error_errno(r, "rm_rf(%s): %m", i->path);

                return 0;

        case REMOVE_PATH:
        case RECURSIVE_REMOVE_PATH:
                return glob_item(i, remove_item_instance);

        default:
                return 0;
        }
}

static char *age_by_to_string(AgeBy ab, bool is_dir) {
        static const char ab_map[] = { 'a', 'b', 'c', 'm' };
        size_t j = 0;
        char *ret;

        ret = new(char, ELEMENTSOF(ab_map) + 1);
        if (!ret)
                return NULL;

        for (size_t i = 0; i < ELEMENTSOF(ab_map); i++)
                if (FLAGS_SET(ab, 1U << i))
                        ret[j++] = is_dir ? ascii_toupper(ab_map[i]) : ab_map[i];

        ret[j] = 0;
        return ret;
}

static int clean_item_instance(Item *i, const char* instance) {
        char timestamp[FORMAT_TIMESTAMP_MAX];
        _cleanup_closedir_ DIR *d = NULL;
        STRUCT_STATX_DEFINE(sx);
        int mountpoint, r;
        usec_t cutoff, n;

        assert(i);

        if (!i->age_set)
                return 0;

        n = now(CLOCK_REALTIME);
        if (n < i->age)
                return 0;

        cutoff = n - i->age;

        d = opendir_nomod(instance);
        if (!d) {
                if (IN_SET(errno, ENOENT, ENOTDIR)) {
                        log_debug_errno(errno, "Directory \"%s\": %m", instance);
                        return 0;
                }

                return log_error_errno(errno, "Failed to open directory %s: %m", instance);
        }

        r = statx_fallback(dirfd(d), "", AT_EMPTY_PATH, STATX_MODE|STATX_INO|STATX_ATIME|STATX_MTIME, &sx);
        if (r < 0)
                return log_error_errno(r, "statx(%s) failed: %m", instance);

        if (FLAGS_SET(sx.stx_attributes_mask, STATX_ATTR_MOUNT_ROOT))
                mountpoint = FLAGS_SET(sx.stx_attributes, STATX_ATTR_MOUNT_ROOT);
        else {
                struct stat ps;

                if (fstatat(dirfd(d), "..", &ps, AT_SYMLINK_NOFOLLOW) != 0)
                        return log_error_errno(errno, "stat(%s/..) failed: %m", i->path);

                mountpoint =
                        sx.stx_dev_major != major(ps.st_dev) ||
                        sx.stx_dev_minor != minor(ps.st_dev) ||
                        sx.stx_ino != ps.st_ino;
        }

        if (DEBUG_LOGGING) {
                _cleanup_free_ char *ab_f = NULL, *ab_d = NULL;

                ab_f = age_by_to_string(i->age_by_file, false);
                if (!ab_f)
                        return log_oom();

                ab_d = age_by_to_string(i->age_by_dir, true);
                if (!ab_d)
                        return log_oom();

                log_debug("Cleanup threshold for %s \"%s\" is %s; age-by: %s%s",
                          mountpoint ? "mount point" : "directory",
                          instance,
                          format_timestamp_style(timestamp, sizeof(timestamp), cutoff, TIMESTAMP_US),
                          ab_f, ab_d);
        }

        return dir_cleanup(i, instance, d,
                           load_statx_timestamp_nsec(&sx.stx_atime),
                           load_statx_timestamp_nsec(&sx.stx_mtime),
                           cutoff * NSEC_PER_USEC,
                           sx.stx_dev_major, sx.stx_dev_minor, mountpoint,
                           MAX_DEPTH, i->keep_first_level,
                           i->age_by_file, i->age_by_dir);
}

static int clean_item(Item *i) {
        assert(i);

        log_debug("Running clean action for entry %c %s", (char) i->type, i->path);

        switch (i->type) {
        case CREATE_DIRECTORY:
        case CREATE_SUBVOLUME:
        case CREATE_SUBVOLUME_INHERIT_QUOTA:
        case CREATE_SUBVOLUME_NEW_QUOTA:
        case TRUNCATE_DIRECTORY:
        case IGNORE_PATH:
        case COPY_FILES:
                clean_item_instance(i, i->path);
                return 0;
        case EMPTY_DIRECTORY:
        case IGNORE_DIRECTORY_PATH:
                return glob_item(i, clean_item_instance);
        default:
                return 0;
        }
}

static int process_item(Item *i, OperationMask operation) {
        OperationMask todo;
        _cleanup_free_ char *_path = NULL;
        const char *path;
        int r, q, p;

        assert(i);

        todo = operation & ~i->done;
        if (todo == 0) /* Everything already done? */
                return 0;

        i->done |= operation;

        path = i->path;
        if (string_is_glob(path)) {
                /* We can't easily check whether a glob matches any autofs path, so let's do the check only
                 * for the non-glob part. */

                r = glob_non_glob_prefix(path, &_path);
                if (r < 0 && r != -ENOENT)
                        return log_debug_errno(r, "Failed to deglob path: %m");
                if (r >= 0)
                        path = _path;
        }

        r = chase_symlinks(path, arg_root, CHASE_NO_AUTOFS|CHASE_NONEXISTENT|CHASE_WARN, NULL, NULL);
        if (r == -EREMOTE) {
                log_notice_errno(r, "Skipping %s", i->path); /* We log the configured path, to not confuse the user. */
                return 0;
        }
        if (r < 0)
                log_debug_errno(r, "Failed to determine whether '%s' is below autofs, ignoring: %m", i->path);

        r = FLAGS_SET(operation, OPERATION_CREATE) ? create_item(i) : 0;
        /* Failure can only be tolerated for create */
        if (i->allow_failure)
                r = 0;

        q = FLAGS_SET(operation, OPERATION_REMOVE) ? remove_item(i) : 0;
        p = FLAGS_SET(operation, OPERATION_CLEAN) ? clean_item(i) : 0;

        return r < 0 ? r :
                q < 0 ? q :
                p;
}

static int process_item_array(ItemArray *array, OperationMask operation) {
        int r = 0;
        size_t n;

        assert(array);

        /* Create any parent first. */
        if (FLAGS_SET(operation, OPERATION_CREATE) && array->parent)
                r = process_item_array(array->parent, operation & OPERATION_CREATE);

        /* Clean up all children first */
        if ((operation & (OPERATION_REMOVE|OPERATION_CLEAN)) && !set_isempty(array->children)) {
                ItemArray *c;

                SET_FOREACH(c, array->children) {
                        int k;

                        k = process_item_array(c, operation & (OPERATION_REMOVE|OPERATION_CLEAN));
                        if (k < 0 && r == 0)
                                r = k;
                }
        }

        for (n = 0; n < array->n_items; n++) {
                int k;

                k = process_item(array->items + n, operation);
                if (k < 0 && r == 0)
                        r = k;
        }

        return r;
}

static void item_free_contents(Item *i) {
        assert(i);
        free(i->path);
        free(i->argument);
        strv_free(i->xattrs);

#if HAVE_ACL
        if (i->acl_access)
                acl_free(i->acl_access);

        if (i->acl_default)
                acl_free(i->acl_default);
#endif
}

static ItemArray* item_array_free(ItemArray *a) {
        size_t n;

        if (!a)
                return NULL;

        for (n = 0; n < a->n_items; n++)
                item_free_contents(a->items + n);

        set_free(a->children);
        free(a->items);
        return mfree(a);
}

static int item_compare(const Item *a, const Item *b) {
        /* Make sure that the ownership taking item is put first, so
         * that we first create the node, and then can adjust it */

        if (takes_ownership(a->type) && !takes_ownership(b->type))
                return -1;
        if (!takes_ownership(a->type) && takes_ownership(b->type))
                return 1;

        return CMP(a->type, b->type);
}

static bool item_compatible(Item *a, Item *b) {
        assert(a);
        assert(b);
        assert(streq(a->path, b->path));

        if (takes_ownership(a->type) && takes_ownership(b->type))
                /* check if the items are the same */
                return  streq_ptr(a->argument, b->argument) &&

                        a->uid_set == b->uid_set &&
                        a->uid == b->uid &&

                        a->gid_set == b->gid_set &&
                        a->gid == b->gid &&

                        a->mode_set == b->mode_set &&
                        a->mode == b->mode &&

                        a->age_set == b->age_set &&
                        a->age == b->age &&

                        a->age_by_file == b->age_by_file &&
                        a->age_by_dir == b->age_by_dir &&

                        a->mask_perms == b->mask_perms &&

                        a->keep_first_level == b->keep_first_level &&

                        a->major_minor == b->major_minor;

        return true;
}

static bool should_include_path(const char *path) {
        char **prefix;

        STRV_FOREACH(prefix, arg_exclude_prefixes)
                if (path_startswith(path, *prefix)) {
                        log_debug("Entry \"%s\" matches exclude prefix \"%s\", skipping.",
                                  path, *prefix);
                        return false;
                }

        STRV_FOREACH(prefix, arg_include_prefixes)
                if (path_startswith(path, *prefix)) {
                        log_debug("Entry \"%s\" matches include prefix \"%s\".", path, *prefix);
                        return true;
                }

        /* no matches, so we should include this path only if we have no allow list at all */
        if (strv_isempty(arg_include_prefixes))
                return true;

        log_debug("Entry \"%s\" does not match any include prefix, skipping.", path);
        return false;
}

static int specifier_expansion_from_arg(Item *i) {
        _cleanup_free_ char *unescaped = NULL, *resolved = NULL;
        char **xattr;
        int r;

        assert(i);

        if (!i->argument)
                return 0;

        switch (i->type) {
        case COPY_FILES:
        case CREATE_SYMLINK:
        case CREATE_FILE:
        case TRUNCATE_FILE:
        case WRITE_FILE:
                r = cunescape(i->argument, 0, &unescaped);
                if (r < 0)
                        return log_error_errno(r, "Failed to unescape parameter to write: %s", i->argument);

                r = specifier_printf(unescaped, PATH_MAX-1, specifier_table, arg_root, NULL, &resolved);
                if (r < 0)
                        return r;

                free_and_replace(i->argument, resolved);
                break;

        case SET_XATTR:
        case RECURSIVE_SET_XATTR:
                STRV_FOREACH(xattr, i->xattrs) {
                        r = specifier_printf(*xattr, SIZE_MAX, specifier_table, arg_root, NULL, &resolved);
                        if (r < 0)
                                return r;

                        free_and_replace(*xattr, resolved);
                }
                break;

        default:
                break;
        }
        return 0;
}

static int patch_var_run(const char *fname, unsigned line, char **path) {
        const char *k;
        char *n;

        assert(path);
        assert(*path);

        /* Optionally rewrites lines referencing /var/run/, to use /run/ instead. Why bother? tmpfiles merges lines in
         * some cases and detects conflicts in others. If files/directories are specified through two equivalent lines
         * this is problematic as neither case will be detected. Ideally we'd detect these cases by resolving symlinks
         * early, but that's precisely not what we can do here as this code very likely is running very early on, at a
         * time where the paths in question are not available yet, or even more importantly, our own tmpfiles rules
         * might create the paths that are intermediary to the listed paths. We can't really cover the generic case,
         * but the least we can do is cover the specific case of /var/run vs. /run, as /var/run is a legacy name for
         * /run only, and we explicitly document that and require that on systemd systems the former is a symlink to
         * the latter. Moreover files below this path are by far the primary usecase for tmpfiles.d/. */

        k = path_startswith(*path, "/var/run/");
        if (isempty(k)) /* Don't complain about other paths than /var/run, and not about /var/run itself either. */
                return 0;

        n = path_join("/run", k);
        if (!n)
                return log_oom();

        /* Also log about this briefly. We do so at LOG_NOTICE level, as we fixed up the situation automatically, hence
         * there's no immediate need for action by the user. However, in the interest of making things less confusing
         * to the user, let's still inform the user that these snippets should really be updated. */
        log_syntax(NULL, LOG_NOTICE, fname, line, 0,
                   "Line references path below legacy directory /var/run/, updating %s → %s; please update the tmpfiles.d/ drop-in file accordingly.",
                   *path, n);

        free_and_replace(*path, n);

        return 0;
}

static int find_uid(const char *user, uid_t *ret_uid, Hashmap **cache) {
        int r;

        assert(user);
        assert(ret_uid);

        /* First: parse as numeric UID string */
        r = parse_uid(user, ret_uid);
        if (r >= 0)
                return r;

        /* Second: pass to NSS if we are running "online" */
        if (!arg_root)
                return get_user_creds(&user, ret_uid, NULL, NULL, NULL, 0);

        /* Third, synthesize "root" unconditionally */
        if (streq(user, "root")) {
                *ret_uid = 0;
                return 0;
        }

        /* Fourth: use fgetpwent() to read /etc/passwd directly, if we are "offline" */
        return name_to_uid_offline(arg_root, user, ret_uid, cache);
}

static int find_gid(const char *group, gid_t *ret_gid, Hashmap **cache) {
        int r;

        assert(group);
        assert(ret_gid);

        /* First: parse as numeric GID string */
        r = parse_gid(group, ret_gid);
        if (r >= 0)
                return r;

        /* Second: pass to NSS if we are running "online" */
        if (!arg_root)
                return get_group_creds(&group, ret_gid, 0);

        /* Third, synthesize "root" unconditionally */
        if (streq(group, "root")) {
                *ret_gid = 0;
                return 0;
        }

        /* Fourth: use fgetgrent() to read /etc/group directly, if we are "offline" */
        return name_to_gid_offline(arg_root, group, ret_gid, cache);
}

static int parse_age_by_from_arg(const char *age_by_str, Item *item) {
        AgeBy ab_f = 0, ab_d = 0;

        static const struct {
                char age_by_chr;
                AgeBy age_by_flag;
        } age_by_types[] = {
                { 'a', AGE_BY_ATIME },
                { 'b', AGE_BY_BTIME },
                { 'c', AGE_BY_CTIME },
                { 'm', AGE_BY_MTIME },
        };

        assert(age_by_str);
        assert(item);

        if (isempty(age_by_str))
                return -EINVAL;

        for (const char *s = age_by_str; *s != 0; s++) {
                size_t i;

                /* Ignore whitespace. */
                if (strchr(WHITESPACE, *s))
                        continue;

                for (i = 0; i < ELEMENTSOF(age_by_types); i++) {
                        /* Check lower-case for files, upper-case for directories. */
                        if (*s == age_by_types[i].age_by_chr) {
                                ab_f |= age_by_types[i].age_by_flag;
                                break;
                        } else if (*s == ascii_toupper(age_by_types[i].age_by_chr)) {
                                ab_d |= age_by_types[i].age_by_flag;
                                break;
                        }
                }

                /* Invalid character. */
                if (i >= ELEMENTSOF(age_by_types))
                        return -EINVAL;
        }

        /* No match. */
        if (ab_f == 0 && ab_d == 0)
                return -EINVAL;

        item->age_by_file = ab_f > 0 ? ab_f : AGE_BY_DEFAULT_FILE;
        item->age_by_dir = ab_d > 0 ? ab_d : AGE_BY_DEFAULT_DIR;

        return 0;
}

static int parse_line(
                const char *fname,
                unsigned line,
                const char *buffer,
                bool *invalid_config,
                Hashmap **uid_cache,
                Hashmap **gid_cache) {

        _cleanup_free_ char *action = NULL, *mode = NULL, *user = NULL, *group = NULL, *age = NULL, *path = NULL;
        _cleanup_(item_free_contents) Item i = {
                /* The "age-by" argument considers all file timestamp types by default. */
                .age_by_file = AGE_BY_DEFAULT_FILE,
                .age_by_dir = AGE_BY_DEFAULT_DIR,
        };
        ItemArray *existing;
        OrderedHashmap *h;
        int r, pos;
        bool append_or_force = false, boot = false, allow_failure = false, try_replace = false;

        assert(fname);
        assert(line >= 1);
        assert(buffer);

        r = extract_many_words(
                        &buffer,
                        NULL,
                        EXTRACT_UNQUOTE,
                        &action,
                        &path,
                        &mode,
                        &user,
                        &group,
                        &age,
                        NULL);
        if (r < 0) {
                if (IN_SET(r, -EINVAL, -EBADSLT))
                        /* invalid quoting and such or an unknown specifier */
                        *invalid_config = true;
                return log_syntax(NULL, LOG_ERR, fname, line, r, "Failed to parse line: %m");
        } else if (r < 2) {
                *invalid_config = true;
                return log_syntax(NULL, LOG_ERR, fname, line, SYNTHETIC_ERRNO(EBADMSG), "Syntax error.");
        }

        if (!empty_or_dash(buffer)) {
                i.argument = strdup(buffer);
                if (!i.argument)
                        return log_oom();
        }

        if (isempty(action)) {
                *invalid_config = true;
                return log_syntax(NULL, LOG_ERR, fname, line, SYNTHETIC_ERRNO(EBADMSG), "Command too short '%s'.", action);
        }

        for (pos = 1; action[pos]; pos++) {
                if (action[pos] == '!' && !boot)
                        boot = true;
                else if (action[pos] == '+' && !append_or_force)
                        append_or_force = true;
                else if (action[pos] == '-' && !allow_failure)
                        allow_failure = true;
                else if (action[pos] == '=' && !try_replace)
                        try_replace = true;
                else {
                        *invalid_config = true;
                        return log_syntax(NULL, LOG_ERR, fname, line, SYNTHETIC_ERRNO(EBADMSG), "Unknown modifiers in command '%s'", action);
                }
        }

        if (boot && !arg_boot) {
                log_syntax(NULL, LOG_DEBUG, fname, line, 0, "Ignoring entry %s \"%s\" because --boot is not specified.", action, path);
                return 0;
        }

        i.type = action[0];
        i.append_or_force = append_or_force;
        i.allow_failure = allow_failure;
        i.try_replace = try_replace;

        r = specifier_printf(path, PATH_MAX-1, specifier_table, arg_root, NULL, &i.path);
        if (r == -ENXIO)
                return log_unresolvable_specifier(fname, line);
        if (r < 0) {
                if (IN_SET(r, -EINVAL, -EBADSLT))
                        *invalid_config = true;
                return log_syntax(NULL, LOG_ERR, fname, line, r, "Failed to replace specifiers in '%s': %m", path);
        }

        r = patch_var_run(fname, line, &i.path);
        if (r < 0)
                return r;

        switch (i.type) {

        case CREATE_DIRECTORY:
        case CREATE_SUBVOLUME:
        case CREATE_SUBVOLUME_INHERIT_QUOTA:
        case CREATE_SUBVOLUME_NEW_QUOTA:
        case EMPTY_DIRECTORY:
        case TRUNCATE_DIRECTORY:
        case CREATE_FIFO:
        case IGNORE_PATH:
        case IGNORE_DIRECTORY_PATH:
        case REMOVE_PATH:
        case RECURSIVE_REMOVE_PATH:
        case ADJUST_MODE:
        case RELABEL_PATH:
        case RECURSIVE_RELABEL_PATH:
                if (i.argument)
                        log_syntax(NULL, LOG_WARNING, fname, line, 0, "%c lines don't take argument fields, ignoring.", i.type);

                break;

        case CREATE_FILE:
        case TRUNCATE_FILE:
                break;

        case CREATE_SYMLINK:
                if (!i.argument) {
                        i.argument = path_join("/usr/share/factory", i.path);
                        if (!i.argument)
                                return log_oom();
                }
                break;

        case WRITE_FILE:
                if (!i.argument) {
                        *invalid_config = true;
                        return log_syntax(NULL, LOG_ERR, fname, line, SYNTHETIC_ERRNO(EBADMSG), "Write file requires argument.");
                }
                break;

        case COPY_FILES:
                if (!i.argument) {
                        i.argument = path_join("/usr/share/factory", i.path);
                        if (!i.argument)
                                return log_oom();

                } else if (!path_is_absolute(i.argument)) {
                        *invalid_config = true;
                        return log_syntax(NULL, LOG_ERR, fname, line, SYNTHETIC_ERRNO(EBADMSG), "Source path '%s' is not absolute.", i.argument);

                }

                if (!empty_or_root(arg_root)) {
                        char *p;

                        p = path_join(arg_root, i.argument);
                        if (!p)
                                return log_oom();
                        free_and_replace(i.argument, p);
                }

                path_simplify(i.argument);
                break;

        case CREATE_CHAR_DEVICE:
        case CREATE_BLOCK_DEVICE:
                if (!i.argument) {
                        *invalid_config = true;
                        return log_syntax(NULL, LOG_ERR, fname, line, SYNTHETIC_ERRNO(EBADMSG), "Device file requires argument.");
                }

                r = parse_dev(i.argument, &i.major_minor);
                if (r < 0) {
                        *invalid_config = true;
                        return log_syntax(NULL, LOG_ERR, fname, line, r, "Can't parse device file major/minor '%s'.", i.argument);
                }

                break;

        case SET_XATTR:
        case RECURSIVE_SET_XATTR:
                if (!i.argument) {
                        *invalid_config = true;
                        return log_syntax(NULL, LOG_ERR, fname, line, SYNTHETIC_ERRNO(EBADMSG),
                                          "Set extended attribute requires argument.");
                }
                r = parse_xattrs_from_arg(&i);
                if (r < 0)
                        return r;
                break;

        case SET_ACL:
        case RECURSIVE_SET_ACL:
                if (!i.argument) {
                        *invalid_config = true;
                        return log_syntax(NULL, LOG_ERR, fname, line, SYNTHETIC_ERRNO(EBADMSG),
                                          "Set ACLs requires argument.");
                }
                r = parse_acls_from_arg(&i);
                if (r < 0)
                        return r;
                break;

        case SET_ATTRIBUTE:
        case RECURSIVE_SET_ATTRIBUTE:
                if (!i.argument) {
                        *invalid_config = true;
                        return log_syntax(NULL, LOG_ERR, fname, line, SYNTHETIC_ERRNO(EBADMSG),
                                          "Set file attribute requires argument.");
                }
                r = parse_attribute_from_arg(&i);
                if (IN_SET(r, -EINVAL, -EBADSLT))
                        *invalid_config = true;
                if (r < 0)
                        return r;
                break;

        default:
                *invalid_config = true;
                return log_syntax(NULL, LOG_ERR, fname, line, SYNTHETIC_ERRNO(EBADMSG),
                                  "Unknown command type '%c'.", (char) i.type);
        }

        if (!path_is_absolute(i.path)) {
                *invalid_config = true;
                return log_syntax(NULL, LOG_ERR, fname, line, SYNTHETIC_ERRNO(EBADMSG),
                                  "Path '%s' not absolute.", i.path);
        }

        path_simplify(i.path);

        if (!should_include_path(i.path))
                return 0;

        r = specifier_expansion_from_arg(&i);
        if (r == -ENXIO)
                return log_unresolvable_specifier(fname, line);
        if (r < 0) {
                if (IN_SET(r, -EINVAL, -EBADSLT))
                        *invalid_config = true;
                return log_syntax(NULL, LOG_ERR, fname, line, r, "Failed to substitute specifiers in argument: %m");
        }

        if (!empty_or_root(arg_root)) {
                char *p;

                p = path_join(arg_root, i.path);
                if (!p)
                        return log_oom();
                free_and_replace(i.path, p);
        }

        if (!empty_or_dash(user)) {
                r = find_uid(user, &i.uid, uid_cache);
                if (r < 0) {
                        *invalid_config = true;
                        return log_syntax(NULL, LOG_ERR, fname, line, r, "Failed to resolve user '%s': %m", user);
                }

                i.uid_set = true;
        }

        if (!empty_or_dash(group)) {
                r = find_gid(group, &i.gid, gid_cache);
                if (r < 0) {
                        *invalid_config = true;
                        return log_syntax(NULL, LOG_ERR, fname, line, r, "Failed to resolve group '%s'.", group);
                }

                i.gid_set = true;
        }

        if (!empty_or_dash(mode)) {
                const char *mm = mode;
                unsigned m;

                if (*mm == '~') {
                        i.mask_perms = true;
                        mm++;
                }

                r = parse_mode(mm, &m);
                if (r < 0) {
                        *invalid_config = true;
                        return log_syntax(NULL, LOG_ERR, fname, line, r, "Invalid mode '%s'.", mode);
                }

                i.mode = m;
                i.mode_set = true;
        } else
                i.mode = IN_SET(i.type, CREATE_DIRECTORY, TRUNCATE_DIRECTORY, CREATE_SUBVOLUME, CREATE_SUBVOLUME_INHERIT_QUOTA, CREATE_SUBVOLUME_NEW_QUOTA) ? 0755 : 0644;

        if (!empty_or_dash(age)) {
                const char *a = age;
                _cleanup_free_ char *seconds = NULL, *age_by = NULL;

                if (*a == '~') {
                        i.keep_first_level = true;
                        a++;
                }

                /* Format: "age-by:age"; where age-by is "[abcmABCM]+". */
                r = split_pair(a, ":", &age_by, &seconds);
                if (r == -ENOMEM)
                        return log_oom();
                if (r < 0 && r != -EINVAL)
                        return log_error_errno(r, "Failed to parse age-by for '%s': %m", age);
                if (r >= 0) {
                        /* We found a ":", parse the "age-by" part. */
                        r = parse_age_by_from_arg(age_by, &i);
                        if (r == -ENOMEM)
                                return log_oom();
                        if (r < 0) {
                                *invalid_config = true;
                                return log_syntax(NULL, LOG_ERR, fname, line, r, "Invalid age-by '%s'.", age_by);
                        }

                        /* For parsing the "age" part, after the ":". */
                        a = seconds;
                }

                r = parse_sec(a, &i.age);
                if (r < 0) {
                        *invalid_config = true;
                        return log_syntax(NULL, LOG_ERR, fname, line, r, "Invalid age '%s'.", a);
                }

                i.age_set = true;
        }

        h = needs_glob(i.type) ? globs : items;

        existing = ordered_hashmap_get(h, i.path);
        if (existing) {
                size_t n;

                for (n = 0; n < existing->n_items; n++) {
                        if (!item_compatible(existing->items + n, &i) && !i.append_or_force) {
                                log_syntax(NULL, LOG_NOTICE, fname, line, 0, "Duplicate line for path \"%s\", ignoring.", i.path);
                                return 0;
                        }
                }
        } else {
                existing = new0(ItemArray, 1);
                if (!existing)
                        return log_oom();

                r = ordered_hashmap_put(h, i.path, existing);
                if (r < 0) {
                        free(existing);
                        return log_oom();
                }
        }

        if (!GREEDY_REALLOC(existing->items, existing->n_items + 1))
                return log_oom();

        existing->items[existing->n_items++] = i;
        i = (struct Item) {};

        /* Sort item array, to enforce stable ordering of application */
        typesafe_qsort(existing->items, existing->n_items, item_compare);

        return 0;
}

static int cat_config(char **config_dirs, char **args) {
        _cleanup_strv_free_ char **files = NULL;
        int r;

        r = conf_files_list_with_replacement(arg_root, config_dirs, arg_replace, &files, NULL);
        if (r < 0)
                return r;

        return cat_files(NULL, files, 0);
}

static int exclude_default_prefixes(void) {
        int r;

        /* Provide an easy way to exclude virtual/memory file systems from what we do here. Useful in
         * combination with --root= where we probably don't want to apply stuff to these dirs as they are
         * likely over-mounted if the root directory is actually used, and it wouldbe less than ideal to have
         * all kinds of files created/adjusted underneath these mount points. */

        r = strv_extend_strv(
                        &arg_exclude_prefixes,
                        STRV_MAKE("/dev",
                                  "/proc",
                                  "/run",
                                  "/sys"),
                                 true);
        if (r < 0)
                return log_oom();

        return 0;
}

static int help(void) {
        _cleanup_free_ char *link = NULL;
        int r;

        r = terminal_urlify_man("systemd-tmpfiles", "8", &link);
        if (r < 0)
                return log_oom();

        printf("%s [OPTIONS...] [CONFIGURATION FILE...]\n"
               "\n%sCreates, deletes and cleans up volatile and temporary files and directories.%s\n\n"
               "  -h --help                 Show this help\n"
               "     --user                 Execute user configuration\n"
               "     --version              Show package version\n"
               "     --cat-config           Show configuration files\n"
               "     --create               Create marked files/directories\n"
               "     --clean                Clean up marked directories\n"
               "     --remove               Remove marked files/directories\n"
               "     --boot                 Execute actions only safe at boot\n"
               "     --prefix=PATH          Only apply rules with the specified prefix\n"
               "     --exclude-prefix=PATH  Ignore rules with the specified prefix\n"
               "  -E                        Ignore rules prefixed with /dev, /proc, /run, /sys\n"
               "     --root=PATH            Operate on an alternate filesystem root\n"
               "     --image=PATH           Operate on disk image as filesystem root\n"
               "     --replace=PATH         Treat arguments as replacement for PATH\n"
               "     --no-pager             Do not pipe output into a pager\n"
               "\nSee the %s for details.\n",
               program_invocation_short_name,
               ansi_highlight(),
               ansi_normal(),
               link);

        return 0;
}

static int parse_argv(int argc, char *argv[]) {

        enum {
                ARG_VERSION = 0x100,
                ARG_CAT_CONFIG,
                ARG_USER,
                ARG_CREATE,
                ARG_CLEAN,
                ARG_REMOVE,
                ARG_BOOT,
                ARG_PREFIX,
                ARG_EXCLUDE_PREFIX,
                ARG_ROOT,
                ARG_IMAGE,
                ARG_REPLACE,
                ARG_NO_PAGER,
        };

        static const struct option options[] = {
                { "help",           no_argument,         NULL, 'h'                },
                { "user",           no_argument,         NULL, ARG_USER           },
                { "version",        no_argument,         NULL, ARG_VERSION        },
                { "cat-config",     no_argument,         NULL, ARG_CAT_CONFIG     },
                { "create",         no_argument,         NULL, ARG_CREATE         },
                { "clean",          no_argument,         NULL, ARG_CLEAN          },
                { "remove",         no_argument,         NULL, ARG_REMOVE         },
                { "boot",           no_argument,         NULL, ARG_BOOT           },
                { "prefix",         required_argument,   NULL, ARG_PREFIX         },
                { "exclude-prefix", required_argument,   NULL, ARG_EXCLUDE_PREFIX },
                { "root",           required_argument,   NULL, ARG_ROOT           },
                { "image",          required_argument,   NULL, ARG_IMAGE          },
                { "replace",        required_argument,   NULL, ARG_REPLACE        },
                { "no-pager",       no_argument,         NULL, ARG_NO_PAGER       },
                {}
        };

        int c, r;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "hE", options, NULL)) >= 0)

                switch (c) {

                case 'h':
                        return help();

                case ARG_VERSION:
                        return version();

                case ARG_CAT_CONFIG:
                        arg_cat_config = true;
                        break;

                case ARG_USER:
                        arg_user = true;
                        break;

                case ARG_CREATE:
                        arg_operation |= OPERATION_CREATE;
                        break;

                case ARG_CLEAN:
                        arg_operation |= OPERATION_CLEAN;
                        break;

                case ARG_REMOVE:
                        arg_operation |= OPERATION_REMOVE;
                        break;

                case ARG_BOOT:
                        arg_boot = true;
                        break;

                case ARG_PREFIX:
                        if (strv_push(&arg_include_prefixes, optarg) < 0)
                                return log_oom();
                        break;

                case ARG_EXCLUDE_PREFIX:
                        if (strv_push(&arg_exclude_prefixes, optarg) < 0)
                                return log_oom();
                        break;

                case ARG_ROOT:
                        r = parse_path_argument(optarg, /* suppress_root= */ false, &arg_root);
                        if (r < 0)
                                return r;
                        break;

                case ARG_IMAGE:
#ifdef STANDALONE
                        return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                                               "This systemd-tmpfiles version is compiled without support for --image=.");
#else
                        r = parse_path_argument(optarg, /* suppress_root= */ false, &arg_image);
                        if (r < 0)
                                return r;
#endif
                        /* Imply -E here since it makes little sense to create files persistently in the /run mountpoint of a disk image */
                        _fallthrough_;

                case 'E':
                        r = exclude_default_prefixes();
                        if (r < 0)
                                return r;

                        break;

                case ARG_REPLACE:
                        if (!path_is_absolute(optarg) ||
                            !endswith(optarg, ".conf"))
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                                       "The argument to --replace= must an absolute path to a config file");

                        arg_replace = optarg;
                        break;

                case ARG_NO_PAGER:
                        arg_pager_flags |= PAGER_DISABLE;
                        break;

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached("Unhandled option");
                }

        if (arg_operation == 0 && !arg_cat_config)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "You need to specify at least one of --clean, --create or --remove.");

        if (arg_replace && arg_cat_config)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "Option --replace= is not supported with --cat-config");

        if (arg_replace && optind >= argc)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "When --replace= is given, some configuration items must be specified");

        if (arg_root && arg_user)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "Combination of --user and --root= is not supported.");

        if (arg_image && arg_root)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Please specify either --root= or --image=, the combination of both is not supported.");

        return 1;
}

static int read_config_file(char **config_dirs, const char *fn, bool ignore_enoent, bool *invalid_config) {
        _cleanup_(hashmap_freep) Hashmap *uid_cache = NULL, *gid_cache = NULL;
        _cleanup_fclose_ FILE *_f = NULL;
        _cleanup_free_ char *pp = NULL;
        unsigned v = 0;
        FILE *f;
        ItemArray *ia;
        int r = 0;

        assert(fn);

        if (streq(fn, "-")) {
                log_debug("Reading config from stdin…");
                fn = "<stdin>";
                f = stdin;
        } else {
                r = search_and_fopen(fn, "re", arg_root, (const char**) config_dirs, &_f, &pp);
                if (r < 0) {
                        if (ignore_enoent && r == -ENOENT) {
                                log_debug_errno(r, "Failed to open \"%s\", ignoring: %m", fn);
                                return 0;
                        }

                        return log_error_errno(r, "Failed to open '%s': %m", fn);
                }

                log_debug("Reading config file \"%s\"…", pp);
                fn = pp;
                f = _f;
        }

        for (;;) {
                _cleanup_free_ char *line = NULL;
                bool invalid_line = false;
                char *l;
                int k;

                k = read_line(f, LONG_LINE_MAX, &line);
                if (k < 0)
                        return log_error_errno(k, "Failed to read '%s': %m", fn);
                if (k == 0)
                        break;

                v++;

                l = strstrip(line);
                if (IN_SET(*l, 0, '#'))
                        continue;

                k = parse_line(fn, v, l, &invalid_line, &uid_cache, &gid_cache);
                if (k < 0) {
                        if (invalid_line)
                                /* Allow reporting with a special code if the caller requested this */
                                *invalid_config = true;
                        else if (r == 0)
                                /* The first error becomes our return value */
                                r = k;
                }
        }

        /* we have to determine age parameter for each entry of type X */
        ORDERED_HASHMAP_FOREACH(ia, globs)
                for (size_t ni = 0; ni < ia->n_items; ni++) {
                        ItemArray *ja;
                        Item *i = ia->items + ni, *candidate_item = NULL;

                        if (i->type != IGNORE_DIRECTORY_PATH)
                                continue;

                        ORDERED_HASHMAP_FOREACH(ja, items)
                                for (size_t nj = 0; nj < ja->n_items; nj++) {
                                        Item *j = ja->items + nj;

                                        if (!IN_SET(j->type, CREATE_DIRECTORY,
                                                             TRUNCATE_DIRECTORY,
                                                             CREATE_SUBVOLUME,
                                                             CREATE_SUBVOLUME_INHERIT_QUOTA,
                                                             CREATE_SUBVOLUME_NEW_QUOTA))
                                                continue;

                                        if (path_equal(j->path, i->path)) {
                                                candidate_item = j;
                                                break;
                                        }

                                        if (candidate_item
                                            ? (path_startswith(j->path, candidate_item->path) && fnmatch(i->path, j->path, FNM_PATHNAME | FNM_PERIOD) == 0)
                                            : path_startswith(i->path, j->path) != NULL)
                                                candidate_item = j;
                                }

                        if (candidate_item && candidate_item->age_set) {
                                i->age = candidate_item->age;
                                i->age_set = true;
                        }
                }

        if (ferror(f)) {
                log_error_errno(errno, "Failed to read from file %s: %m", fn);
                if (r == 0)
                        r = -EIO;
        }

        return r;
}

static int parse_arguments(char **config_dirs, char **args, bool *invalid_config) {
        char **arg;
        int r;

        STRV_FOREACH(arg, args) {
                r = read_config_file(config_dirs, *arg, false, invalid_config);
                if (r < 0)
                        return r;
        }

        return 0;
}

static int read_config_files(char **config_dirs, char **args, bool *invalid_config) {
        _cleanup_strv_free_ char **files = NULL;
        _cleanup_free_ char *p = NULL;
        char **f;
        int r;

        r = conf_files_list_with_replacement(arg_root, config_dirs, arg_replace, &files, &p);
        if (r < 0)
                return r;

        STRV_FOREACH(f, files)
                if (p && path_equal(*f, p)) {
                        log_debug("Parsing arguments at position \"%s\"…", *f);

                        r = parse_arguments(config_dirs, args, invalid_config);
                        if (r < 0)
                                return r;
                } else
                        /* Just warn, ignore result otherwise.
                         * read_config_file() has some debug output, so no need to print anything. */
                        (void) read_config_file(config_dirs, *f, true, invalid_config);

        return 0;
}

static int link_parent(ItemArray *a) {
        const char *path;
        char *prefix;
        int r;

        assert(a);

        /* Finds the closest "parent" item array for the specified item array. Then registers the specified item array
         * as child of it, and fills the parent in, linking them both ways. This allows us to later create parents
         * before their children, and clean up/remove children before their parents. */

        if (a->n_items <= 0)
                return 0;

        path = a->items[0].path;
        prefix = newa(char, strlen(path) + 1);
        PATH_FOREACH_PREFIX(prefix, path) {
                ItemArray *j;

                j = ordered_hashmap_get(items, prefix);
                if (!j)
                        j = ordered_hashmap_get(globs, prefix);
                if (j) {
                        r = set_ensure_put(&j->children, NULL, a);
                        if (r < 0)
                                return log_oom();

                        a->parent = j;
                        return 1;
                }
        }

        return 0;
}

DEFINE_PRIVATE_HASH_OPS_WITH_VALUE_DESTRUCTOR(item_array_hash_ops, char, string_hash_func, string_compare_func,
                                              ItemArray, item_array_free);

static int run(int argc, char *argv[]) {
#ifndef STANDALONE
        _cleanup_(loop_device_unrefp) LoopDevice *loop_device = NULL;
        _cleanup_(decrypted_image_unrefp) DecryptedImage *decrypted_image = NULL;
        _cleanup_(umount_and_rmdir_and_freep) char *unlink_dir = NULL;
#endif
        _cleanup_strv_free_ char **config_dirs = NULL;
        bool invalid_config = false;
        ItemArray *a;
        enum {
                PHASE_REMOVE_AND_CLEAN,
                PHASE_CREATE,
                _PHASE_MAX
        } phase;
        int r, k;

        r = parse_argv(argc, argv);
        if (r <= 0)
                return r;

        log_setup();

        /* We require /proc/ for a lot of our operations, i.e. for adjusting access modes, for anything
         * SELinux related, for recursive operation, for xattr, acl and chattr handling, for btrfs stuff and
         * a lot more. It's probably the majority of invocations where /proc/ is required. Since people
         * apparently invoke it without anyway and are surprised about the failures, let's catch this early
         * and output a nice and friendly warning. */
        if (proc_mounted() == 0)
                return log_error_errno(SYNTHETIC_ERRNO(ENOSYS),
                                       "/proc/ is not mounted, but required for successful operation of systemd-tmpfiles. "
                                       "Please mount /proc/. Alternatively, consider using the --root= or --image= switches.");

        /* Descending down file system trees might take a lot of fds */
        (void) rlimit_nofile_bump(HIGH_RLIMIT_NOFILE);

        if (arg_user) {
                r = user_config_paths(&config_dirs);
                if (r < 0)
                        return log_error_errno(r, "Failed to initialize configuration directory list: %m");
        } else {
                config_dirs = strv_split_nulstr(CONF_PATHS_NULSTR("tmpfiles.d"));
                if (!config_dirs)
                        return log_oom();
        }

        if (DEBUG_LOGGING) {
                _cleanup_free_ char *t = NULL;
                char **i;

                STRV_FOREACH(i, config_dirs) {
                        _cleanup_free_ char *j = NULL;

                        j = path_join(arg_root, *i);
                        if (!j)
                                return log_oom();

                        if (!strextend(&t, "\n\t", j))
                                return log_oom();
                }

                log_debug("Looking for configuration files in (higher priority first):%s", t);
        }

        if (arg_cat_config) {
                (void) pager_open(arg_pager_flags);

                return cat_config(config_dirs, argv + optind);
        }

        umask(0022);

        r = mac_selinux_init();
        if (r < 0)
                return r;

#ifndef STANDALONE
        if (arg_image) {
                assert(!arg_root);

                r = mount_image_privately_interactively(
                                arg_image,
                                DISSECT_IMAGE_GENERIC_ROOT |
                                DISSECT_IMAGE_REQUIRE_ROOT |
                                DISSECT_IMAGE_VALIDATE_OS |
                                DISSECT_IMAGE_RELAX_VAR_CHECK |
                                DISSECT_IMAGE_FSCK |
                                DISSECT_IMAGE_GROWFS,
                                &unlink_dir,
                                &loop_device,
                                &decrypted_image);
                if (r < 0)
                        return r;

                arg_root = strdup(unlink_dir);
                if (!arg_root)
                        return log_oom();
        }
#else
        assert(!arg_image);
#endif

        items = ordered_hashmap_new(&item_array_hash_ops);
        globs = ordered_hashmap_new(&item_array_hash_ops);
        if (!items || !globs)
                return log_oom();

        /* If command line arguments are specified along with --replace, read all
         * configuration files and insert the positional arguments at the specified
         * place. Otherwise, if command line arguments are specified, execute just
         * them, and finally, without --replace= or any positional arguments, just
         * read configuration and execute it.
         */
        if (arg_replace || optind >= argc)
                r = read_config_files(config_dirs, argv + optind, &invalid_config);
        else
                r = parse_arguments(config_dirs, argv + optind, &invalid_config);
        if (r < 0)
                return r;

        /* Let's now link up all child/parent relationships */
        ORDERED_HASHMAP_FOREACH(a, items) {
                r = link_parent(a);
                if (r < 0)
                        return r;
        }
        ORDERED_HASHMAP_FOREACH(a, globs) {
                r = link_parent(a);
                if (r < 0)
                        return r;
        }

        /* If multiple operations are requested, let's first run the remove/clean operations, and only then the create
         * operations. i.e. that we first clean out the platform we then build on. */
        for (phase = 0; phase < _PHASE_MAX; phase++) {
                OperationMask op;

                if (phase == PHASE_REMOVE_AND_CLEAN)
                        op = arg_operation & (OPERATION_REMOVE|OPERATION_CLEAN);
                else if (phase == PHASE_CREATE)
                        op = arg_operation & OPERATION_CREATE;
                else
                        assert_not_reached("unexpected phase");

                if (op == 0) /* Nothing requested in this phase */
                        continue;

                /* The non-globbing ones usually create things, hence we apply them first */
                ORDERED_HASHMAP_FOREACH(a, items) {
                        k = process_item_array(a, op);
                        if (k < 0 && r >= 0)
                                r = k;
                }

                /* The globbing ones usually alter things, hence we apply them second. */
                ORDERED_HASHMAP_FOREACH(a, globs) {
                        k = process_item_array(a, op);
                        if (k < 0 && r >= 0)
                                r = k;
                }
        }

        if (ERRNO_IS_RESOURCE(r))
                return r;
        if (invalid_config)
                return EX_DATAERR;
        if (r < 0)
                return EX_CANTCREAT;
        return 0;
}

DEFINE_MAIN_FUNCTION_WITH_POSITIVE_FAILURE(run);
