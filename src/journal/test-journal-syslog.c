/* SPDX-License-Identifier: LGPL-2.1+ */
/***
  This file is part of systemd.

  Copyright 2011 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include "alloc-util.h"
#include "journald-syslog.h"
#include "macro.h"
#include "string-util.h"

static void test_syslog_parse_identifier(const char *str,
                                         const char *ident, const char *pid, const char *rest, int ret) {
        const char *buf = str;
        _cleanup_free_ char *ident2 = NULL, *pid2 = NULL;
        int ret2;

        ret2 = syslog_parse_identifier(&buf, &ident2, &pid2);

        assert_se(ret == ret2);
        assert_se(ident == ident2 || streq_ptr(ident, ident2));
        assert_se(pid == pid2 || streq_ptr(pid, pid2));
        assert_se(streq(buf, rest));
}

int main(void) {
        test_syslog_parse_identifier("pidu[111]: xxx", "pidu", "111", "xxx", 11);
        test_syslog_parse_identifier("pidu: xxx", "pidu", NULL, "xxx", 6);
        test_syslog_parse_identifier("pidu:  xxx", "pidu", NULL, " xxx", 6);
        test_syslog_parse_identifier("pidu xxx", NULL, NULL, "pidu xxx", 0);
        test_syslog_parse_identifier("   pidu xxx", NULL, NULL, "   pidu xxx", 0);
        test_syslog_parse_identifier("", NULL, NULL, "", 0);
        test_syslog_parse_identifier("  ", NULL, NULL, "  ", 0);
        test_syslog_parse_identifier(":", "", NULL, "", 1);
        test_syslog_parse_identifier(":  ", "", NULL, " ", 2);
        test_syslog_parse_identifier("pidu:", "pidu", NULL, "", 5);
        test_syslog_parse_identifier("pidu: ", "pidu", NULL, "", 6);
        test_syslog_parse_identifier("pidu : ", NULL, NULL, "pidu : ", 0);

        return 0;
}
