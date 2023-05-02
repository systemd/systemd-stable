/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "fuzz.h"

#include <sys/mman.h>

#include "sd-journal.h"

#include "env-util.h"
#include "fd-util.h"
#include "fileio.h"
#include "fs-util.h"
#include "journal-remote.h"
#include "logs-show.h"
#include "memfd-util.h"
#include "path-util.h"
#include "rm-rf.h"
#include "strv.h"
#include "tmpfile-util.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
        _cleanup_close_ int fdin_close = -EBADF, fdout = -EBADF;
        _cleanup_(rm_rf_physical_and_freep) char *tmp = NULL;
        _cleanup_(unlink_and_freep) char *name = NULL;
        _cleanup_(sd_journal_closep) sd_journal *j = NULL;
        _cleanup_(journal_remote_server_destroy) RemoteServer s = {};
        void *mem;
        int fdin, r;

        if (outside_size_range(size, 3, 65536))
                return 0;

        if (!getenv("SYSTEMD_LOG_LEVEL"))
                log_set_max_level(LOG_ERR);

        assert_se(mkdtemp_malloc("/tmp/fuzz-journal-remote-XXXXXX", &tmp) >= 0);
        assert_se(name = path_join(tmp, "fuzz-journal-remote.XXXXXX.journal"));

        fdin = fdin_close = memfd_new_and_map("fuzz-journal-remote", size, &mem);
        if (fdin < 0)
                return log_error_errno(fdin, "memfd_new_and_map() failed: %m");

        memcpy(mem, data, size);
        assert_se(munmap(mem, size) == 0);

        fdout = mkostemps(name, STRLEN(".journal"), O_CLOEXEC);
        if (fdout < 0)
                return log_error_errno(errno, "mkostemps() failed: %m");

        /* In */

        r = journal_remote_server_init(&s, name, JOURNAL_WRITE_SPLIT_NONE, 0);
        if (r < 0) {
                assert_se(IN_SET(r, -ENOMEM, -EMFILE, -ENFILE));
                return r;
        }

        r = journal_remote_add_source(&s, fdin, (char*) "fuzz-data", false);
        if (r < 0)
                return r;
        TAKE_FD(fdin_close);
        assert(r > 0);

        while (s.active)
                assert_se(journal_remote_handle_raw_source(NULL, fdin, 0, &s) >= 0);

        assert_se(close(fdin) < 0 && errno == EBADF); /* Check that the fd is closed already */

        /* Out */

        r = sd_journal_open_files(&j, (const char**) STRV_MAKE(name), 0);
        if (r < 0) {
                log_error_errno(r, "sd_journal_open_files([\"%s\"]) failed: %m", name);
                assert_se(IN_SET(r, -ENOMEM, -EMFILE, -ENFILE, -ENODATA));
                return r;
        }

        _cleanup_fclose_ FILE *dev_null = NULL;
        if (getenv_bool("SYSTEMD_FUZZ_OUTPUT") <= 0) {
                dev_null = fopen("/dev/null", "we");
                if (!dev_null)
                        return log_error_errno(errno, "fopen(\"/dev/null\") failed: %m");
        }

        for (OutputMode mode = 0; mode < _OUTPUT_MODE_MAX; mode++) {
                if (!dev_null)
                        log_info("/* %s */", output_mode_to_string(mode));
                r = show_journal(dev_null ?: stdout, j, mode, 0, 0, -1, 0, NULL);
                assert_se(r >= 0);

                r = sd_journal_seek_head(j);
                assert_se(r >= 0);
        }

        return 0;
}
