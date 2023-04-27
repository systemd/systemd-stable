/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "bus-polkit.h"
#include "copy.h"
#include "env-file-label.h"
#include "env-file.h"
#include "env-util.h"
#include "fd-util.h"
#include "fileio-label.h"
#include "fileio.h"
#include "fs-util.h"
#include "kbd-util.h"
#include "localed-util.h"
#include "macro.h"
#include "mkdir-label.h"
#include "nulstr-util.h"
#include "process-util.h"
#include "string-util.h"
#include "strv.h"
#include "tmpfile-util.h"

static bool startswith_comma(const char *s, const char *prefix) {
        s = startswith(s, prefix);
        if (!s)
                return false;

        return IN_SET(*s, ',', '\0');
}

static const char* systemd_kbd_model_map(void) {
        const char* s;

        s = getenv("SYSTEMD_KBD_MODEL_MAP");
        if (s)
                return s;

        return SYSTEMD_KBD_MODEL_MAP;
}

static const char* systemd_language_fallback_map(void) {
        const char* s;

        s = getenv("SYSTEMD_LANGUAGE_FALLBACK_MAP");
        if (s)
                return s;

        return SYSTEMD_LANGUAGE_FALLBACK_MAP;
}

static void context_free_x11(Context *c) {
        c->x11_layout = mfree(c->x11_layout);
        c->x11_options = mfree(c->x11_options);
        c->x11_model = mfree(c->x11_model);
        c->x11_variant = mfree(c->x11_variant);
}

static void context_free_vconsole(Context *c) {
        c->vc_keymap = mfree(c->vc_keymap);
        c->vc_keymap_toggle = mfree(c->vc_keymap_toggle);
}

void context_clear(Context *c) {
        locale_context_clear(&c->locale_context);
        context_free_x11(c);
        context_free_vconsole(c);

        sd_bus_message_unref(c->locale_cache);
        sd_bus_message_unref(c->x11_cache);
        sd_bus_message_unref(c->vc_cache);

        bus_verify_polkit_async_registry_free(c->polkit_registry);
};

int locale_read_data(Context *c, sd_bus_message *m) {
        assert(c);

        /* Do not try to re-read the file within single bus operation. */
        if (m) {
                if (m == c->locale_cache)
                        return 0;

                sd_bus_message_unref(c->locale_cache);
                c->locale_cache = sd_bus_message_ref(m);
        }

        return locale_context_load(&c->locale_context, LOCALE_LOAD_LOCALE_CONF | LOCALE_LOAD_ENVIRONMENT | LOCALE_LOAD_SIMPLIFY);
}

int vconsole_read_data(Context *c, sd_bus_message *m) {
        struct stat st;
        usec_t t;

        /* Do not try to re-read the file within single bus operation. */
        if (m) {
                if (m == c->vc_cache)
                        return 0;

                sd_bus_message_unref(c->vc_cache);
                c->vc_cache = sd_bus_message_ref(m);
        }

        if (stat("/etc/vconsole.conf", &st) < 0) {
                if (errno != ENOENT)
                        return -errno;

                c->vc_mtime = USEC_INFINITY;
                context_free_vconsole(c);
                return 0;
        }

        /* If mtime is not changed, then we do not need to re-read */
        t = timespec_load(&st.st_mtim);
        if (c->vc_mtime != USEC_INFINITY && t == c->vc_mtime)
                return 0;

        c->vc_mtime = t;
        context_free_vconsole(c);

        return parse_env_file(NULL, "/etc/vconsole.conf",
                              "KEYMAP",        &c->vc_keymap,
                              "KEYMAP_TOGGLE", &c->vc_keymap_toggle);
}

int x11_read_data(Context *c, sd_bus_message *m) {
        _cleanup_fclose_ FILE *f = NULL;
        bool in_section = false;
        struct stat st;
        usec_t t;
        int r;

        /* Do not try to re-read the file within single bus operation. */
        if (m) {
                if (m == c->x11_cache)
                        return 0;

                sd_bus_message_unref(c->x11_cache);
                c->x11_cache = sd_bus_message_ref(m);
        }

        if (stat("/etc/X11/xorg.conf.d/00-keyboard.conf", &st) < 0) {
                if (errno != ENOENT)
                        return -errno;

                c->x11_mtime = USEC_INFINITY;
                context_free_x11(c);
                return 0;
        }

        /* If mtime is not changed, then we do not need to re-read */
        t = timespec_load(&st.st_mtim);
        if (c->x11_mtime != USEC_INFINITY && t == c->x11_mtime)
                return 0;

        c->x11_mtime = t;
        context_free_x11(c);

        f = fopen("/etc/X11/xorg.conf.d/00-keyboard.conf", "re");
        if (!f)
                return -errno;

        for (;;) {
                _cleanup_free_ char *line = NULL;
                char *l;

                r = read_line(f, LONG_LINE_MAX, &line);
                if (r < 0)
                        return r;
                if (r == 0)
                        break;

                l = strstrip(line);
                if (IN_SET(l[0], 0, '#'))
                        continue;

                if (in_section && first_word(l, "Option")) {
                        _cleanup_strv_free_ char **a = NULL;

                        r = strv_split_full(&a, l, WHITESPACE, EXTRACT_UNQUOTE);
                        if (r < 0)
                                return r;

                        if (strv_length(a) == 3) {
                                char **p = NULL;

                                if (streq(a[1], "XkbLayout"))
                                        p = &c->x11_layout;
                                else if (streq(a[1], "XkbModel"))
                                        p = &c->x11_model;
                                else if (streq(a[1], "XkbVariant"))
                                        p = &c->x11_variant;
                                else if (streq(a[1], "XkbOptions"))
                                        p = &c->x11_options;

                                if (p)
                                        free_and_replace(*p, a[2]);
                        }

                } else if (!in_section && first_word(l, "Section")) {
                        _cleanup_strv_free_ char **a = NULL;

                        r = strv_split_full(&a, l, WHITESPACE, EXTRACT_UNQUOTE);
                        if (r < 0)
                                return -ENOMEM;

                        if (strv_length(a) == 2 && streq(a[1], "InputClass"))
                                in_section = true;

                } else if (in_section && first_word(l, "EndSection"))
                        in_section = false;
        }

        return 0;
}

int vconsole_write_data(Context *c) {
        _cleanup_strv_free_ char **l = NULL;
        struct stat st;
        int r;

        r = load_env_file(NULL, "/etc/vconsole.conf", &l);
        if (r < 0 && r != -ENOENT)
                return r;

        r = strv_env_assign(&l, "KEYMAP", empty_to_null(c->vc_keymap));
        if (r < 0)
                return r;

        r = strv_env_assign(&l, "KEYMAP_TOGGLE", empty_to_null(c->vc_keymap_toggle));
        if (r < 0)
                return r;

        if (strv_isempty(l)) {
                if (unlink("/etc/vconsole.conf") < 0)
                        return errno == ENOENT ? 0 : -errno;

                c->vc_mtime = USEC_INFINITY;
                return 0;
        }

        r = write_env_file_label("/etc/vconsole.conf", l);
        if (r < 0)
                return r;

        if (stat("/etc/vconsole.conf", &st) >= 0)
                c->vc_mtime = timespec_load(&st.st_mtim);

        return 0;
}

int x11_write_data(Context *c) {
        _cleanup_fclose_ FILE *f = NULL;
        _cleanup_free_ char *temp_path = NULL;
        struct stat st;
        int r;

        if (isempty(c->x11_layout) &&
            isempty(c->x11_model) &&
            isempty(c->x11_variant) &&
            isempty(c->x11_options)) {

                if (unlink("/etc/X11/xorg.conf.d/00-keyboard.conf") < 0)
                        return errno == ENOENT ? 0 : -errno;

                c->vc_mtime = USEC_INFINITY;
                return 0;
        }

        (void) mkdir_p_label("/etc/X11/xorg.conf.d", 0755);
        r = fopen_temporary("/etc/X11/xorg.conf.d/00-keyboard.conf", &f, &temp_path);
        if (r < 0)
                return r;

        (void) fchmod(fileno(f), 0644);

        fputs("# Written by systemd-localed(8), read by systemd-localed and Xorg. It's\n"
              "# probably wise not to edit this file manually. Use localectl(1) to\n"
              "# instruct systemd-localed to update it.\n"
              "Section \"InputClass\"\n"
              "        Identifier \"system-keyboard\"\n"
              "        MatchIsKeyboard \"on\"\n", f);

        if (!isempty(c->x11_layout))
                fprintf(f, "        Option \"XkbLayout\" \"%s\"\n", c->x11_layout);

        if (!isempty(c->x11_model))
                fprintf(f, "        Option \"XkbModel\" \"%s\"\n", c->x11_model);

        if (!isempty(c->x11_variant))
                fprintf(f, "        Option \"XkbVariant\" \"%s\"\n", c->x11_variant);

        if (!isempty(c->x11_options))
                fprintf(f, "        Option \"XkbOptions\" \"%s\"\n", c->x11_options);

        fputs("EndSection\n", f);

        r = fflush_sync_and_check(f);
        if (r < 0)
                goto fail;

        if (rename(temp_path, "/etc/X11/xorg.conf.d/00-keyboard.conf") < 0) {
                r = -errno;
                goto fail;
        }

        if (stat("/etc/X11/xorg.conf.d/00-keyboard.conf", &st) >= 0)
                c->x11_mtime = timespec_load(&st.st_mtim);

        return 0;

fail:
        if (temp_path)
                (void) unlink(temp_path);

        return r;
}

static int read_next_mapping(const char* filename,
                             unsigned min_fields, unsigned max_fields,
                             FILE *f, unsigned *n, char ***a) {
        assert(f);
        assert(n);
        assert(a);

        for (;;) {
                _cleanup_free_ char *line = NULL;
                size_t length;
                char *l, **b;
                int r;

                r = read_line(f, LONG_LINE_MAX, &line);
                if (r < 0)
                        return r;
                if (r == 0)
                        break;

                (*n)++;

                l = strstrip(line);
                if (IN_SET(l[0], 0, '#'))
                        continue;

                r = strv_split_full(&b, l, WHITESPACE, EXTRACT_UNQUOTE);
                if (r < 0)
                        return r;

                length = strv_length(b);
                if (length < min_fields || length > max_fields) {
                        log_error("Invalid line %s:%u, ignoring.", filename, *n);
                        strv_free(b);
                        continue;

                }

                *a = b;
                return 1;
        }

        return 0;
}

int vconsole_convert_to_x11(Context *c) {
        const char *map;
        int modified = -1;

        map = systemd_kbd_model_map();

        if (isempty(c->vc_keymap)) {
                modified =
                        !isempty(c->x11_layout) ||
                        !isempty(c->x11_model) ||
                        !isempty(c->x11_variant) ||
                        !isempty(c->x11_options);

                context_free_x11(c);
        } else {
                _cleanup_fclose_ FILE *f = NULL;
                unsigned n = 0;

                f = fopen(map, "re");
                if (!f)
                        return -errno;

                for (;;) {
                        _cleanup_strv_free_ char **a = NULL;
                        int r;

                        r = read_next_mapping(map, 5, UINT_MAX, f, &n, &a);
                        if (r < 0)
                                return r;
                        if (r == 0)
                                break;

                        if (!streq(c->vc_keymap, a[0]))
                                continue;

                        if (!streq_ptr(c->x11_layout, empty_or_dash_to_null(a[1])) ||
                            !streq_ptr(c->x11_model, empty_or_dash_to_null(a[2])) ||
                            !streq_ptr(c->x11_variant, empty_or_dash_to_null(a[3])) ||
                            !streq_ptr(c->x11_options, empty_or_dash_to_null(a[4]))) {

                                if (free_and_strdup(&c->x11_layout, empty_or_dash_to_null(a[1])) < 0 ||
                                    free_and_strdup(&c->x11_model, empty_or_dash_to_null(a[2])) < 0 ||
                                    free_and_strdup(&c->x11_variant, empty_or_dash_to_null(a[3])) < 0 ||
                                    free_and_strdup(&c->x11_options, empty_or_dash_to_null(a[4])) < 0)
                                        return -ENOMEM;

                                modified = true;
                        }

                        break;
                }
        }

        if (modified > 0)
                log_info("Changing X11 keyboard layout to '%s' model '%s' variant '%s' options '%s'",
                         strempty(c->x11_layout),
                         strempty(c->x11_model),
                         strempty(c->x11_variant),
                         strempty(c->x11_options));
        else if (modified < 0)
                log_notice("X11 keyboard layout was not modified: no conversion found for \"%s\".",
                           c->vc_keymap);
        else
                log_debug("X11 keyboard layout did not need to be modified.");

        return modified > 0;
}

int find_converted_keymap(const char *x11_layout, const char *x11_variant, char **new_keymap) {
        const char *dir;
        _cleanup_free_ char *n = NULL;

        if (x11_variant)
                n = strjoin(x11_layout, "-", x11_variant);
        else
                n = strdup(x11_layout);
        if (!n)
                return -ENOMEM;

        NULSTR_FOREACH(dir, KBD_KEYMAP_DIRS) {
                _cleanup_free_ char *p = NULL, *pz = NULL;
                bool uncompressed;

                p = strjoin(dir, "xkb/", n, ".map");
                pz = strjoin(dir, "xkb/", n, ".map.gz");
                if (!p || !pz)
                        return -ENOMEM;

                uncompressed = access(p, F_OK) == 0;
                if (uncompressed || access(pz, F_OK) == 0) {
                        log_debug("Found converted keymap %s at %s",
                                  n, uncompressed ? p : pz);

                        *new_keymap = TAKE_PTR(n);
                        return 1;
                }
        }

        return 0;
}

int find_legacy_keymap(Context *c, char **ret) {
        const char *map;
        _cleanup_fclose_ FILE *f = NULL;
        _cleanup_free_ char *new_keymap = NULL;
        unsigned n = 0;
        unsigned best_matching = 0;
        int r;

        assert(!isempty(c->x11_layout));

        map = systemd_kbd_model_map();

        f = fopen(map, "re");
        if (!f)
                return -errno;

        for (;;) {
                _cleanup_strv_free_ char **a = NULL;
                unsigned matching = 0;

                r = read_next_mapping(map, 5, UINT_MAX, f, &n, &a);
                if (r < 0)
                        return r;
                if (r == 0)
                        break;

                /* Determine how well matching this entry is */
                if (streq(c->x11_layout, a[1]))
                        /* If we got an exact match, this is best */
                        matching = 10;
                else {
                        /* We have multiple X layouts, look for an
                         * entry that matches our key with everything
                         * but the first layout stripped off. */
                        if (startswith_comma(c->x11_layout, a[1]))
                                matching = 5;
                        else  {
                                _cleanup_free_ char *x = NULL;

                                /* If that didn't work, strip off the
                                 * other layouts from the entry, too */
                                x = strndup(a[1], strcspn(a[1], ","));
                                if (startswith_comma(c->x11_layout, x))
                                        matching = 1;
                        }
                }

                if (matching > 0) {
                        if (isempty(c->x11_model) || streq_ptr(c->x11_model, a[2])) {
                                matching++;

                                if (streq_ptr(c->x11_variant, a[3])) {
                                        matching++;

                                        if (streq_ptr(c->x11_options, a[4]))
                                                matching++;
                                }
                        }
                }

                /* The best matching entry so far, then let's save that */
                if (matching >= MAX(best_matching, 1u)) {
                        log_debug("Found legacy keymap %s with score %u",
                                  a[0], matching);

                        if (matching > best_matching) {
                                best_matching = matching;

                                r = free_and_strdup(&new_keymap, a[0]);
                                if (r < 0)
                                        return r;
                        }
                }
        }

        if (best_matching < 10 && c->x11_layout) {
                /* The best match is only the first part of the X11
                 * keymap. Check if we have a converted map which
                 * matches just the first layout.
                 */
                char *l, *v = NULL, *converted;

                l = strndupa_safe(c->x11_layout, strcspn(c->x11_layout, ","));
                if (c->x11_variant)
                        v = strndupa_safe(c->x11_variant,
                                          strcspn(c->x11_variant, ","));
                r = find_converted_keymap(l, v, &converted);
                if (r < 0)
                        return r;
                if (r > 0)
                        free_and_replace(new_keymap, converted);
        }

        *ret = TAKE_PTR(new_keymap);
        return (bool) *ret;
}

int find_language_fallback(const char *lang, char **language) {
        const char *map;
        _cleanup_fclose_ FILE *f = NULL;
        unsigned n = 0;

        assert(lang);
        assert(language);

        map = systemd_language_fallback_map();

        f = fopen(map, "re");
        if (!f)
                return -errno;

        for (;;) {
                _cleanup_strv_free_ char **a = NULL;
                int r;

                r = read_next_mapping(map, 2, 2, f, &n, &a);
                if (r <= 0)
                        return r;

                if (streq(lang, a[0])) {
                        assert(strv_length(a) == 2);
                        *language = TAKE_PTR(a[1]);
                        return 1;
                }
        }

        assert_not_reached();
}

int x11_convert_to_vconsole(Context *c) {
        bool modified = false;

        if (isempty(c->x11_layout)) {
                modified =
                        !isempty(c->vc_keymap) ||
                        !isempty(c->vc_keymap_toggle);

                context_free_vconsole(c);
        } else {
                _cleanup_free_ char *new_keymap = NULL;
                int r;

                r = find_converted_keymap(c->x11_layout, c->x11_variant, &new_keymap);
                if (r < 0)
                        return r;
                else if (r == 0) {
                        r = find_legacy_keymap(c, &new_keymap);
                        if (r < 0)
                                return r;
                }
                if (r == 0)
                        /* We search for layout-variant match first, but then we also look
                         * for anything which matches just the layout. So it's accurate to say
                         * that we couldn't find anything which matches the layout. */
                        log_notice("No conversion to virtual console map found for \"%s\".",
                                   c->x11_layout);

                if (!streq_ptr(c->vc_keymap, new_keymap)) {
                        free_and_replace(c->vc_keymap, new_keymap);
                        c->vc_keymap_toggle = mfree(c->vc_keymap_toggle);
                        modified = true;
                }
        }

        if (modified)
                log_info("Changing virtual console keymap to '%s' toggle '%s'",
                         strempty(c->vc_keymap), strempty(c->vc_keymap_toggle));
        else
                log_debug("Virtual console keymap was not modified.");

        return modified;
}

bool locale_gen_check_available(void) {
#if HAVE_LOCALEGEN
        if (access(LOCALEGEN_PATH, X_OK) < 0) {
                if (errno != ENOENT)
                        log_warning_errno(errno, "Unable to determine whether " LOCALEGEN_PATH " exists and is executable, assuming it is not: %m");
                return false;
        }
        if (access("/etc/locale.gen", F_OK) < 0) {
                if (errno != ENOENT)
                        log_warning_errno(errno, "Unable to determine whether /etc/locale.gen exists, assuming it does not: %m");
                return false;
        }
        return true;
#else
        return false;
#endif
}

#if HAVE_LOCALEGEN
static bool locale_encoding_is_utf8_or_unspecified(const char *locale) {
        const char *c = strchr(locale, '.');
        return !c || strcaseeq(c, ".UTF-8") || strcasestr(locale, ".UTF-8@");
}

static int locale_gen_locale_supported(const char *locale_entry) {
        /* Returns an error valus <= 0 if the locale-gen entry is invalid or unsupported,
         * 1 in case the locale entry is valid, and -EOPNOTSUPP specifically in case
         * the distributor has not provided us with a SUPPORTED file to check
         * locale for validity. */

        _cleanup_fclose_ FILE *f = NULL;
        int r;

        assert(locale_entry);

        /* Locale templates without country code are never supported */
        if (!strstr(locale_entry, "_"))
                return -EINVAL;

        f = fopen("/usr/share/i18n/SUPPORTED", "re");
        if (!f) {
                if (errno == ENOENT)
                        return log_debug_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                                               "Unable to check validity of locale entry %s: /usr/share/i18n/SUPPORTED does not exist",
                                               locale_entry);
                return -errno;
        }

        for (;;) {
                _cleanup_free_ char *line = NULL;
                char *l;

                r = read_line(f, LONG_LINE_MAX, &line);
                if (r < 0)
                        return log_debug_errno(r, "Failed to read /usr/share/i18n/SUPPORTED: %m");
                if (r == 0)
                        return 0;

                l = strstrip(line);
                if (strcaseeq_ptr(l, locale_entry))
                        return 1;
        }
}
#endif

int locale_gen_enable_locale(const char *locale) {
#if HAVE_LOCALEGEN
        _cleanup_fclose_ FILE *fr = NULL, *fw = NULL;
        _cleanup_(unlink_and_freep) char *temp_path = NULL;
        _cleanup_free_ char *locale_entry = NULL;
        bool locale_enabled = false, first_line = false;
        bool write_new = false;
        int r;

        if (isempty(locale))
                return 0;

        if (locale_encoding_is_utf8_or_unspecified(locale)) {
                locale_entry = strjoin(locale, " UTF-8");
                if (!locale_entry)
                        return -ENOMEM;
        } else
                return -ENOEXEC; /* We do not process non-UTF-8 locale */

        r = locale_gen_locale_supported(locale_entry);
        if (r == 0)
                return -EINVAL;
        if (r < 0 && r != -EOPNOTSUPP)
                return r;

        fr = fopen("/etc/locale.gen", "re");
        if (!fr) {
                if (errno != ENOENT)
                        return -errno;
                write_new = true;
        }

        r = fopen_temporary("/etc/locale.gen", &fw, &temp_path);
        if (r < 0)
                return r;

        if (write_new)
                (void) fchmod(fileno(fw), 0644);
        else {
                /* apply mode & xattrs of the original file to new file */
                r = copy_access(fileno(fr), fileno(fw));
                if (r < 0)
                        return r;
                r = copy_xattr(fileno(fr), fileno(fw), COPY_ALL_XATTRS);
                if (r < 0)
                        log_debug_errno(r, "Failed to copy all xattrs from old to new /etc/locale.gen file, ignoring: %m");
        }

        if (!write_new) {
                /* The config file ends with a line break, which we do not want to include before potentially appending a new locale
                * instead of uncommenting an existing line. By prepending linebreaks, we can avoid buffering this file but can still write
                * a nice config file without empty lines */
                first_line = true;
                for (;;) {
                        _cleanup_free_ char *line = NULL;
                        char *line_locale;

                        r = read_line(fr, LONG_LINE_MAX, &line);
                        if (r < 0)
                                return r;
                        if (r == 0)
                                break;

                        if (locale_enabled) {
                                /* Just complete writing the file if the new locale was already enabled */
                                if (!first_line)
                                        fputc('\n', fw);
                                fputs(line, fw);
                                first_line = false;
                                continue;
                        }

                        line_locale = strstrip(line);
                        if (isempty(line_locale)) {
                                fputc('\n', fw);
                                first_line = false;
                                continue;
                        }

                        if (line_locale[0] == '#')
                                line_locale = strstrip(line_locale + 1);
                        else if (strcaseeq_ptr(line_locale, locale_entry))
                                return 0; /* the file already had our locale activated, so skip updating it */

                        if (strcaseeq_ptr(line_locale, locale_entry)) {
                                /* Uncomment existing line for new locale */
                                if (!first_line)
                                        fputc('\n', fw);
                                fputs(locale_entry, fw);
                                locale_enabled = true;
                                first_line = false;
                                continue;
                        }

                        /* The line was not for the locale we want to enable, just copy it */
                        if (!first_line)
                                fputc('\n', fw);
                        fputs(line, fw);
                        first_line = false;
                }
        }

        /* Add locale to enable to the end of the file if it was not found as commented line */
        if (!locale_enabled) {
                if (!write_new)
                        fputc('\n', fw);
                fputs(locale_entry, fw);
        }
        fputc('\n', fw);

        r = fflush_sync_and_check(fw);
        if (r < 0)
                return r;

        if (rename(temp_path, "/etc/locale.gen") < 0)
                return -errno;
        temp_path = mfree(temp_path);

        return 0;
#else
        return -EOPNOTSUPP;
#endif
}

int locale_gen_run(void) {
#if HAVE_LOCALEGEN
        pid_t pid;
        int r;

        r = safe_fork("(sd-localegen)", FORK_RESET_SIGNALS|FORK_RLIMIT_NOFILE_SAFE|FORK_CLOSE_ALL_FDS|FORK_LOG|FORK_WAIT, &pid);
        if (r < 0)
                return r;
        if (r == 0) {
                execl(LOCALEGEN_PATH, LOCALEGEN_PATH, NULL);
                _exit(EXIT_FAILURE);
        }

        return 0;
#else
        return -EOPNOTSUPP;
#endif
}
