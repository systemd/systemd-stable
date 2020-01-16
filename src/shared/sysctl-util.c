/* SPDX-License-Identifier: LGPL-2.1+ */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "fd-util.h"
#include "fileio.h"
#include "log.h"
#include "macro.h"
#include "path-util.h"
#include "string-util.h"
#include "sysctl-util.h"

char *sysctl_normalize(char *s) {
        char *n;

        n = strpbrk(s, "/.");

        /* If the first separator is a slash, the path is
         * assumed to be normalized and slashes remain slashes
         * and dots remains dots. */

        if (n && *n == '.')
                /* Dots become slashes and slashes become dots. Fun. */
                do {
                        if (*n == '.')
                                *n = '/';
                        else
                                *n = '.';

                        n = strpbrk(n + 1, "/.");
                } while (n);

        path_simplify(s, true);

        /* Kill the leading slash, but keep the first character of the string in the same place. */
        if (*s == '/' && *(s+1))
                memmove(s, s+1, strlen(s));

        return s;
}

int sysctl_write(const char *property, const char *value) {
        char *p;
        _cleanup_close_ int fd = -1;

        assert(property);
        assert(value);

        log_debug("Setting '%s' to '%.*s'.", property, (int) strcspn(value, NEWLINE), value);

        p = strjoina("/proc/sys/", property);
        fd = open(p, O_WRONLY|O_CLOEXEC);
        if (fd < 0)
                return -errno;

        if (!endswith(value, "\n"))
                value = strjoina(value, "\n");

        if (write(fd, value, strlen(value)) < 0)
                return -errno;

        return 0;
}

int sysctl_read(const char *property, char **content) {
        char *p;

        assert(property);
        assert(content);

        p = strjoina("/proc/sys/", property);
        return read_full_file(p, content, NULL);
}
