/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <libgen.h>

#include "alloc-util.h"
#include "journal-remote.h"
#include "path-util.h"
#include "stat-util.h"

static int do_rotate(ManagedJournalFile **f, MMapCache *m, JournalFileFlags file_flags) {
        int r;

        r = managed_journal_file_rotate(f, m, file_flags, UINT64_MAX, NULL);
        if (r < 0) {
                if (*f)
                        log_error_errno(r, "Failed to rotate %s: %m", (*f)->file->path);
                else
                        log_error_errno(r, "Failed to create rotated journal: %m");
        }

        return r;
}

Writer* writer_new(RemoteServer *server) {
        _cleanup_(writer_unrefp) Writer *w = NULL;
        int r;

        w = new0(Writer, 1);
        if (!w)
                return NULL;

        w->n_ref = 1;
        w->metrics = server->metrics;

        w->mmap = mmap_cache_new();
        if (!w->mmap)
                return NULL;

        w->server = server;

        if (is_dir(server->output, /* follow = */ true) > 0) {
                w->output = strdup(server->output);
                if (!w->output)
                        return NULL;
        } else {
                r = path_extract_directory(server->output, &w->output);
                if (r < 0) {
                        log_error_errno(r, "Failed to find directory of file \"%s\": %m", server->output);
                        return NULL;
                }
        }

        return TAKE_PTR(w);
}

static Writer* writer_free(Writer *w) {
        if (!w)
                return NULL;

        if (w->journal) {
                log_debug("Closing journal file %s.", w->journal->file->path);
                managed_journal_file_close(w->journal);
        }

        if (w->server && w->hashmap_key)
                hashmap_remove(w->server->writers, w->hashmap_key);

        free(w->hashmap_key);

        if (w->mmap)
                mmap_cache_unref(w->mmap);

        free(w->output);

        return mfree(w);
}

DEFINE_TRIVIAL_REF_UNREF_FUNC(Writer, writer, writer_free);

int writer_write(Writer *w,
                 const struct iovec_wrapper *iovw,
                 const dual_timestamp *ts,
                 const sd_id128_t *boot_id,
                 JournalFileFlags file_flags) {
        int r;

        assert(w);
        assert(iovw);
        assert(iovw->count > 0);

        if (journal_file_rotate_suggested(w->journal->file, 0, LOG_DEBUG)) {
                log_info("%s: Journal header limits reached or header out-of-date, rotating",
                         w->journal->file->path);
                r = do_rotate(&w->journal, w->mmap, file_flags);
                if (r < 0)
                        return r;
                r = journal_directory_vacuum(w->output, w->metrics.max_use, w->metrics.n_max_files, 0, NULL, /* verbose = */ true);
                if (r < 0)
                        return r;
        }

        r = journal_file_append_entry(w->journal->file, ts, boot_id,
                                      iovw->iovec, iovw->count,
                                      &w->seqnum, NULL, NULL);
        if (r >= 0) {
                if (w->server)
                        w->server->event_count += 1;
                return 0;
        } else if (r == -EBADMSG)
                return r;

        log_debug_errno(r, "%s: Write failed, rotating: %m", w->journal->file->path);
        r = do_rotate(&w->journal, w->mmap, file_flags);
        if (r < 0)
                return r;
        else
                log_debug("%s: Successfully rotated journal", w->journal->file->path);
        r = journal_directory_vacuum(w->output, w->metrics.max_use, w->metrics.n_max_files, 0, NULL, /* verbose = */ true);
        if (r < 0)
                return r;

        log_debug("Retrying write.");
        r = journal_file_append_entry(w->journal->file, ts, boot_id,
                                      iovw->iovec, iovw->count,
                                      &w->seqnum, NULL, NULL);
        if (r < 0)
                return r;

        if (w->server)
                w->server->event_count += 1;
        return 0;
}
