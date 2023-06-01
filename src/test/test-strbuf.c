/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <stdlib.h>

#include "nulstr-util.h"
#include "strbuf.h"
#include "string-util.h"
#include "strv.h"
#include "tests.h"

static ssize_t add_string(struct strbuf *sb, const char *s) {
        return strbuf_add_string(sb, s, strlen(s));
}

TEST(strbuf) {
        _cleanup_(strbuf_freep) struct strbuf *sb = NULL;
        _cleanup_strv_free_ char **l = NULL;
        ssize_t a, b, c, d, e, f, g, h;

        sb = strbuf_new();

        a = add_string(sb, "waldo");
        b = add_string(sb, "foo");
        c = add_string(sb, "bar");
        d = add_string(sb, "waldo");   /* duplicate */
        e = add_string(sb, "aldo");    /* duplicate */
        f = add_string(sb, "do");      /* duplicate */
        g = add_string(sb, "waldorf"); /* not a duplicate: matches from tail */
        h = add_string(sb, "");

        /* check the content of the buffer directly */
        l = strv_parse_nulstr(sb->buf, sb->len);
        assert_se(l);

        assert_se(streq(l[0], "")); /* root */
        assert_se(streq(l[1], "waldo"));
        assert_se(streq(l[2], "foo"));
        assert_se(streq(l[3], "bar"));
        assert_se(streq(l[4], "waldorf"));
        assert_se(l[5] == NULL);

        assert_se(sb->nodes_count == 5); /* root + 4 non-duplicates */
        assert_se(sb->dedup_count == 4);
        assert_se(sb->in_count == 8);

        assert_se(sb->in_len == 29);    /* length of all strings added */
        assert_se(sb->dedup_len == 11); /* length of all strings duplicated */
        assert_se(sb->len == 23);       /* buffer length: in - dedup + \0 for each node */

        /* check the returned offsets and the respective content in the buffer */
        assert_se(a == 1);
        assert_se(b == 7);
        assert_se(c == 11);
        assert_se(d == 1);
        assert_se(e == 2);
        assert_se(f == 4);
        assert_se(g == 15);
        assert_se(h == 0);

        assert_se(streq(sb->buf + a, "waldo"));
        assert_se(streq(sb->buf + b, "foo"));
        assert_se(streq(sb->buf + c, "bar"));
        assert_se(streq(sb->buf + d, "waldo"));
        assert_se(streq(sb->buf + e, "aldo"));
        assert_se(streq(sb->buf + f, "do"));
        assert_se(streq(sb->buf + g, "waldorf"));
        assert_se(streq(sb->buf + h, ""));

        strbuf_complete(sb);
        assert_se(sb->root == NULL);
}

DEFINE_TEST_MAIN(LOG_INFO);
