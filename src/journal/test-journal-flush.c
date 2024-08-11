/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <fcntl.h>
#include <unistd.h>

#include "sd-journal.h"

#include "alloc-util.h"
#include "chattr-util.h"
#include "journal-internal.h"
#include "logs-show.h"
#include "macro.h"
#include "managed-journal-file.h"
#include "path-util.h"
#include "string-util.h"
#include "tests.h"

static void test_journal_flush_one(int argc, char *argv[]) {
        _cleanup_(mmap_cache_unrefp) MMapCache *m = NULL;
        _cleanup_free_ char *fn = NULL;
        char dn[] = "/var/tmp/test-journal-flush.XXXXXX";
        _cleanup_(managed_journal_file_closep) ManagedJournalFile *new_journal = NULL;
        _cleanup_(sd_journal_closep) sd_journal *j = NULL;
        unsigned n, limit;
        int r;

        m = mmap_cache_new();
        assert_se(m != NULL);
        assert_se(mkdtemp(dn));
        (void) chattr_path(dn, FS_NOCOW_FL, FS_NOCOW_FL, NULL);

        fn = path_join(dn, "test.journal");

        r = managed_journal_file_open(-1, fn, O_CREAT|O_RDWR, 0, 0644, 0, NULL, m, NULL, NULL, &new_journal);
        assert_se(r >= 0);

        if (argc > 1)
                r = sd_journal_open_files(&j, (const char **) strv_skip(argv, 1), 0);
        else
                r = sd_journal_open(&j, 0);
        assert_se(r == 0);

        sd_journal_set_data_threshold(j, 0);

        n = 0;
        limit = slow_tests_enabled() ? 10000 : 1000;
        SD_JOURNAL_FOREACH(j) {
                Object *o;
                JournalFile *f;

                f = j->current_file;
                assert_se(f && f->current_offset > 0);

                r = journal_file_move_to_object(f, OBJECT_ENTRY, f->current_offset, &o);
                if (r < 0)
                        log_error_errno(r, "journal_file_move_to_object failed: %m");
                assert_se(r >= 0);

                r = journal_file_copy_entry(f, new_journal->file, o, f->current_offset);
                if (r < 0)
                        log_warning_errno(r, "journal_file_copy_entry failed: %m");
                assert_se(r >= 0 ||
                          IN_SET(r, -EBADMSG,         /* corrupted file */
                                    -EPROTONOSUPPORT, /* unsupported compression */
                                    -EIO));           /* file rotated */

                if (++n >= limit)
                        break;
        }

        if (n == 0)
                return (void) log_tests_skipped("No journal entry found");

        /* Open the new journal before archiving and offlining the file. */
        sd_journal_close(j);
        assert_se(sd_journal_open_directory(&j, dn, 0) >= 0);

        /* Read the online journal. */
        assert_se(sd_journal_seek_tail(j) >= 0);
        assert_se(sd_journal_step_one(j, 0) > 0);
        printf("current_journal: %s (%i)\n", j->current_file->path, j->current_file->fd);
        assert_se(show_journal_entry(stdout, j, OUTPUT_EXPORT, 0, 0, NULL, NULL, NULL, &(dual_timestamp) {}, &(sd_id128_t) {}) >= 0);

        uint64_t p;
        assert_se(journal_file_tail_end_by_mmap(j->current_file, &p) >= 0);
        for (uint64_t q = ALIGN64(p + 1); q < (uint64_t) j->current_file->last_stat.st_size; q = ALIGN64(q + 1)) {
                Object *o;

                r = journal_file_move_to_object(j->current_file, OBJECT_UNUSED, q, &o);
                assert_se(IN_SET(r, -EBADMSG, -EADDRNOTAVAIL));
        }

        /* Archive and offline file. */
        assert_se(journal_file_archive(new_journal, NULL) >= 0);
        assert_se(journal_file_set_offline(new_journal, /* wait = */ true) >= 0);

        /* Read the archived and offline journal. */
        for (uint64_t q = ALIGN64(p + 1); q < (uint64_t) j->current_file->last_stat.st_size; q = ALIGN64(q + 1)) {
                Object *o;

                r = journal_file_move_to_object(j->current_file, OBJECT_UNUSED, q, &o);
                assert_se(IN_SET(r, -EBADMSG, -EADDRNOTAVAIL, -EIDRM));
        }

        unlink(fn);
        assert_se(rmdir(dn) == 0);
}

TEST(journal_flush) {
        assert_se(setenv("SYSTEMD_JOURNAL_COMPACT", "0", 1) >= 0);
        test_journal_flush_one(saved_argc, saved_argv);
}

TEST(journal_flush_compact) {
        assert_se(setenv("SYSTEMD_JOURNAL_COMPACT", "1", 1) >= 0);
        test_journal_flush_one(saved_argc, saved_argv);
}

DEFINE_TEST_MAIN(LOG_INFO);
