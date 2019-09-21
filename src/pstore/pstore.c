/* SPDX-License-Identifier: LGPL-2.1+ */

/* Copyright © 2019 Oracle and/or its affiliates. */

/* Generally speaking, the pstore contains a small number of files
 * that in turn contain a small amount of data.  */
#include <errno.h>
#include <stdio.h>
#include <stdio_ext.h>
#include <sys/prctl.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "sd-daemon.h"
#include "sd-journal.h"
#include "sd-login.h"
#include "sd-messages.h"

#include "acl-util.h"
#include "alloc-util.h"
#include "capability-util.h"
#include "cgroup-util.h"
#include "compress.h"
#include "conf-parser.h"
#include "copy.h"
#include "dirent-util.h"
#include "escape.h"
#include "fd-util.h"
#include "fileio.h"
#include "fs-util.h"
#include "io-util.h"
#include "journal-importer.h"
#include "log.h"
#include "macro.h"
#include "main-func.h"
#include "missing.h"
#include "mkdir.h"
#include "parse-util.h"
#include "process-util.h"
#include "signal-util.h"
#include "socket-util.h"
#include "special.h"
#include "sort-util.h"
#include "string-table.h"
#include "string-util.h"
#include "strv.h"
#include "tmpfile-util.h"
#include "user-util.h"
#include "util.h"

/* Command line argument handling */
typedef enum PStoreStorage {
        PSTORE_STORAGE_NONE,
        PSTORE_STORAGE_EXTERNAL,
        PSTORE_STORAGE_JOURNAL,
        _PSTORE_STORAGE_MAX,
        _PSTORE_STORAGE_INVALID = -1
} PStoreStorage;

static const char* const pstore_storage_table[_PSTORE_STORAGE_MAX] = {
        [PSTORE_STORAGE_NONE] = "none",
        [PSTORE_STORAGE_EXTERNAL] = "external",
        [PSTORE_STORAGE_JOURNAL] = "journal",
};

DEFINE_PRIVATE_STRING_TABLE_LOOKUP(pstore_storage, PStoreStorage);
static DEFINE_CONFIG_PARSE_ENUM(config_parse_pstore_storage, pstore_storage, PStoreStorage, "Failed to parse storage setting");

static PStoreStorage arg_storage = PSTORE_STORAGE_EXTERNAL;

static bool arg_unlink = true;
static const char *arg_sourcedir = "/sys/fs/pstore";
static const char *arg_archivedir = "/var/lib/systemd/pstore";

static int parse_config(void) {
        static const ConfigTableItem items[] = {
                { "PStore", "Unlink",  config_parse_bool,           0, &arg_unlink },
                { "PStore", "Storage", config_parse_pstore_storage, 0, &arg_storage },
                {}
        };

        return config_parse_many_nulstr(PKGSYSCONFDIR "/pstore.conf",
                                        CONF_PATHS_NULSTR("systemd/pstore.conf.d"),
                                        "PStore\0",
                                        config_item_table_lookup, items,
                                        CONFIG_PARSE_WARN, NULL);
}

/* File list handling - PStoreEntry is the struct and
 * and PStoreEntry is the type that contains all info
 * about a pstore entry.  */
typedef struct PStoreEntry {
        struct dirent dirent;
        bool is_binary;
        bool handled;
        char *content;
        size_t content_size;
} PStoreEntry;

typedef struct PStoreList {
        PStoreEntry *entries;
        size_t n_entries;
        size_t n_entries_allocated;
} PStoreList;

static void pstore_entries_reset(PStoreList *list) {
        for (size_t i = 0; i < list->n_entries; i++)
                free(list->entries[i].content);
        free(list->entries);
        list->n_entries = 0;
}

static int compare_pstore_entries(const void *_a, const void *_b) {
        PStoreEntry *a = (PStoreEntry *)_a, *b = (PStoreEntry *)_b;
        return strcmp(a->dirent.d_name, b->dirent.d_name);
}

static int move_file(PStoreEntry *pe, const char *subdir) {
        _cleanup_free_ char *ifd_path = NULL, *ofd_path = NULL;
        _cleanup_free_ void *field = NULL;
        const char *suffix, *message;
        struct iovec iovec[2];
        int n_iovec = 0, r;

        if (pe->handled)
                return 0;

        ifd_path = path_join(arg_sourcedir, pe->dirent.d_name);
        if (!ifd_path)
                return log_oom();

        ofd_path = path_join(arg_archivedir, subdir, pe->dirent.d_name);
        if (!ofd_path)
                return log_oom();

        /* Always log to the journal */
        suffix = arg_storage == PSTORE_STORAGE_EXTERNAL ? strjoina(" moved to ", ofd_path) : (char *)".";
        message = strjoina("MESSAGE=PStore ", pe->dirent.d_name, suffix);
        iovec[n_iovec++] = IOVEC_MAKE_STRING(message);

        if (pe->content_size > 0) {
                size_t field_size;

                field_size = strlen("FILE=") + pe->content_size;
                field = malloc(field_size);
                if (!field)
                        return log_oom();
                memcpy(stpcpy(field, "FILE="), pe->content, pe->content_size);
                iovec[n_iovec++] = IOVEC_MAKE(field, field_size);
        }

        r = sd_journal_sendv(iovec, n_iovec);
        if (r < 0)
                return log_error_errno(r, "Failed to log pstore entry: %m");

        if (arg_storage == PSTORE_STORAGE_EXTERNAL) {
                /* Move file from pstore to external storage */
                r = mkdir_parents(ofd_path, 0755);
                if (r < 0)
                        return log_error_errno(r, "Failed to create directory %s: %m", ofd_path);
                r = copy_file_atomic(ifd_path, ofd_path, 0600, 0, 0, COPY_REPLACE);
                if (r < 0)
                        return log_error_errno(r, "Failed to copy_file_atomic: %s to %s", ifd_path, ofd_path);
        }

        /* If file copied properly, remove it from pstore */
        if (arg_unlink)
                (void) unlink(ifd_path);

        pe->handled = true;

        return 0;
}

static int write_dmesg(const char *dmesg, size_t size, const char *id) {
        _cleanup_(unlink_and_freep) char *tmp_path = NULL;
        _cleanup_free_ char *ofd_path = NULL;
        _cleanup_close_ int ofd = -1;
        ssize_t wr;
        int r;

        if (isempty(dmesg) || size == 0)
                return 0;

        ofd_path = path_join(arg_archivedir, id, "dmesg.txt");
        if (!ofd_path)
                return log_oom();

        ofd = open_tmpfile_linkable(ofd_path, O_CLOEXEC|O_CREAT|O_TRUNC|O_WRONLY, &tmp_path);
        if (ofd < 0)
                return log_error_errno(ofd, "Failed to open temporary file %s: %m", ofd_path);
        wr = write(ofd, dmesg, size);
        if (wr < 0)
                return log_error_errno(errno, "Failed to store dmesg to %s: %m", ofd_path);
        if (wr != (ssize_t)size)
                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Failed to store dmesg to %s. %zu bytes are lost.", ofd_path, size - wr);
        r = link_tmpfile(ofd, tmp_path, ofd_path);
        if (r < 0)
                return log_error_errno(r, "Failed to write temporary file %s: %m", ofd_path);
        tmp_path = mfree(tmp_path);

        return 0;
}

static void process_dmesg_files(PStoreList *list) {
        /* Move files, reconstruct dmesg.txt */
        _cleanup_free_ char *dmesg = NULL, *dmesg_id = NULL;
        size_t dmesg_size = 0;
        PStoreEntry *pe;

        /* Handle each dmesg file: files processed in reverse
         * order so as to properly reconstruct original dmesg */
        for (size_t n = list->n_entries; n > 0; n--) {
                bool move_file_and_continue = false;
                _cleanup_free_ char *pe_id = NULL;
                char *p;
                size_t plen;

                pe = &list->entries[n-1];

                if (pe->handled)
                        continue;
                if (!startswith(pe->dirent.d_name, "dmesg-"))
                        continue;

                if (endswith(pe->dirent.d_name, ".enc.z")) /* indicates a problem */
                        move_file_and_continue = true;
                p = strrchr(pe->dirent.d_name, '-');
                if (!p)
                        move_file_and_continue = true;

                if (move_file_and_continue) {
                        /* A dmesg file on which we do NO additional processing */
                        (void) move_file(pe, NULL);
                        continue;
                }

                /* See if this file is one of a related group of files
                 * in order to reconstruct dmesg */

                /* When dmesg is written into pstore, it is done so in
                 * small chunks, whatever the exchange buffer size is
                 * with the underlying pstore backend (ie. EFI may be
                 * ~2KiB), which means an example pstore with approximately
                 * 64KB of storage may have up to roughly 32 dmesg files
                 * that could be related, depending upon the size of the
                 * original dmesg.
                 *
                 * Here we look at the dmesg filename and try to discern
                 * if files are part of a related group, meaning the same
                 * original dmesg.
                 *
                 * The two known pstore backends are EFI and ERST. These
                 * backends store data in the Common Platform Error
                 * Record, CPER, format. The dmesg- filename contains the
                 * CPER record id, a 64bit number (in decimal notation).
                 * In Linux, the record id is encoded with two digits for
                 * the dmesg part (chunk) number and 3 digits for the
                 * count number. So allowing an additional digit to
                 * compensate for advancing time, this code ignores the
                 * last six digits of the filename in determining the
                 * record id.
                 *
                 * For the EFI backend, the record id encodes an id in the
                 * upper 32 bits, and a timestamp in the lower 32-bits.
                 * So ignoring the least significant 6 digits has proven
                 * to generally identify related dmesg entries.  */
#define PSTORE_FILENAME_IGNORE 6

                /* determine common portion of record id */
                ++p; /* move beyond dmesg- */
                plen = strlen(p);
                if (plen > PSTORE_FILENAME_IGNORE) {
                        pe_id = memdup_suffix0(p, plen - PSTORE_FILENAME_IGNORE);
                        if (!pe_id) {
                                log_oom();
                                return;
                        }
                } else
                        pe_id = mfree(pe_id);

                /* Now move file from pstore to archive storage */
                move_file(pe, pe_id);

                /* If the current record id is NOT the same as the
                 * previous record id, then start a new dmesg.txt file */
                if (!pe_id || !dmesg_id || !streq(pe_id, dmesg_id)) {
                        /* Encountered a new dmesg group, close out old one, open new one */
                        if (dmesg) {
                                (void) write_dmesg(dmesg, dmesg_size, dmesg_id);
                                dmesg = mfree(dmesg);
                                dmesg_size = 0;
                        }

                        /* now point dmesg_id to storage of pe_id */
                        free_and_replace(dmesg_id, pe_id);
                }

                /* Reconstruction of dmesg is done as a useful courtesy, do not log errors */
                dmesg = realloc(dmesg, dmesg_size + strlen(pe->dirent.d_name) + strlen(":\n") + pe->content_size + 1);
                if (dmesg) {
                        dmesg_size += sprintf(&dmesg[dmesg_size], "%s:\n", pe->dirent.d_name);
                        if (pe->content) {
                                memcpy(&dmesg[dmesg_size], pe->content, pe->content_size);
                                dmesg_size += pe->content_size;
                        }
                }

                pe_id = mfree(pe_id);
        }
        if (dmesg)
                (void) write_dmesg(dmesg, dmesg_size, dmesg_id);
}

static int list_files(PStoreList *list, const char *sourcepath) {
        _cleanup_(closedirp) DIR *dirp = NULL;
        struct dirent *de;
        int r;

        dirp = opendir(sourcepath);
        if (!dirp)
                return log_error_errno(errno, "Failed to opendir %s: %m", sourcepath);

        FOREACH_DIRENT(de, dirp, return log_error_errno(errno, "Failed to iterate through %s: %m", sourcepath)) {
                _cleanup_free_ char *ifd_path = NULL;

                ifd_path = path_join(sourcepath, de->d_name);
                if (!ifd_path)
                        return log_oom();

                _cleanup_free_ char *buf = NULL;
                size_t buf_size;

                /* Now read contents of pstore file */
                r = read_full_file(ifd_path, &buf, &buf_size);
                if (r < 0) {
                        log_warning_errno(r, "Failed to read file %s, skipping: %m", ifd_path);
                        continue;
                }

                if (!GREEDY_REALLOC(list->entries, list->n_entries_allocated, list->n_entries + 1))
                        return log_oom();

                list->entries[list->n_entries++] = (PStoreEntry) {
                        .dirent = *de,
                        .content = TAKE_PTR(buf),
                        .content_size = buf_size,
                        .is_binary = true,
                        .handled = false,
                };
        }

        return 0;
}

static int run(int argc, char *argv[]) {
        _cleanup_(pstore_entries_reset) PStoreList list = {};
        int r;

        log_setup_service();

        if (argc > 1)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "This program takes no arguments.");

        /* Ignore all parse errors */
        (void) parse_config();

        log_debug("Selected storage '%s'.", pstore_storage_to_string(arg_storage));
        log_debug("Selected Unlink '%d'.", arg_unlink);

        if (arg_storage == PSTORE_STORAGE_NONE)
                /* Do nothing, intentionally, leaving pstore untouched */
                return 0;

        /* Obtain list of files in pstore */
        r = list_files(&list, arg_sourcedir);
        if (r < 0)
                return r;

        /* Handle each pstore file */
        /* Sort files lexigraphically ascending, generally needed by all */
        qsort_safe(list.entries, list.n_entries, sizeof(PStoreEntry), compare_pstore_entries);

        /* Process known file types */
        process_dmesg_files(&list);

        /* Move left over files out of pstore */
        for (size_t n = 0; n < list.n_entries; n++)
                move_file(&list.entries[n], NULL);

        return 0;
}

DEFINE_MAIN_FUNCTION(run);
