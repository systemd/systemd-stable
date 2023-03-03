/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "bus-error.h"
#include "copy.h"
#include "fd-util.h"
#include "fileio.h"
#include "fs-util.h"
#include "mkdir-label.h"
#include "pager.h"
#include "path-util.h"
#include "pretty-print.h"
#include "process-util.h"
#include "selinux-util.h"
#include "stat-util.h"
#include "systemctl-daemon-reload.h"
#include "systemctl-edit.h"
#include "systemctl-util.h"
#include "systemctl.h"
#include "terminal-util.h"
#include "tmpfile-util.h"

#define EDIT_MARKER_START "### Anything between here and the comment below will become the contents of the drop-in file"
#define EDIT_MARKER_END "### Edits below this comment will be discarded"

int verb_cat(int argc, char *argv[], void *userdata) {
        _cleanup_(hashmap_freep) Hashmap *cached_name_map = NULL, *cached_id_map = NULL;
        _cleanup_(lookup_paths_free) LookupPaths lp = {};
        _cleanup_strv_free_ char **names = NULL;
        sd_bus *bus;
        bool first = true;
        int r, rc = 0;

        /* Include all units by default — i.e. continue as if the --all option was used */
        if (strv_isempty(arg_states))
                arg_all = true;

        if (arg_transport != BUS_TRANSPORT_LOCAL)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Cannot remotely cat units.");

        r = lookup_paths_init_or_warn(&lp, arg_scope, 0, arg_root);
        if (r < 0)
                return r;

        r = acquire_bus(BUS_MANAGER, &bus);
        if (r < 0)
                return r;

        r = expand_unit_names(bus, strv_skip(argv, 1), NULL, &names, NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to expand names: %m");

        r = maybe_extend_with_unit_dependencies(bus, &names);
        if (r < 0)
                return r;

        pager_open(arg_pager_flags);

        STRV_FOREACH(name, names) {
                _cleanup_free_ char *fragment_path = NULL;
                _cleanup_strv_free_ char **dropin_paths = NULL;

                r = unit_find_paths(bus, *name, &lp, false, &cached_name_map, &cached_id_map, &fragment_path, &dropin_paths);
                if (r == -ERFKILL) {
                        printf("%s# Unit %s is masked%s.\n",
                               ansi_highlight_magenta(),
                               *name,
                               ansi_normal());
                        continue;
                }
                if (r == -EKEYREJECTED) {
                        printf("%s# Unit %s could not be loaded.%s\n",
                               ansi_highlight_magenta(),
                               *name,
                               ansi_normal());
                        continue;
                }
                if (r < 0)
                        return r;
                if (r == 0) {
                        /* Skip units which have no on-disk counterpart, but propagate the error to the
                         * user */
                        rc = -ENOENT;
                        continue;
                }

                if (first)
                        first = false;
                else
                        puts("");

                if (need_daemon_reload(bus, *name) > 0) /* ignore errors (<0), this is informational output */
                        fprintf(stderr,
                                "%s# Warning: %s changed on disk, the version systemd has loaded is outdated.\n"
                                "%s# This output shows the current version of the unit's original fragment and drop-in files.\n"
                                "%s# If fragments or drop-ins were added or removed, they are not properly reflected in this output.\n"
                                "%s# Run 'systemctl%s daemon-reload' to reload units.%s\n",
                                ansi_highlight_red(),
                                *name,
                                ansi_highlight_red(),
                                ansi_highlight_red(),
                                ansi_highlight_red(),
                                arg_scope == LOOKUP_SCOPE_SYSTEM ? "" : " --user",
                                ansi_normal());

                r = cat_files(fragment_path, dropin_paths, 0);
                if (r < 0)
                        return r;
        }

        return rc;
}

static int create_edit_temp_file(
                const char *new_path,
                const char *original_path,
                char ** const original_unit_paths,
                char **ret_tmp_fn,
                unsigned *ret_edit_line) {

        _cleanup_free_ char *t = NULL;
        unsigned ln = 1;
        int r;

        assert(new_path);
        assert(ret_tmp_fn);
        assert(ret_edit_line);

        r = tempfn_random(new_path, NULL, &t);
        if (r < 0)
                return log_error_errno(r, "Failed to determine temporary filename for \"%s\": %m", new_path);

        r = mkdir_parents_label(new_path, 0755);
        if (r < 0)
                return log_error_errno(r, "Failed to create directories for \"%s\": %m", new_path);

        if (original_path) {
                r = mac_selinux_create_file_prepare(new_path, S_IFREG);
                if (r < 0)
                        return r;

                r = copy_file(original_path, t, 0, 0644, 0, 0, COPY_REFLINK);
                if (r == -ENOENT) {
                        r = touch(t);
                        mac_selinux_create_file_clear();
                        if (r < 0)
                                return log_error_errno(r, "Failed to create temporary file \"%s\": %m", t);
                } else {
                        mac_selinux_create_file_clear();
                        if (r < 0)
                                return log_error_errno(r, "Failed to create temporary file for \"%s\": %m", new_path);
                }
        } else if (original_unit_paths) {
                _cleanup_free_ char *new_contents = NULL;
                _cleanup_fclose_ FILE *f = NULL;

                r = mac_selinux_create_file_prepare(new_path, S_IFREG);
                if (r < 0)
                        return r;

                f = fopen(t, "we");
                mac_selinux_create_file_clear();
                if (!f)
                        return log_error_errno(errno, "Failed to open \"%s\": %m", t);

                if (fchmod(fileno(f), 0644) < 0)
                        return log_error_errno(errno, "Failed to change mode of \"%s\": %m", t);

                r = read_full_file(new_path, &new_contents, NULL);
                if (r < 0 && r != -ENOENT)
                        return log_error_errno(r, "Failed to read \"%s\": %m", new_path);

                fprintf(f,
                        "### Editing %s\n"
                        EDIT_MARKER_START "\n"
                        "\n"
                        "%s%s"
                        "\n"
                        EDIT_MARKER_END,
                        new_path,
                        strempty(new_contents),
                        new_contents && endswith(new_contents, "\n") ? "" : "\n");

                ln = 4; /* start editing at the contents */

                /* Add a comment with the contents of the original unit files */
                STRV_FOREACH(path, original_unit_paths) {
                        _cleanup_free_ char *contents = NULL;

                        /* Skip the file that's being edited */
                        if (path_equal(*path, new_path))
                                continue;

                        r = read_full_file(*path, &contents, NULL);
                        if (r < 0)
                                return log_error_errno(r, "Failed to read \"%s\": %m", *path);

                        fprintf(f, "\n\n### %s", *path);
                        if (!isempty(contents)) {
                                _cleanup_free_ char *commented_contents = NULL;

                                commented_contents = strreplace(strstrip(contents), "\n", "\n# ");
                                if (!commented_contents)
                                        return log_oom();
                                fprintf(f, "\n# %s", commented_contents);
                        }
                }

                r = fflush_and_check(f);
                if (r < 0)
                        return log_error_errno(r, "Failed to create temporary file \"%s\": %m", t);
        }

        *ret_tmp_fn = TAKE_PTR(t);
        *ret_edit_line = ln;

        return 0;
}

static int get_file_to_edit(
                const LookupPaths *paths,
                const char *name,
                char **ret_path) {

        _cleanup_free_ char *path = NULL, *run = NULL;

        assert(name);
        assert(ret_path);

        path = path_join(paths->persistent_config, name);
        if (!path)
                return log_oom();

        if (arg_runtime) {
                run = path_join(paths->runtime_config, name);
                if (!run)
                        return log_oom();

                if (access(path, F_OK) >= 0)
                        return log_error_errno(SYNTHETIC_ERRNO(EEXIST),
                                               "Refusing to create \"%s\" because it would be overridden by \"%s\" anyway.",
                                               run, path);

                *ret_path = TAKE_PTR(run);
        } else
                *ret_path = TAKE_PTR(path);

        return 0;
}

typedef struct EditFile {
        char *path;
        char *tmp;
        unsigned line;
} EditFile;

static void edit_file_free_all(EditFile **f) {
        if (!f || !*f)
                return;

        for (EditFile *i = *f; i->path; i++) {
                free(i->path);
                free(i->tmp);
        }

        free(*f);
}

static int unit_file_create_new(
                const LookupPaths *paths,
                const char *unit_name,
                const char *suffix,
                char ** const original_unit_paths,
                EditFile *ret_edit_file) {

        _cleanup_free_ char *new_path = NULL, *tmp_path = NULL;
        unsigned edit_line;
        const char *ending;
        int r;

        assert(unit_name);
        assert(ret_edit_file);

        ending = strjoina(unit_name, suffix);
        r = get_file_to_edit(paths, ending, &new_path);
        if (r < 0)
                return r;

        r = create_edit_temp_file(new_path, NULL, original_unit_paths, &tmp_path, &edit_line);
        if (r < 0)
                return r;

        *ret_edit_file = (EditFile) {
                .path = TAKE_PTR(new_path),
                .tmp = TAKE_PTR(tmp_path),
                .line = edit_line,
        };
        return 0;
}

static int unit_file_create_copy(
                const LookupPaths *paths,
                const char *unit_name,
                const char *fragment_path,
                EditFile *ret_edit_file) {

        _cleanup_free_ char *new_path = NULL, *tmp_path = NULL;
        unsigned edit_line;
        int r;

        assert(fragment_path);
        assert(unit_name);
        assert(ret_edit_file);

        r = get_file_to_edit(paths, unit_name, &new_path);
        if (r < 0)
                return r;

        if (!path_equal(fragment_path, new_path) && access(new_path, F_OK) >= 0) {
                char response;

                r = ask_char(&response, "yn", "\"%s\" already exists. Overwrite with \"%s\"? [(y)es, (n)o] ", new_path, fragment_path);
                if (r < 0)
                        return r;
                if (response != 'y')
                        return log_warning_errno(SYNTHETIC_ERRNO(EKEYREJECTED), "%s skipped.", unit_name);
        }

        r = create_edit_temp_file(new_path, fragment_path, NULL, &tmp_path, &edit_line);
        if (r < 0)
                return r;

        *ret_edit_file = (EditFile) {
                .path = TAKE_PTR(new_path),
                .tmp = TAKE_PTR(tmp_path),
                .line = edit_line,
        };
        return 0;
}

static int run_editor(const EditFile *files) {
        int r;

        assert(files);

        r = safe_fork("(editor)", FORK_RESET_SIGNALS|FORK_DEATHSIG|FORK_RLIMIT_NOFILE_SAFE|FORK_LOG|FORK_WAIT, NULL);
        if (r < 0)
                return r;
        if (r == 0) {
                size_t n_editor_args = 0, i = 1, argc;
                char **editor_args = NULL, **args;
                const char *editor;

                /* SYSTEMD_EDITOR takes precedence over EDITOR which takes precedence over VISUAL.  If
                 * neither SYSTEMD_EDITOR nor EDITOR nor VISUAL are present, we try to execute well known
                 * editors. */
                editor = getenv("SYSTEMD_EDITOR");
                if (!editor)
                        editor = getenv("EDITOR");
                if (!editor)
                        editor = getenv("VISUAL");

                if (isempty(editor))
                        argc = 1;
                else {
                        editor_args = strv_split(editor, WHITESPACE);
                        if (!editor_args) {
                                (void) log_oom();
                                _exit(EXIT_FAILURE);
                        }
                        n_editor_args = strv_length(editor_args);
                        argc = n_editor_args;
                }

                for (const EditFile *f = files; f->path; f++)
                        argc += 2;

                args = newa(char*, argc + 1);

                if (n_editor_args > 0) {
                        args[0] = editor_args[0];
                        for (; i < n_editor_args; i++)
                                args[i] = editor_args[i];
                }

                if (files[0].path && files[0].line > 1 && !files[1].path) {
                        /* If editing a single file only, use the +LINE syntax to put cursor on the right line */
                        if (asprintf(args + i, "+%u", files[0].line) < 0) {
                                (void) log_oom();
                                _exit(EXIT_FAILURE);
                        }

                        i++;
                        args[i++] = files[0].tmp;
                } else
                        for (const EditFile *f = files; f->path; f++)
                                args[i++] = f->tmp;

                args[i] = NULL;

                if (n_editor_args > 0)
                        execvp(args[0], (char* const*) args);

                FOREACH_STRING(name, "editor", "nano", "vim", "vi") {
                        args[0] = (char*) name;
                        execvp(name, (char* const*) args);
                        /* We do not fail if the editor doesn't exist because we want to try each one of them
                         * before failing. */
                        if (errno != ENOENT) {
                                log_error_errno(errno, "Failed to execute %s: %m", name);
                                _exit(EXIT_FAILURE);
                        }
                }

                log_error("Cannot edit units, no editor available. Please set either $SYSTEMD_EDITOR, $EDITOR or $VISUAL.");
                _exit(EXIT_FAILURE);
        }

        return 0;
}

static int find_paths_to_edit(
                sd_bus *bus,
                char **names,
                EditFile **ret_edit_files) {

        _cleanup_(hashmap_freep) Hashmap *cached_name_map = NULL, *cached_id_map = NULL;
        _cleanup_(edit_file_free_all) EditFile *edit_files = NULL;
        _cleanup_(lookup_paths_free) LookupPaths lp = {};
        _cleanup_free_ char *drop_in_alloc = NULL, *suffix = NULL;
        const char *drop_in;
        size_t n_edit_files = 0;
        int r;

        assert(names);
        assert(ret_edit_files);

        if (isempty(arg_drop_in))
                drop_in = "override.conf";
        else if (!endswith(arg_drop_in, ".conf")) {
                drop_in_alloc = strjoin(arg_drop_in, ".conf");
                if (!drop_in_alloc)
                        return log_oom();

                drop_in = drop_in_alloc;
        } else
                drop_in = arg_drop_in;

        if (!filename_is_valid(drop_in))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Invalid drop-in file name '%s'.", drop_in);

        suffix = strjoin(".d/", drop_in);
        if (!suffix)
                return log_oom();

        r = lookup_paths_init(&lp, arg_scope, 0, arg_root);
        if (r < 0)
                return r;

        STRV_FOREACH(name, names) {
                _cleanup_free_ char *path = NULL;
                _cleanup_strv_free_ char **unit_paths = NULL;

                r = unit_find_paths(bus, *name, &lp, false, &cached_name_map, &cached_id_map, &path, &unit_paths);
                if (r == -EKEYREJECTED) {
                        /* If loading of the unit failed server side complete, then the server won't tell us
                         * the unit file path. In that case, find the file client side. */
                        log_debug_errno(r, "Unit '%s' was not loaded correctly, retrying client-side.", *name);
                        r = unit_find_paths(bus, *name, &lp, true, &cached_name_map, &cached_id_map, &path, &unit_paths);
                }
                if (r == -ERFKILL)
                        return log_error_errno(r, "Unit '%s' masked, cannot edit.", *name);
                if (r < 0)
                        return r;

                if (!GREEDY_REALLOC0(edit_files, n_edit_files + 2))
                        return log_oom();

                if (!path) {
                        if (!arg_force) {
                                log_info("Run 'systemctl edit%s --force --full %s' to create a new unit.",
                                         arg_scope == LOOKUP_SCOPE_GLOBAL ? " --global" :
                                         arg_scope == LOOKUP_SCOPE_USER ? " --user" : "",
                                         *name);
                                return -ENOENT;
                        }

                        /* Create a new unit from scratch */
                        r = unit_file_create_new(
                                        &lp,
                                        *name,
                                        arg_full ? NULL : suffix,
                                        NULL,
                                        edit_files + n_edit_files);
                } else {
                        _cleanup_free_ char *unit_name = NULL;

                        r = path_extract_filename(path, &unit_name);
                        if (r < 0)
                                return log_error_errno(r, "Failed to extract unit name from path '%s': %m", path);

                        /* We follow unit aliases, but we need to propagate the instance */
                        if (unit_name_is_valid(*name, UNIT_NAME_INSTANCE) &&
                            unit_name_is_valid(unit_name, UNIT_NAME_TEMPLATE)) {
                                _cleanup_free_ char *instance = NULL, *tmp_name = NULL;

                                r = unit_name_to_instance(*name, &instance);
                                if (r < 0)
                                        return r;

                                r = unit_name_replace_instance(unit_name, instance, &tmp_name);
                                if (r < 0)
                                        return r;

                                free_and_replace(unit_name, tmp_name);
                        }

                        if (arg_full)
                                r = unit_file_create_copy(
                                                &lp,
                                                unit_name,
                                                path,
                                                edit_files + n_edit_files);
                        else {
                                r = strv_prepend(&unit_paths, path);
                                if (r < 0)
                                        return log_oom();

                                r = unit_file_create_new(
                                                &lp,
                                                unit_name,
                                                suffix,
                                                unit_paths,
                                                edit_files + n_edit_files);
                        }
                }
                if (r < 0)
                        return r;

                n_edit_files++;
        }

        *ret_edit_files = n_edit_files > 0 ? TAKE_PTR(edit_files) : NULL;
        return 0;
}

static int trim_edit_markers(const char *path) {
        _cleanup_free_ char *old_contents = NULL, *new_contents = NULL;
        char *contents_start, *contents_end;
        const char *c = NULL;
        int r;

        /* Trim out the lines between the two markers */
        r = read_full_file(path, &old_contents, NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to read temporary file \"%s\": %m", path);

        contents_start = strstr(old_contents, EDIT_MARKER_START);
        if (contents_start)
                contents_start += strlen(EDIT_MARKER_START);
        else
                contents_start = old_contents;

        contents_end = strstr(contents_start, EDIT_MARKER_END);
        if (contents_end)
                contents_end[0] = 0;

        c = strstrip(contents_start);
        if (isempty(c))
                return 0; /* All gone now */

        new_contents = strjoin(c, "\n"); /* Trim prefix and suffix, but ensure suffixed by single newline */
        if (!new_contents)
                return log_oom();

        if (streq(old_contents, new_contents)) /* Don't touch the file if the above didn't change a thing */
                return 1; /* Unchanged, but good */

        r = write_string_file(path, new_contents, WRITE_STRING_FILE_CREATE | WRITE_STRING_FILE_TRUNCATE | WRITE_STRING_FILE_AVOID_NEWLINE);
        if (r < 0)
                return log_error_errno(r, "Failed to modify temporary file \"%s\": %m", path);

        return 1; /* Changed, but good */
}

int verb_edit(int argc, char *argv[], void *userdata) {
        _cleanup_(edit_file_free_all) EditFile *edit_files = NULL;
        _cleanup_(lookup_paths_free) LookupPaths lp = {};
        _cleanup_strv_free_ char **names = NULL;
        sd_bus *bus;
        int r;

        if (!on_tty())
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Cannot edit units if not on a tty.");

        if (arg_transport != BUS_TRANSPORT_LOCAL)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Cannot edit units remotely.");

        r = lookup_paths_init_or_warn(&lp, arg_scope, 0, arg_root);
        if (r < 0)
                return r;

        r = mac_selinux_init();
        if (r < 0)
                return r;

        r = acquire_bus(BUS_MANAGER, &bus);
        if (r < 0)
                return r;

        r = expand_unit_names(bus, strv_skip(argv, 1), NULL, &names, NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to expand names: %m");
        if (strv_isempty(names))
                return log_error_errno(SYNTHETIC_ERRNO(ENOENT), "No units matched the specified patterns.");

        STRV_FOREACH(tmp, names) {
                r = unit_is_masked(bus, &lp, *tmp);
                if (r < 0)
                        return r;
                if (r > 0)
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Cannot edit %s: unit is masked.", *tmp);
        }

        r = find_paths_to_edit(bus, names, &edit_files);
        if (r < 0)
                return r;
        if (!edit_files)
                return -ENOENT;

        r = run_editor(edit_files);
        if (r < 0)
                goto end;

        for (EditFile *f = edit_files; f->path; f++) {
                /* If the temporary file is empty we ignore it. This allows the user to cancel the
                 * modification. */
                r = trim_edit_markers(f->tmp);
                if (r < 0)
                        goto end;
                if (r == 0) /* has no actual contents? then ignore it */
                        continue;

                r = RET_NERRNO(rename(f->tmp, f->path));
                if (r < 0) {
                        log_error_errno(r, "Failed to rename \"%s\" to \"%s\": %m", f->tmp, f->path);
                        goto end;
                }

                f->tmp = mfree(f->tmp);
                log_info("Successfully installed edited file '%s'.", f->path);
        }

        r = 0;

        if (!arg_no_reload && !install_client_side()) {
                r = daemon_reload(ACTION_RELOAD, /* graceful= */ false);
                if (r > 0)
                        r = 0;
        }

end:
        for (EditFile *f = ASSERT_PTR(edit_files); f->path; f++) {

                if (f->tmp)
                        (void) unlink(f->tmp);

                /* Removing empty dropin dirs */
                if (!arg_full) {
                        _cleanup_free_ char *dir = NULL;

                        r = path_extract_directory(f->path, &dir);
                        if (r < 0)
                                return log_error_errno(r, "Failed to extract directory from '%s': %m", f->path);

                        /* No need to check if the dir is empty, rmdir does nothing if it is not the case. */
                        (void) rmdir(dir);
                }
        }

        return r;
}
