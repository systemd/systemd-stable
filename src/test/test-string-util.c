/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "alloc-util.h"
#include "locale-util.h"
#include "macro.h"
#include "string-util.h"
#include "strv.h"
#include "tests.h"
#include "utf8.h"
#include "util.h"

TEST(string_erase) {
        char *x;
        x = strdupa_safe("");
        assert_se(streq(string_erase(x), ""));

        x = strdupa_safe("1");
        assert_se(streq(string_erase(x), ""));

        x = strdupa_safe("123456789");
        assert_se(streq(string_erase(x), ""));

        assert_se(x[1] == '\0');
        assert_se(x[2] == '\0');
        assert_se(x[3] == '\0');
        assert_se(x[4] == '\0');
        assert_se(x[5] == '\0');
        assert_se(x[6] == '\0');
        assert_se(x[7] == '\0');
        assert_se(x[8] == '\0');
        assert_se(x[9] == '\0');
}

static void test_free_and_strndup_one(char **t, const char *src, size_t l, const char *expected, bool change) {
        log_debug("%s: \"%s\", \"%s\", %zd (expect \"%s\", %s)",
                  __func__, strnull(*t), strnull(src), l, strnull(expected), yes_no(change));

        int r = free_and_strndup(t, src, l);
        assert_se(streq_ptr(*t, expected));
        assert_se(r == change); /* check that change occurs only when necessary */
}

TEST(free_and_strndup) {
        static const struct test_case {
                const char *src;
                size_t len;
                const char *expected;
        } cases[] = {
                     {"abc", 0, ""},
                     {"abc", 0, ""},
                     {"abc", 1, "a"},
                     {"abc", 2, "ab"},
                     {"abc", 3, "abc"},
                     {"abc", 4, "abc"},
                     {"abc", 5, "abc"},
                     {"abc", 5, "abc"},
                     {"abc", 4, "abc"},
                     {"abc", 3, "abc"},
                     {"abc", 2, "ab"},
                     {"abc", 1, "a"},
                     {"abc", 0, ""},

                     {"", 0, ""},
                     {"", 1, ""},
                     {"", 2, ""},
                     {"", 0, ""},
                     {"", 1, ""},
                     {"", 2, ""},
                     {"", 2, ""},
                     {"", 1, ""},
                     {"", 0, ""},

                     {NULL, 0, NULL},

                     {"foo", 3, "foo"},
                     {"foobar", 6, "foobar"},
        };

        _cleanup_free_ char *t = NULL;
        const char *prev_expected = t;

        for (unsigned i = 0; i < ELEMENTSOF(cases); i++) {
                test_free_and_strndup_one(&t,
                                          cases[i].src, cases[i].len, cases[i].expected,
                                          !streq_ptr(cases[i].expected, prev_expected));
                prev_expected = t;
        }
}

TEST(ascii_strcasecmp_n) {
        assert_se(ascii_strcasecmp_n("", "", 0) == 0);
        assert_se(ascii_strcasecmp_n("", "", 1) == 0);
        assert_se(ascii_strcasecmp_n("", "a", 1) < 0);
        assert_se(ascii_strcasecmp_n("", "a", 2) < 0);
        assert_se(ascii_strcasecmp_n("a", "", 1) > 0);
        assert_se(ascii_strcasecmp_n("a", "", 2) > 0);
        assert_se(ascii_strcasecmp_n("a", "a", 1) == 0);
        assert_se(ascii_strcasecmp_n("a", "a", 2) == 0);
        assert_se(ascii_strcasecmp_n("a", "b", 1) < 0);
        assert_se(ascii_strcasecmp_n("a", "b", 2) < 0);
        assert_se(ascii_strcasecmp_n("b", "a", 1) > 0);
        assert_se(ascii_strcasecmp_n("b", "a", 2) > 0);
        assert_se(ascii_strcasecmp_n("xxxxyxxxx", "xxxxYxxxx", 9) == 0);
        assert_se(ascii_strcasecmp_n("xxxxxxxxx", "xxxxyxxxx", 9) < 0);
        assert_se(ascii_strcasecmp_n("xxxxXxxxx", "xxxxyxxxx", 9) < 0);
        assert_se(ascii_strcasecmp_n("xxxxxxxxx", "xxxxYxxxx", 9) < 0);
        assert_se(ascii_strcasecmp_n("xxxxXxxxx", "xxxxYxxxx", 9) < 0);

        assert_se(ascii_strcasecmp_n("xxxxYxxxx", "xxxxYxxxx", 9) == 0);
        assert_se(ascii_strcasecmp_n("xxxxyxxxx", "xxxxxxxxx", 9) > 0);
        assert_se(ascii_strcasecmp_n("xxxxyxxxx", "xxxxXxxxx", 9) > 0);
        assert_se(ascii_strcasecmp_n("xxxxYxxxx", "xxxxxxxxx", 9) > 0);
        assert_se(ascii_strcasecmp_n("xxxxYxxxx", "xxxxXxxxx", 9) > 0);
}

TEST(ascii_strcasecmp_nn) {
        assert_se(ascii_strcasecmp_nn("", 0, "", 0) == 0);
        assert_se(ascii_strcasecmp_nn("", 0, "", 1) < 0);
        assert_se(ascii_strcasecmp_nn("", 1, "", 0) > 0);
        assert_se(ascii_strcasecmp_nn("", 1, "", 1) == 0);

        assert_se(ascii_strcasecmp_nn("aaaa", 4, "aaAa", 4) == 0);
        assert_se(ascii_strcasecmp_nn("aaa", 3, "aaAa", 4) < 0);
        assert_se(ascii_strcasecmp_nn("aaa", 4, "aaAa", 4) < 0);
        assert_se(ascii_strcasecmp_nn("aaaa", 4, "aaA", 3) > 0);
        assert_se(ascii_strcasecmp_nn("aaaa", 4, "AAA", 4) > 0);

        assert_se(ascii_strcasecmp_nn("aaaa", 4, "bbbb", 4) < 0);
        assert_se(ascii_strcasecmp_nn("aaAA", 4, "BBbb", 4) < 0);
        assert_se(ascii_strcasecmp_nn("BBbb", 4, "aaaa", 4) > 0);
}

TEST(cellescape) {
        char buf[40];

        assert_se(streq(cellescape(buf, 1, ""), ""));
        assert_se(streq(cellescape(buf, 1, "1"), ""));
        assert_se(streq(cellescape(buf, 1, "12"), ""));

        assert_se(streq(cellescape(buf, 2, ""), ""));
        assert_se(streq(cellescape(buf, 2, "1"), "1"));
        assert_se(streq(cellescape(buf, 2, "12"), "."));
        assert_se(streq(cellescape(buf, 2, "123"), "."));

        assert_se(streq(cellescape(buf, 3, ""), ""));
        assert_se(streq(cellescape(buf, 3, "1"), "1"));
        assert_se(streq(cellescape(buf, 3, "12"), "12"));
        assert_se(streq(cellescape(buf, 3, "123"), ".."));
        assert_se(streq(cellescape(buf, 3, "1234"), ".."));

        assert_se(streq(cellescape(buf, 4, ""), ""));
        assert_se(streq(cellescape(buf, 4, "1"), "1"));
        assert_se(streq(cellescape(buf, 4, "12"), "12"));
        assert_se(streq(cellescape(buf, 4, "123"), "123"));
        assert_se(streq(cellescape(buf, 4, "1234"), is_locale_utf8() ? "…" : "..."));
        assert_se(streq(cellescape(buf, 4, "12345"), is_locale_utf8() ? "…" : "..."));

        assert_se(streq(cellescape(buf, 5, ""), ""));
        assert_se(streq(cellescape(buf, 5, "1"), "1"));
        assert_se(streq(cellescape(buf, 5, "12"), "12"));
        assert_se(streq(cellescape(buf, 5, "123"), "123"));
        assert_se(streq(cellescape(buf, 5, "1234"), "1234"));
        assert_se(streq(cellescape(buf, 5, "12345"), is_locale_utf8() ? "1…" : "1..."));
        assert_se(streq(cellescape(buf, 5, "123456"), is_locale_utf8() ? "1…" : "1..."));

        assert_se(streq(cellescape(buf, 1, "\020"), ""));
        assert_se(streq(cellescape(buf, 2, "\020"), "."));
        assert_se(streq(cellescape(buf, 3, "\020"), ".."));
        assert_se(streq(cellescape(buf, 4, "\020"), is_locale_utf8() ? "…" : "..."));
        assert_se(streq(cellescape(buf, 5, "\020"), "\\020"));

        assert_se(streq(cellescape(buf, 5, "1234\020"), is_locale_utf8() ? "1…" : "1..."));
        assert_se(streq(cellescape(buf, 6, "1234\020"), is_locale_utf8() ? "12…" : "12..."));
        assert_se(streq(cellescape(buf, 7, "1234\020"), is_locale_utf8() ? "123…" : "123..."));
        assert_se(streq(cellescape(buf, 8, "1234\020"), is_locale_utf8() ? "1234…" : "1234..."));
        assert_se(streq(cellescape(buf, 9, "1234\020"), "1234\\020"));

        assert_se(streq(cellescape(buf, 1, "\t\n"), ""));
        assert_se(streq(cellescape(buf, 2, "\t\n"), "."));
        assert_se(streq(cellescape(buf, 3, "\t\n"), ".."));
        assert_se(streq(cellescape(buf, 4, "\t\n"), is_locale_utf8() ? "…" : "..."));
        assert_se(streq(cellescape(buf, 5, "\t\n"), "\\t\\n"));

        assert_se(streq(cellescape(buf, 5, "1234\t\n"), is_locale_utf8() ? "1…" : "1..."));
        assert_se(streq(cellescape(buf, 6, "1234\t\n"), is_locale_utf8() ? "12…" : "12..."));
        assert_se(streq(cellescape(buf, 7, "1234\t\n"), is_locale_utf8() ? "123…" : "123..."));
        assert_se(streq(cellescape(buf, 8, "1234\t\n"), is_locale_utf8() ? "1234…" : "1234..."));
        assert_se(streq(cellescape(buf, 9, "1234\t\n"), "1234\\t\\n"));

        assert_se(streq(cellescape(buf, 4, "x\t\020\n"), is_locale_utf8() ? "…" : "..."));
        assert_se(streq(cellescape(buf, 5, "x\t\020\n"), is_locale_utf8() ? "x…" : "x..."));
        assert_se(streq(cellescape(buf, 6, "x\t\020\n"), is_locale_utf8() ? "x…" : "x..."));
        assert_se(streq(cellescape(buf, 7, "x\t\020\n"), is_locale_utf8() ? "x\\t…" : "x\\t..."));
        assert_se(streq(cellescape(buf, 8, "x\t\020\n"), is_locale_utf8() ? "x\\t…" : "x\\t..."));
        assert_se(streq(cellescape(buf, 9, "x\t\020\n"), is_locale_utf8() ? "x\\t…" : "x\\t..."));
        assert_se(streq(cellescape(buf, 10, "x\t\020\n"), "x\\t\\020\\n"));

        assert_se(streq(cellescape(buf, 6, "1\011"), "1\\t"));
        assert_se(streq(cellescape(buf, 6, "1\020"), "1\\020"));
        assert_se(streq(cellescape(buf, 6, "1\020x"), is_locale_utf8() ? "1…" : "1..."));

        assert_se(streq(cellescape(buf, 40, "1\020"), "1\\020"));
        assert_se(streq(cellescape(buf, 40, "1\020x"), "1\\020x"));

        assert_se(streq(cellescape(buf, 40, "\a\b\f\n\r\t\v\\\"'"), "\\a\\b\\f\\n\\r\\t\\v\\\\\\\"\\'"));
        assert_se(streq(cellescape(buf, 6, "\a\b\f\n\r\t\v\\\"'"), is_locale_utf8() ? "\\a…" : "\\a..."));
        assert_se(streq(cellescape(buf, 7, "\a\b\f\n\r\t\v\\\"'"), is_locale_utf8() ? "\\a…" : "\\a..."));
        assert_se(streq(cellescape(buf, 8, "\a\b\f\n\r\t\v\\\"'"), is_locale_utf8() ? "\\a\\b…" : "\\a\\b..."));

        assert_se(streq(cellescape(buf, sizeof buf, "1\020"), "1\\020"));
        assert_se(streq(cellescape(buf, sizeof buf, "1\020x"), "1\\020x"));
}

TEST(streq_ptr) {
        assert_se(streq_ptr(NULL, NULL));
        assert_se(!streq_ptr("abc", "cdef"));
}

TEST(strstrip) {
        char *ret, input[] = "   hello, waldo.   ";

        ret = strstrip(input);
        assert_se(streq(ret, "hello, waldo."));
}

TEST(strextend) {
        _cleanup_free_ char *str = NULL;

        assert_se(strextend(&str, NULL));
        assert_se(streq_ptr(str, ""));
        assert_se(strextend(&str, "", "0", "", "", "123"));
        assert_se(streq_ptr(str, "0123"));
        assert_se(strextend(&str, "456", "78", "9"));
        assert_se(streq_ptr(str, "0123456789"));
}

TEST(strextend_with_separator) {
        _cleanup_free_ char *str = NULL;

        assert_se(strextend_with_separator(&str, NULL, NULL));
        assert_se(streq_ptr(str, ""));
        str = mfree(str);

        assert_se(strextend_with_separator(&str, "...", NULL));
        assert_se(streq_ptr(str, ""));
        assert_se(strextend_with_separator(&str, "...", NULL));
        assert_se(streq_ptr(str, ""));
        str = mfree(str);

        assert_se(strextend_with_separator(&str, "xyz", "a", "bb", "ccc"));
        assert_se(streq_ptr(str, "axyzbbxyzccc"));
        str = mfree(str);

        assert_se(strextend_with_separator(&str, ",", "start", "", "1", "234"));
        assert_se(streq_ptr(str, "start,,1,234"));
        assert_se(strextend_with_separator(&str, ";", "more", "5", "678"));
        assert_se(streq_ptr(str, "start,,1,234;more;5;678"));
}

TEST(strrep) {
        _cleanup_free_ char *one, *three, *zero;
        one = strrep("waldo", 1);
        three = strrep("waldo", 3);
        zero = strrep("waldo", 0);

        assert_se(streq(one, "waldo"));
        assert_se(streq(three, "waldowaldowaldo"));
        assert_se(streq(zero, ""));
}

TEST(string_has_cc) {
        assert_se(string_has_cc("abc\1", NULL));
        assert_se(string_has_cc("abc\x7f", NULL));
        assert_se(string_has_cc("abc\x7f", NULL));
        assert_se(string_has_cc("abc\t\x7f", "\t"));
        assert_se(string_has_cc("abc\t\x7f", "\t"));
        assert_se(string_has_cc("\x7f", "\t"));
        assert_se(string_has_cc("\x7f", "\t\a"));

        assert_se(!string_has_cc("abc\t\t", "\t"));
        assert_se(!string_has_cc("abc\t\t\a", "\t\a"));
        assert_se(!string_has_cc("a\ab\tc", "\t\a"));
}

TEST(ascii_strlower) {
        char a[] = "AabBcC Jk Ii Od LKJJJ kkd LK";
        assert_se(streq(ascii_strlower(a), "aabbcc jk ii od lkjjj kkd lk"));
}

TEST(strshorten) {
        char s[] = "foobar";

        assert_se(strlen(strshorten(s, 6)) == 6);
        assert_se(strlen(strshorten(s, 12)) == 6);
        assert_se(strlen(strshorten(s, 2)) == 2);
        assert_se(strlen(strshorten(s, 0)) == 0);
}

TEST(strjoina) {
        char *actual;

        actual = strjoina("", "foo", "bar");
        assert_se(streq(actual, "foobar"));

        actual = strjoina("foo", "bar", "baz");
        assert_se(streq(actual, "foobarbaz"));

        actual = strjoina("foo", "", "bar", "baz");
        assert_se(streq(actual, "foobarbaz"));

        actual = strjoina("foo");
        assert_se(streq(actual, "foo"));

        actual = strjoina(NULL);
        assert_se(streq(actual, ""));

        actual = strjoina(NULL, "foo");
        assert_se(streq(actual, ""));

        actual = strjoina("foo", NULL, "bar");
        assert_se(streq(actual, "foo"));

        actual = strjoina("/sys/fs/cgroup/", "dn", "/a/b/c", "/cgroup.procs");
        assert_se(streq(actual, "/sys/fs/cgroup/dn/a/b/c/cgroup.procs"));

        actual = strjoina("/sys/fs/cgroup/", "dn", NULL, NULL);
        assert_se(streq(actual, "/sys/fs/cgroup/dn"));
}

TEST(strjoin) {
        char *actual;

        actual = strjoin("", "foo", "bar");
        assert_se(streq(actual, "foobar"));
        mfree(actual);

        actual = strjoin("foo", "bar", "baz");
        assert_se(streq(actual, "foobarbaz"));
        mfree(actual);

        actual = strjoin("foo", "", "bar", "baz");
        assert_se(streq(actual, "foobarbaz"));
        mfree(actual);

        actual = strjoin("foo", NULL);
        assert_se(streq(actual, "foo"));
        mfree(actual);

        actual = strjoin(NULL, NULL);
        assert_se(streq(actual, ""));
        mfree(actual);

        actual = strjoin(NULL, "foo");
        assert_se(streq(actual, ""));
        mfree(actual);

        actual = strjoin("foo", NULL, "bar");
        assert_se(streq(actual, "foo"));
        mfree(actual);
}

TEST(strcmp_ptr) {
        assert_se(strcmp_ptr(NULL, NULL) == 0);
        assert_se(strcmp_ptr("", NULL) > 0);
        assert_se(strcmp_ptr("foo", NULL) > 0);
        assert_se(strcmp_ptr(NULL, "") < 0);
        assert_se(strcmp_ptr(NULL, "bar") < 0);
        assert_se(strcmp_ptr("foo", "bar") > 0);
        assert_se(strcmp_ptr("bar", "baz") < 0);
        assert_se(strcmp_ptr("foo", "foo") == 0);
        assert_se(strcmp_ptr("", "") == 0);
}

TEST(foreach_word) {
        const char *test = "test abc d\te   f   ";
        const char * const expected[] = {
                "test",
                "abc",
                "d",
                "e",
                "f",
        };

        size_t i = 0;
        int r;
        for (const char *p = test;;) {
                _cleanup_free_ char *word = NULL;

                r = extract_first_word(&p, &word, NULL, 0);
                if (r == 0) {
                        assert_se(i == ELEMENTSOF(expected));
                        break;
                }
                assert_se(r > 0);

                assert_se(streq(expected[i++], word));
        }
}

static void check(const char *test, char** expected, bool trailing) {
        size_t i = 0;
        int r;

        printf("<<<%s>>>\n", test);
        for (;;) {
                _cleanup_free_ char *word = NULL;

                r = extract_first_word(&test, &word, NULL, EXTRACT_UNQUOTE);
                if (r == 0) {
                        assert_se(!trailing);
                        break;
                } else if (r < 0) {
                        assert_se(trailing);
                        break;
                }

                assert_se(streq(word, expected[i++]));
                printf("<%s>\n", word);
        }
        assert_se(expected[i] == NULL);
}

TEST(foreach_word_quoted) {
        check("test a b c 'd' e '' '' hhh '' '' \"a b c\"",
              STRV_MAKE("test",
                        "a",
                        "b",
                        "c",
                        "d",
                        "e",
                        "",
                        "",
                        "hhh",
                        "",
                        "",
                        "a b c"),
              false);

        check("test \"xxx",
              STRV_MAKE("test"),
              true);

        check("test\\",
              STRV_MAKE_EMPTY,
              true);
}

TEST(endswith) {
        assert_se(endswith("foobar", "bar"));
        assert_se(endswith("foobar", ""));
        assert_se(endswith("foobar", "foobar"));
        assert_se(endswith("", ""));

        assert_se(!endswith("foobar", "foo"));
        assert_se(!endswith("foobar", "foobarfoofoo"));
}

TEST(endswith_no_case) {
        assert_se(endswith_no_case("fooBAR", "bar"));
        assert_se(endswith_no_case("foobar", ""));
        assert_se(endswith_no_case("foobar", "FOOBAR"));
        assert_se(endswith_no_case("", ""));

        assert_se(!endswith_no_case("foobar", "FOO"));
        assert_se(!endswith_no_case("foobar", "FOOBARFOOFOO"));
}

TEST(delete_chars) {
        char *s, input[] = "   hello, waldo.   abc";

        s = delete_chars(input, WHITESPACE);
        assert_se(streq(s, "hello,waldo.abc"));
        assert_se(s == input);
}

TEST(delete_trailing_chars) {
        char *s,
                input1[] = " \n \r k \n \r ",
                input2[] = "kkkkthiskkkiskkkaktestkkk",
                input3[] = "abcdef";

        s = delete_trailing_chars(input1, WHITESPACE);
        assert_se(streq(s, " \n \r k"));
        assert_se(s == input1);

        s = delete_trailing_chars(input2, "kt");
        assert_se(streq(s, "kkkkthiskkkiskkkaktes"));
        assert_se(s == input2);

        s = delete_trailing_chars(input3, WHITESPACE);
        assert_se(streq(s, "abcdef"));
        assert_se(s == input3);

        s = delete_trailing_chars(input3, "fe");
        assert_se(streq(s, "abcd"));
        assert_se(s == input3);
}

TEST(delete_trailing_slashes) {
        char s1[] = "foobar//",
             s2[] = "foobar/",
             s3[] = "foobar",
             s4[] = "";

        assert_se(streq(delete_trailing_chars(s1, "_"), "foobar//"));
        assert_se(streq(delete_trailing_chars(s1, "/"), "foobar"));
        assert_se(streq(delete_trailing_chars(s2, "/"), "foobar"));
        assert_se(streq(delete_trailing_chars(s3, "/"), "foobar"));
        assert_se(streq(delete_trailing_chars(s4, "/"), ""));
}

TEST(skip_leading_chars) {
        char input1[] = " \n \r k \n \r ",
                input2[] = "kkkkthiskkkiskkkaktestkkk",
                input3[] = "abcdef";

        assert_se(streq(skip_leading_chars(input1, WHITESPACE), "k \n \r "));
        assert_se(streq(skip_leading_chars(input2, "k"), "thiskkkiskkkaktestkkk"));
        assert_se(streq(skip_leading_chars(input2, "tk"), "hiskkkiskkkaktestkkk"));
        assert_se(streq(skip_leading_chars(input3, WHITESPACE), "abcdef"));
        assert_se(streq(skip_leading_chars(input3, "bcaef"), "def"));
}

TEST(in_charset) {
        assert_se(in_charset("dddaaabbbcccc", "abcd"));
        assert_se(!in_charset("dddaaabbbcccc", "abc f"));
}

TEST(split_pair) {
        _cleanup_free_ char *a = NULL, *b = NULL;

        assert_se(split_pair("", "", &a, &b) == -EINVAL);
        assert_se(split_pair("foo=bar", "", &a, &b) == -EINVAL);
        assert_se(split_pair("", "=", &a, &b) == -EINVAL);
        assert_se(split_pair("foo=bar", "=", &a, &b) >= 0);
        assert_se(streq(a, "foo"));
        assert_se(streq(b, "bar"));
        free(a);
        free(b);
        assert_se(split_pair("==", "==", &a, &b) >= 0);
        assert_se(streq(a, ""));
        assert_se(streq(b, ""));
        free(a);
        free(b);

        assert_se(split_pair("===", "==", &a, &b) >= 0);
        assert_se(streq(a, ""));
        assert_se(streq(b, "="));
}

TEST(first_word) {
        assert_se(first_word("Hello", ""));
        assert_se(first_word("Hello", "Hello"));
        assert_se(first_word("Hello world", "Hello"));
        assert_se(first_word("Hello\tworld", "Hello"));
        assert_se(first_word("Hello\nworld", "Hello"));
        assert_se(first_word("Hello\rworld", "Hello"));
        assert_se(first_word("Hello ", "Hello"));

        assert_se(!first_word("Hello", "Hellooo"));
        assert_se(!first_word("Hello", "xxxxx"));
        assert_se(!first_word("Hellooo", "Hello"));
}

TEST(strlen_ptr) {
        assert_se(strlen_ptr("foo") == 3);
        assert_se(strlen_ptr("") == 0);
        assert_se(strlen_ptr(NULL) == 0);
}

TEST(memory_startswith) {
        assert_se(streq(memory_startswith("", 0, ""), ""));
        assert_se(streq(memory_startswith("", 1, ""), ""));
        assert_se(streq(memory_startswith("x", 2, ""), "x"));
        assert_se(!memory_startswith("", 1, "x"));
        assert_se(!memory_startswith("", 1, "xxxxxxxx"));
        assert_se(streq(memory_startswith("xxx", 4, "x"), "xx"));
        assert_se(streq(memory_startswith("xxx", 4, "xx"), "x"));
        assert_se(streq(memory_startswith("xxx", 4, "xxx"), ""));
        assert_se(!memory_startswith("xxx", 4, "xxxx"));
}

TEST(memory_startswith_no_case) {
        assert_se(streq(memory_startswith_no_case("", 0, ""), ""));
        assert_se(streq(memory_startswith_no_case("", 1, ""), ""));
        assert_se(streq(memory_startswith_no_case("x", 2, ""), "x"));
        assert_se(streq(memory_startswith_no_case("X", 2, ""), "X"));
        assert_se(!memory_startswith_no_case("", 1, "X"));
        assert_se(!memory_startswith_no_case("", 1, "xxxxXXXX"));
        assert_se(streq(memory_startswith_no_case("xxx", 4, "X"), "xx"));
        assert_se(streq(memory_startswith_no_case("XXX", 4, "x"), "XX"));
        assert_se(streq(memory_startswith_no_case("XXX", 4, "X"), "XX"));
        assert_se(streq(memory_startswith_no_case("xxx", 4, "XX"), "x"));
        assert_se(streq(memory_startswith_no_case("XXX", 4, "xx"), "X"));
        assert_se(streq(memory_startswith_no_case("XXX", 4, "XX"), "X"));
        assert_se(streq(memory_startswith_no_case("xxx", 4, "XXX"), ""));
        assert_se(streq(memory_startswith_no_case("XXX", 4, "xxx"), ""));
        assert_se(streq(memory_startswith_no_case("XXX", 4, "XXX"), ""));

        assert_se(memory_startswith_no_case((char[2]){'x', 'x'}, 2, "xx"));
        assert_se(memory_startswith_no_case((char[2]){'x', 'X'}, 2, "xX"));
        assert_se(memory_startswith_no_case((char[2]){'X', 'x'}, 2, "Xx"));
        assert_se(memory_startswith_no_case((char[2]){'X', 'X'}, 2, "XX"));
}

static void test_string_truncate_lines_one(const char *input, size_t n_lines, const char *output, bool truncation) {
        _cleanup_free_ char *b = NULL;
        int k;

        assert_se((k = string_truncate_lines(input, n_lines, &b)) >= 0);
        assert_se(streq(b, output));
        assert_se(!!k == truncation);
}

TEST(string_truncate_lines) {
        test_string_truncate_lines_one("", 0, "", false);
        test_string_truncate_lines_one("", 1, "", false);
        test_string_truncate_lines_one("", 2, "", false);
        test_string_truncate_lines_one("", 3, "", false);

        test_string_truncate_lines_one("x", 0, "", true);
        test_string_truncate_lines_one("x", 1, "x", false);
        test_string_truncate_lines_one("x", 2, "x", false);
        test_string_truncate_lines_one("x", 3, "x", false);

        test_string_truncate_lines_one("x\n", 0, "", true);
        test_string_truncate_lines_one("x\n", 1, "x", false);
        test_string_truncate_lines_one("x\n", 2, "x", false);
        test_string_truncate_lines_one("x\n", 3, "x", false);

        test_string_truncate_lines_one("x\ny", 0, "", true);
        test_string_truncate_lines_one("x\ny", 1, "x", true);
        test_string_truncate_lines_one("x\ny", 2, "x\ny", false);
        test_string_truncate_lines_one("x\ny", 3, "x\ny", false);

        test_string_truncate_lines_one("x\ny\n", 0, "", true);
        test_string_truncate_lines_one("x\ny\n", 1, "x", true);
        test_string_truncate_lines_one("x\ny\n", 2, "x\ny", false);
        test_string_truncate_lines_one("x\ny\n", 3, "x\ny", false);

        test_string_truncate_lines_one("x\ny\nz", 0, "", true);
        test_string_truncate_lines_one("x\ny\nz", 1, "x", true);
        test_string_truncate_lines_one("x\ny\nz", 2, "x\ny", true);
        test_string_truncate_lines_one("x\ny\nz", 3, "x\ny\nz", false);

        test_string_truncate_lines_one("x\ny\nz\n", 0, "", true);
        test_string_truncate_lines_one("x\ny\nz\n", 1, "x", true);
        test_string_truncate_lines_one("x\ny\nz\n", 2, "x\ny", true);
        test_string_truncate_lines_one("x\ny\nz\n", 3, "x\ny\nz", false);

        test_string_truncate_lines_one("\n", 0, "", false);
        test_string_truncate_lines_one("\n", 1, "", false);
        test_string_truncate_lines_one("\n", 2, "", false);
        test_string_truncate_lines_one("\n", 3, "", false);

        test_string_truncate_lines_one("\n\n", 0, "", false);
        test_string_truncate_lines_one("\n\n", 1, "", false);
        test_string_truncate_lines_one("\n\n", 2, "", false);
        test_string_truncate_lines_one("\n\n", 3, "", false);

        test_string_truncate_lines_one("\n\n\n", 0, "", false);
        test_string_truncate_lines_one("\n\n\n", 1, "", false);
        test_string_truncate_lines_one("\n\n\n", 2, "", false);
        test_string_truncate_lines_one("\n\n\n", 3, "", false);

        test_string_truncate_lines_one("\nx\n\n", 0, "", true);
        test_string_truncate_lines_one("\nx\n\n", 1, "", true);
        test_string_truncate_lines_one("\nx\n\n", 2, "\nx", false);
        test_string_truncate_lines_one("\nx\n\n", 3, "\nx", false);

        test_string_truncate_lines_one("\n\nx\n", 0, "", true);
        test_string_truncate_lines_one("\n\nx\n", 1, "", true);
        test_string_truncate_lines_one("\n\nx\n", 2, "", true);
        test_string_truncate_lines_one("\n\nx\n", 3, "\n\nx", false);
}

static void test_string_extract_lines_one(const char *input, size_t i, const char *output, bool more) {
        _cleanup_free_ char *b = NULL;
        int k;

        assert_se((k = string_extract_line(input, i, &b)) >= 0);
        assert_se(streq(b ?: input, output));
        assert_se(!!k == more);
}

TEST(string_extract_line) {
        test_string_extract_lines_one("", 0, "", false);
        test_string_extract_lines_one("", 1, "", false);
        test_string_extract_lines_one("", 2, "", false);
        test_string_extract_lines_one("", 3, "", false);

        test_string_extract_lines_one("x", 0, "x", false);
        test_string_extract_lines_one("x", 1, "", false);
        test_string_extract_lines_one("x", 2, "", false);
        test_string_extract_lines_one("x", 3, "", false);

        test_string_extract_lines_one("x\n", 0, "x", false);
        test_string_extract_lines_one("x\n", 1, "", false);
        test_string_extract_lines_one("x\n", 2, "", false);
        test_string_extract_lines_one("x\n", 3, "", false);

        test_string_extract_lines_one("x\ny", 0, "x", true);
        test_string_extract_lines_one("x\ny", 1, "y", false);
        test_string_extract_lines_one("x\ny", 2, "", false);
        test_string_extract_lines_one("x\ny", 3, "", false);

        test_string_extract_lines_one("x\ny\n", 0, "x", true);
        test_string_extract_lines_one("x\ny\n", 1, "y", false);
        test_string_extract_lines_one("x\ny\n", 2, "", false);
        test_string_extract_lines_one("x\ny\n", 3, "", false);

        test_string_extract_lines_one("x\ny\nz", 0, "x", true);
        test_string_extract_lines_one("x\ny\nz", 1, "y", true);
        test_string_extract_lines_one("x\ny\nz", 2, "z", false);
        test_string_extract_lines_one("x\ny\nz", 3, "", false);

        test_string_extract_lines_one("\n", 0, "", false);
        test_string_extract_lines_one("\n", 1, "", false);
        test_string_extract_lines_one("\n", 2, "", false);
        test_string_extract_lines_one("\n", 3, "", false);

        test_string_extract_lines_one("\n\n", 0, "", true);
        test_string_extract_lines_one("\n\n", 1, "", false);
        test_string_extract_lines_one("\n\n", 2, "", false);
        test_string_extract_lines_one("\n\n", 3, "", false);

        test_string_extract_lines_one("\n\n\n", 0, "", true);
        test_string_extract_lines_one("\n\n\n", 1, "", true);
        test_string_extract_lines_one("\n\n\n", 2, "", false);
        test_string_extract_lines_one("\n\n\n", 3, "", false);

        test_string_extract_lines_one("\n\n\n\n", 0, "", true);
        test_string_extract_lines_one("\n\n\n\n", 1, "", true);
        test_string_extract_lines_one("\n\n\n\n", 2, "", true);
        test_string_extract_lines_one("\n\n\n\n", 3, "", false);

        test_string_extract_lines_one("\nx\n\n\n", 0, "", true);
        test_string_extract_lines_one("\nx\n\n\n", 1, "x", true);
        test_string_extract_lines_one("\nx\n\n\n", 2, "", true);
        test_string_extract_lines_one("\nx\n\n\n", 3, "", false);

        test_string_extract_lines_one("\n\nx\n\n", 0, "", true);
        test_string_extract_lines_one("\n\nx\n\n", 1, "", true);
        test_string_extract_lines_one("\n\nx\n\n", 2, "x", true);
        test_string_extract_lines_one("\n\nx\n\n", 3, "", false);

        test_string_extract_lines_one("\n\n\nx\n", 0, "", true);
        test_string_extract_lines_one("\n\n\nx\n", 1, "", true);
        test_string_extract_lines_one("\n\n\nx\n", 2, "", true);
        test_string_extract_lines_one("\n\n\nx\n", 3, "x", false);
}

TEST(string_contains_word_strv) {
        const char *w;

        assert_se(string_contains_word_strv("a b cc", NULL, STRV_MAKE("a", "b"), NULL));

        assert_se(string_contains_word_strv("a b cc", NULL, STRV_MAKE("a", "b"), &w));
        assert_se(streq(w, "a"));

        assert_se(!string_contains_word_strv("a b cc", NULL, STRV_MAKE("d"), &w));
        assert_se(w == NULL);

        assert_se(string_contains_word_strv("a b cc", NULL, STRV_MAKE("b", "a"), &w));
        assert_se(streq(w, "a"));

        assert_se(string_contains_word_strv("b a b cc", NULL, STRV_MAKE("b", "a", "b"), &w));
        assert_se(streq(w, "b"));

        assert_se(string_contains_word_strv("a b cc", NULL, STRV_MAKE("b", ""), &w));
        assert_se(streq(w, "b"));

        assert_se(!string_contains_word_strv("a b cc", NULL, STRV_MAKE(""), &w));
        assert_se(w == NULL);

        assert_se(string_contains_word_strv("a b  cc", " ", STRV_MAKE(""), &w));
        assert_se(streq(w, ""));
}

TEST(string_contains_word) {
        assert_se( string_contains_word("a b cc", NULL, "a"));
        assert_se( string_contains_word("a b cc", NULL, "b"));
        assert_se(!string_contains_word("a b cc", NULL, "c"));
        assert_se( string_contains_word("a b cc", NULL, "cc"));
        assert_se(!string_contains_word("a b cc", NULL, "d"));
        assert_se(!string_contains_word("a b cc", NULL, "a b"));
        assert_se(!string_contains_word("a b cc", NULL, "a b c"));
        assert_se(!string_contains_word("a b cc", NULL, "b c"));
        assert_se(!string_contains_word("a b cc", NULL, "b cc"));
        assert_se(!string_contains_word("a b cc", NULL, "a "));
        assert_se(!string_contains_word("a b cc", NULL, " b "));
        assert_se(!string_contains_word("a b cc", NULL, " cc"));

        assert_se( string_contains_word("  a  b\t\tcc", NULL, "a"));
        assert_se( string_contains_word("  a  b\t\tcc", NULL, "b"));
        assert_se(!string_contains_word("  a  b\t\tcc", NULL, "c"));
        assert_se( string_contains_word("  a  b\t\tcc", NULL, "cc"));
        assert_se(!string_contains_word("  a  b\t\tcc", NULL, "d"));
        assert_se(!string_contains_word("  a  b\t\tcc", NULL, "a b"));
        assert_se(!string_contains_word("  a  b\t\tcc", NULL, "a b\t\tc"));
        assert_se(!string_contains_word("  a  b\t\tcc", NULL, "b\t\tc"));
        assert_se(!string_contains_word("  a  b\t\tcc", NULL, "b\t\tcc"));
        assert_se(!string_contains_word("  a  b\t\tcc", NULL, "a "));
        assert_se(!string_contains_word("  a  b\t\tcc", NULL, " b "));
        assert_se(!string_contains_word("  a  b\t\tcc", NULL, " cc"));

        assert_se(!string_contains_word("  a  b\t\tcc", NULL, ""));
        assert_se(!string_contains_word("  a  b\t\tcc", NULL, " "));
        assert_se(!string_contains_word("  a  b\t\tcc", NULL, "  "));
        assert_se( string_contains_word("  a  b\t\tcc", " ", ""));
        assert_se( string_contains_word("  a  b\t\tcc", "\t", ""));
        assert_se( string_contains_word("  a  b\t\tcc", WHITESPACE, ""));

        assert_se( string_contains_word("a:b:cc", ":#", "a"));
        assert_se( string_contains_word("a:b:cc", ":#", "b"));
        assert_se(!string_contains_word("a:b:cc", ":#", "c"));
        assert_se( string_contains_word("a:b:cc", ":#", "cc"));
        assert_se(!string_contains_word("a:b:cc", ":#", "d"));
        assert_se(!string_contains_word("a:b:cc", ":#", "a:b"));
        assert_se(!string_contains_word("a:b:cc", ":#", "a:b:c"));
        assert_se(!string_contains_word("a:b:cc", ":#", "b:c"));
        assert_se(!string_contains_word("a#b#cc", ":#", "b:cc"));
        assert_se( string_contains_word("a#b#cc", ":#", "b"));
        assert_se( string_contains_word("a#b#cc", ":#", "cc"));
        assert_se(!string_contains_word("a:b:cc", ":#", "a:"));
        assert_se(!string_contains_word("a:b cc", ":#", "b"));
        assert_se( string_contains_word("a:b cc", ":#", "b cc"));
        assert_se(!string_contains_word("a:b:cc", ":#", ":cc"));
}

static void test_strverscmp_improved_one(const char *newer, const char *older) {
        log_info("/* %s(%s, %s) */", __func__, strnull(newer), strnull(older));

        assert_se(strverscmp_improved(newer, newer) == 0);
        assert_se(strverscmp_improved(newer, older) >  0);
        assert_se(strverscmp_improved(older, newer) <  0);
        assert_se(strverscmp_improved(older, older) == 0);
}

TEST(strverscmp_improved) {
        static const char * const versions[] = {
                "",
                "~1",
                "ab",
                "abb",
                "abc",
                "0001",
                "002",
                "12",
                "122",
                "122.9",
                "123~rc1",
                "123",
                "123-a",
                "123-a.1",
                "123-a1",
                "123-a1.1",
                "123-3",
                "123-3.1",
                "123^patch1",
                "123^1",
                "123.a-1",
                "123.1-1",
                "123a-1",
                "124",
                NULL,
        };

        STRV_FOREACH(p, versions)
                STRV_FOREACH(q, p + 1)
                        test_strverscmp_improved_one(*q, *p);

        test_strverscmp_improved_one("123.45-67.89", "123.45-67.88");
        test_strverscmp_improved_one("123.45-67.89a", "123.45-67.89");
        test_strverscmp_improved_one("123.45-67.89", "123.45-67.ab");
        test_strverscmp_improved_one("123.45-67.89", "123.45-67.9");
        test_strverscmp_improved_one("123.45-67.89", "123.45-67");
        test_strverscmp_improved_one("123.45-67.89", "123.45-66.89");
        test_strverscmp_improved_one("123.45-67.89", "123.45-9.99");
        test_strverscmp_improved_one("123.45-67.89", "123.42-99.99");
        test_strverscmp_improved_one("123.45-67.89", "123-99.99");

        /* '~' : pre-releases */
        test_strverscmp_improved_one("123.45-67.89", "123~rc1-99.99");
        test_strverscmp_improved_one("123-45.67.89", "123~rc1-99.99");
        test_strverscmp_improved_one("123~rc2-67.89", "123~rc1-99.99");
        test_strverscmp_improved_one("123^aa2-67.89", "123~rc1-99.99");
        test_strverscmp_improved_one("123aa2-67.89", "123~rc1-99.99");

        /* '-' : separator between version and release. */
        test_strverscmp_improved_one("123.45-67.89", "123-99.99");
        test_strverscmp_improved_one("123^aa2-67.89", "123-99.99");
        test_strverscmp_improved_one("123aa2-67.89", "123-99.99");

        /* '^' : patch releases */
        test_strverscmp_improved_one("123.45-67.89", "123^45-67.89");
        test_strverscmp_improved_one("123^aa2-67.89", "123^aa1-99.99");
        test_strverscmp_improved_one("123aa2-67.89", "123^aa2-67.89");

        /* '.' : point release */
        test_strverscmp_improved_one("123aa2-67.89", "123.aa2-67.89");
        test_strverscmp_improved_one("123.ab2-67.89", "123.aa2-67.89");

        /* invalid characters */
        assert_se(strverscmp_improved("123_aa2-67.89", "123aa+2-67.89") == 0);
}

TEST(strextendf) {
        _cleanup_free_ char *p = NULL;

        assert_se(strextendf(&p, "<%i>", 77) >= 0);
        assert_se(streq(p, "<77>"));

        assert_se(strextendf(&p, "<%i>", 99) >= 0);
        assert_se(streq(p, "<77><99>"));

        assert_se(strextendf(&p, "<%80i>", 88) >= 0);
        assert_se(streq(p, "<77><99><                                                                              88>"));

        assert_se(strextendf(&p, "<%08x>", 0x1234) >= 0);
        assert_se(streq(p, "<77><99><                                                                              88><00001234>"));

        p = mfree(p);

        assert_se(strextendf_with_separator(&p, ",", "<%i>", 77) >= 0);
        assert_se(streq(p, "<77>"));

        assert_se(strextendf_with_separator(&p, ",", "<%i>", 99) >= 0);
        assert_se(streq(p, "<77>,<99>"));

        assert_se(strextendf_with_separator(&p, ",", "<%80i>", 88) >= 0);
        assert_se(streq(p, "<77>,<99>,<                                                                              88>"));

        assert_se(strextendf_with_separator(&p, ",", "<%08x>", 0x1234) >= 0);
        assert_se(streq(p, "<77>,<99>,<                                                                              88>,<00001234>"));
}

TEST(string_replace_char) {
        assert_se(streq(string_replace_char(strdupa(""), 'a', 'b'), ""));
        assert_se(streq(string_replace_char(strdupa("abc"), 'a', 'b'), "bbc"));
        assert_se(streq(string_replace_char(strdupa("hoge"), 'a', 'b'), "hoge"));
        assert_se(streq(string_replace_char(strdupa("aaaa"), 'a', 'b'), "bbbb"));
        assert_se(streq(string_replace_char(strdupa("aaaa"), 'a', '\t'), "\t\t\t\t"));
}

TEST(strspn_from_end) {
        assert_se(strspn_from_end(NULL, NULL) == 0);
        assert_se(strspn_from_end("hoge", NULL) == 0);
        assert_se(strspn_from_end(NULL, DIGITS) == 0);
        assert_se(strspn_from_end("", DIGITS) == 0);
        assert_se(strspn_from_end("hoge", DIGITS) == 0);
        assert_se(strspn_from_end("1234", DIGITS) == 4);
        assert_se(strspn_from_end("aaa1234", DIGITS) == 4);
        assert_se(strspn_from_end("aaa1234aaa", DIGITS) == 0);
        assert_se(strspn_from_end("aaa12aa34", DIGITS) == 2);
}

TEST(streq_skip_trailing_chars) {
        /* NULL is WHITESPACE by default*/
        assert_se(streq_skip_trailing_chars("foo bar", "foo bar", NULL));
        assert_se(streq_skip_trailing_chars("foo", "foo", NULL));
        assert_se(streq_skip_trailing_chars("foo bar      ", "foo bar", NULL));
        assert_se(streq_skip_trailing_chars("foo bar", "foo bar\t\t", NULL));
        assert_se(streq_skip_trailing_chars("foo bar  ", "foo bar\t\t", NULL));
        assert_se(streq_skip_trailing_chars("foo\nbar", "foo\nbar", NULL));
        assert_se(streq_skip_trailing_chars("\t\tfoo bar", "\t\tfoo bar", NULL));
        assert_se(streq_skip_trailing_chars(" foo bar\t", " foo bar\n", NULL));

        assert_se(!streq_skip_trailing_chars("foobar", "foo bar", NULL));
        assert_se(!streq_skip_trailing_chars("foo\nbar", "foo\tbar", NULL));
        assert_se(!streq_skip_trailing_chars("\t\nfoo bar", "\t foo bar", NULL));

        assert_se(streq_skip_trailing_chars("foo bar      ", "foo bar", WHITESPACE));
        assert_se(!streq_skip_trailing_chars("foo bar      ", "foo bar", NEWLINE));

        assert_se(streq_skip_trailing_chars(NULL, NULL, NULL));
        assert_se(streq_skip_trailing_chars("", "", NULL));
        assert_se(!streq_skip_trailing_chars(NULL, "foo bar", NULL));
        assert_se(!streq_skip_trailing_chars("foo", NULL, NULL));
        assert_se(!streq_skip_trailing_chars("", "f", NULL));
}

TEST(strstrafter) {
        static const char buffer[] = "abcdefghijklmnopqrstuvwxyz";

        assert_se(!strstrafter(NULL, NULL));
        assert_se(!strstrafter("", NULL));
        assert_se(!strstrafter(NULL, ""));
        assert_se(streq_ptr(strstrafter("", ""), ""));

        assert_se(strstrafter(buffer, "a") == buffer + 1);
        assert_se(strstrafter(buffer, "") == buffer);
        assert_se(strstrafter(buffer, "ab") == buffer + 2);
        assert_se(strstrafter(buffer, "cde") == buffer + 5);
        assert_se(strstrafter(buffer, "xyz") == strchr(buffer, 0));
        assert_se(strstrafter(buffer, buffer) == strchr(buffer, 0));
        assert_se(!strstrafter(buffer, "-"));
}

DEFINE_TEST_MAIN(LOG_DEBUG);
