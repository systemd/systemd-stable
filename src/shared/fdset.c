/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>

#include "sd-daemon.h"

#include "alloc-util.h"
#include "dirent-util.h"
#include "fd-util.h"
#include "fdset.h"
#include "log.h"
#include "macro.h"
#include "parse-util.h"
#include "path-util.h"
#include "set.h"

#define MAKE_SET(s) ((Set*) s)
#define MAKE_FDSET(s) ((FDSet*) s)

FDSet *fdset_new(void) {
        return MAKE_FDSET(set_new(NULL));
}

int fdset_new_array(FDSet **ret, const int *fds, size_t n_fds) {
        size_t i;
        FDSet *s;
        int r;

        assert(ret);

        s = fdset_new();
        if (!s)
                return -ENOMEM;

        for (i = 0; i < n_fds; i++) {

                r = fdset_put(s, fds[i]);
                if (r < 0) {
                        set_free(MAKE_SET(s));
                        return r;
                }
        }

        *ret = s;
        return 0;
}

void fdset_close(FDSet *s) {
        void *p;

        while ((p = set_steal_first(MAKE_SET(s)))) {
                int fd = PTR_TO_FD(p);

                /* Valgrind's fd might have ended up in this set here, due to fdset_new_fill(). We'll ignore
                 * all failures here, so that the EBADFD that valgrind will return us on close() doesn't
                 * influence us */

                /* When reloading duplicates of the private bus connection fds and suchlike are closed here,
                 * which has no effect at all, since they are only duplicates. So don't be surprised about
                 * these log messages. */

                if (DEBUG_LOGGING) {
                        _cleanup_free_ char *path = NULL;

                        (void) fd_get_path(fd, &path);
                        log_debug("Closing set fd %i (%s)", fd, strna(path));
                }

                (void) close_nointr(fd);
        }
}

FDSet* fdset_free(FDSet *s) {
        fdset_close(s);
        set_free(MAKE_SET(s));
        return NULL;
}

int fdset_put(FDSet *s, int fd) {
        assert(s);
        assert(fd >= 0);

        /* Avoid integer overflow in FD_TO_PTR() */
        if (fd == INT_MAX)
                return log_debug_errno(SYNTHETIC_ERRNO(EINVAL), "Refusing invalid fd: %d", fd);

        return set_put(MAKE_SET(s), FD_TO_PTR(fd));
}

int fdset_put_dup(FDSet *s, int fd) {
        int copy, r;

        assert(s);
        assert(fd >= 0);

        copy = fcntl(fd, F_DUPFD_CLOEXEC, 3);
        if (copy < 0)
                return -errno;

        r = fdset_put(s, copy);
        if (r < 0) {
                safe_close(copy);
                return r;
        }

        return copy;
}

bool fdset_contains(FDSet *s, int fd) {
        assert(s);
        assert(fd >= 0);

        /* Avoid integer overflow in FD_TO_PTR() */
        if (fd == INT_MAX) {
                log_debug("Refusing invalid fd: %d", fd);
                return false;
        }

        return !!set_get(MAKE_SET(s), FD_TO_PTR(fd));
}

int fdset_remove(FDSet *s, int fd) {
        assert(s);
        assert(fd >= 0);

        /* Avoid integer overflow in FD_TO_PTR() */
        if (fd == INT_MAX)
                return log_debug_errno(SYNTHETIC_ERRNO(ENOENT), "Refusing invalid fd: %d", fd);

        return set_remove(MAKE_SET(s), FD_TO_PTR(fd)) ? fd : -ENOENT;
}

int fdset_new_fill(
                int filter_cloexec, /* if < 0 takes all fds, otherwise only those with O_CLOEXEC set (1) or unset (0) */
                FDSet **_s) {
        _cleanup_closedir_ DIR *d = NULL;
        int r = 0;
        FDSet *s;

        assert(_s);

        /* Creates an fdset and fills in all currently open file descriptors. Also set all collected fds
         * to CLOEXEC. */

        d = opendir("/proc/self/fd");
        if (!d)
                return -errno;

        s = fdset_new();
        if (!s) {
                r = -ENOMEM;
                goto finish;
        }

        FOREACH_DIRENT(de, d, return -errno) {
                int fd = -1;

                r = safe_atoi(de->d_name, &fd);
                if (r < 0)
                        goto finish;

                if (fd < 3)
                        continue;

                if (fd == dirfd(d))
                        continue;

                if (filter_cloexec >= 0) {
                        int fl;

                        /* If user asked for that filter by O_CLOEXEC. This is useful so that fds that have
                         * been passed in can be collected and fds which have been created locally can be
                         * ignored, under the assumption that only the latter have O_CLOEXEC set. */

                        fl = fcntl(fd, F_GETFD);
                        if (fl < 0)
                                return -errno;

                        if (FLAGS_SET(fl, FD_CLOEXEC) != !!filter_cloexec)
                                continue;
                }

                /* We need to set CLOEXEC manually only if we're collecting non-CLOEXEC fds. */
                if (filter_cloexec <= 0) {
                        r = fd_cloexec(fd, true);
                        if (r < 0)
                                return r;
                }

                r = fdset_put(s, fd);
                if (r < 0)
                        goto finish;
        }

        r = 0;
        *_s = TAKE_PTR(s);

finish:
        /* We won't close the fds here! */
        if (s)
                set_free(MAKE_SET(s));

        return r;
}

int fdset_cloexec(FDSet *fds, bool b) {
        void *p;
        int r;

        assert(fds);

        SET_FOREACH(p, MAKE_SET(fds)) {
                r = fd_cloexec(PTR_TO_FD(p), b);
                if (r < 0)
                        return r;
        }

        return 0;
}

int fdset_new_listen_fds(FDSet **_s, bool unset) {
        int n, fd, r;
        FDSet *s;

        assert(_s);

        /* Creates an fdset and fills in all passed file descriptors */

        s = fdset_new();
        if (!s) {
                r = -ENOMEM;
                goto fail;
        }

        n = sd_listen_fds(unset);
        for (fd = SD_LISTEN_FDS_START; fd < SD_LISTEN_FDS_START + n; fd ++) {
                r = fdset_put(s, fd);
                if (r < 0)
                        goto fail;
        }

        *_s = s;
        return 0;

fail:
        if (s)
                set_free(MAKE_SET(s));

        return r;
}

int fdset_close_others(FDSet *fds) {
        void *e;
        int *a = NULL;
        size_t j = 0, m;

        m = fdset_size(fds);

        if (m > 0) {
                a = newa(int, m);
                SET_FOREACH(e, MAKE_SET(fds))
                        a[j++] = PTR_TO_FD(e);
        }

        assert(j == m);

        return close_all_fds(a, j);
}

unsigned fdset_size(FDSet *fds) {
        return set_size(MAKE_SET(fds));
}

bool fdset_isempty(FDSet *fds) {
        return set_isempty(MAKE_SET(fds));
}

int fdset_iterate(FDSet *s, Iterator *i) {
        void *p;

        if (!set_iterate(MAKE_SET(s), i, &p))
                return -ENOENT;

        return PTR_TO_FD(p);
}

int fdset_steal_first(FDSet *fds) {
        void *p;

        p = set_steal_first(MAKE_SET(fds));
        if (!p)
                return -ENOENT;

        return PTR_TO_FD(p);
}
