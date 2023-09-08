/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <unistd.h>

#include "alloc-util.h"
#include "dropin.h"
#include "generator.h"
#include "mkdir-label.h"
#include "parse-util.h"
#include "path-util.h"
#include "proc-cmdline.h"
#include "special.h"
#include "string-util.h"
#include "strv.h"
#include "unit-file.h"
#include "unit-name.h"

static const char *arg_dest = NULL;
static char *arg_default_unit = NULL;
static char **arg_mask = NULL;
static char **arg_wants = NULL;
static char *arg_debug_shell = NULL;

STATIC_DESTRUCTOR_REGISTER(arg_default_unit, freep);
STATIC_DESTRUCTOR_REGISTER(arg_mask, strv_freep);
STATIC_DESTRUCTOR_REGISTER(arg_wants, strv_freep);
STATIC_DESTRUCTOR_REGISTER(arg_debug_shell, freep);

static int parse_proc_cmdline_item(const char *key, const char *value, void *data) {
        int r;

        assert(key);

        if (streq(key, "systemd.mask")) {
                char *n;

                if (proc_cmdline_value_missing(key, value))
                        return 0;

                r = unit_name_mangle(value, UNIT_NAME_MANGLE_WARN, &n);
                if (r < 0)
                        return log_error_errno(r, "Failed to glob unit name: %m");

                r = strv_consume(&arg_mask, n);
                if (r < 0)
                        return log_oom();

        } else if (streq(key, "systemd.wants")) {
                char *n;

                if (proc_cmdline_value_missing(key, value))
                        return 0;

                r = unit_name_mangle(value, UNIT_NAME_MANGLE_WARN, &n);
                if (r < 0)
                        return log_error_errno(r, "Failed to glob unit name: %m");

                r = strv_consume(&arg_wants, n);
                if (r < 0)
                        return log_oom();

        } else if (proc_cmdline_key_streq(key, "systemd.debug_shell")) {
                const char *t = NULL;

                r = value ? parse_boolean(value) : 1;
                if (r < 0)
                        t = skip_dev_prefix(value);
                else if (r > 0)
                        t = skip_dev_prefix(DEBUGTTY);

                return free_and_strdup_warn(&arg_debug_shell, t);

        } else if (streq(key, "systemd.unit")) {

                if (proc_cmdline_value_missing(key, value))
                        return 0;

                return free_and_strdup_warn(&arg_default_unit, value);

        } else if (!value) {
                const char *target;

                target = runlevel_to_target(key);
                if (target)
                        return free_and_strdup_warn(&arg_default_unit, target);
        }

        return 0;
}

static int generate_mask_symlinks(void) {
        int r = 0;

        STRV_FOREACH(u, arg_mask) {
                _cleanup_free_ char *p = NULL;

                p = path_join(empty_to_root(arg_dest), *u);
                if (!p)
                        return log_oom();

                if (symlink("/dev/null", p) < 0)
                        r = log_error_errno(errno,
                                            "Failed to create mask symlink %s: %m",
                                            p);
        }

        return r;
}

static int generate_wants_symlinks(void) {
        int r = 0;

        STRV_FOREACH(u, arg_wants) {
                _cleanup_free_ char *f = NULL;
                const char *target;

                /* This should match what do_queue_default_job() in core/main.c does. */
                if (arg_default_unit)
                        target = arg_default_unit;
                else if (in_initrd())
                        target = SPECIAL_INITRD_TARGET;
                else
                        target = SPECIAL_DEFAULT_TARGET;

                f = path_join(SYSTEM_DATA_UNIT_DIR, *u);
                if (!f)
                        return log_oom();

                r = generator_add_symlink(arg_dest, target, "wants", f);
                if (r < 0)
                        return r;
        }

        return r;
}

static void install_debug_shell_dropin(const char *dir) {
        int r;

        if (streq(arg_debug_shell, skip_dev_prefix(DEBUGTTY)))
                return;

        r = write_drop_in_format(dir, "debug-shell.service", 50, "tty",
                        "[Unit]\n"
                        "Description=Early root shell on /dev/%s FOR DEBUGGING ONLY\n"
                        "ConditionPathExists=\n"
                        "[Service]\n"
                        "TTYPath=/dev/%s",
                        arg_debug_shell, arg_debug_shell);
        if (r < 0)
                log_warning_errno(r, "Failed to write drop-in for debug-shell.service, ignoring: %m");
}

static int run(const char *dest, const char *dest_early, const char *dest_late) {
        int r, q;

        assert_se(arg_dest = dest_early);

        r = proc_cmdline_parse(parse_proc_cmdline_item, NULL, PROC_CMDLINE_RD_STRICT | PROC_CMDLINE_STRIP_RD_PREFIX);
        if (r < 0)
                log_warning_errno(r, "Failed to parse kernel command line, ignoring: %m");

        if (arg_debug_shell) {
                r = strv_extend(&arg_wants, "debug-shell.service");
                if (r < 0)
                        return log_oom();

                install_debug_shell_dropin(arg_dest);
        }

        r = generate_mask_symlinks();
        q = generate_wants_symlinks();

        return r < 0 ? r : q;
}

DEFINE_MAIN_GENERATOR_FUNCTION(run);
