/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <linux/fs.h>

#include "sd-daemon.h"
#include "sd-event.h"

#include "alloc-util.h"
#include "btrfs-util.h"
#include "copy.h"
#include "fd-util.h"
#include "fs-util.h"
#include "hostname-util.h"
#include "import-common.h"
#include "import-compress.h"
#include "import-raw.h"
#include "io-util.h"
#include "machine-pool.h"
#include "mkdir.h"
#include "path-util.h"
#include "qcow2-util.h"
#include "ratelimit.h"
#include "rm-rf.h"
#include "string-util.h"
#include "tmpfile-util.h"
#include "util.h"

struct RawImport {
        sd_event *event;

        char *image_root;

        RawImportFinished on_finished;
        void *userdata;

        char *local;
        ImportFlags flags;

        char *temp_path;
        char *final_path;

        int input_fd;
        int output_fd;

        ImportCompress compress;

        sd_event_source *input_event_source;

        uint8_t buffer[16*1024];
        size_t buffer_size;

        uint64_t written_compressed;
        uint64_t written_uncompressed;

        struct stat st;

        unsigned last_percent;
        RateLimit progress_ratelimit;
};

RawImport* raw_import_unref(RawImport *i) {
        if (!i)
                return NULL;

        sd_event_unref(i->event);

        unlink_and_free(i->temp_path);

        import_compress_free(&i->compress);

        sd_event_source_unref(i->input_event_source);

        safe_close(i->output_fd);

        free(i->final_path);
        free(i->image_root);
        free(i->local);
        return mfree(i);
}

int raw_import_new(
                RawImport **ret,
                sd_event *event,
                const char *image_root,
                RawImportFinished on_finished,
                void *userdata) {

        _cleanup_(raw_import_unrefp) RawImport *i = NULL;
        _cleanup_free_ char *root = NULL;
        int r;

        assert(ret);

        root = strdup(image_root ?: "/var/lib/machines");
        if (!root)
                return -ENOMEM;

        i = new(RawImport, 1);
        if (!i)
                return -ENOMEM;

        *i = (RawImport) {
                .input_fd = -1,
                .output_fd = -1,
                .on_finished = on_finished,
                .userdata = userdata,
                .last_percent = UINT_MAX,
                .image_root = TAKE_PTR(root),
                .progress_ratelimit = { 100 * USEC_PER_MSEC, 1 },
        };

        if (event)
                i->event = sd_event_ref(event);
        else {
                r = sd_event_default(&i->event);
                if (r < 0)
                        return r;
        }

        *ret = TAKE_PTR(i);

        return 0;
}

static void raw_import_report_progress(RawImport *i) {
        unsigned percent;
        assert(i);

        /* We have no size information, unless the source is a regular file */
        if (!S_ISREG(i->st.st_mode))
                return;

        if (i->written_compressed >= (uint64_t) i->st.st_size)
                percent = 100;
        else
                percent = (unsigned) ((i->written_compressed * UINT64_C(100)) / (uint64_t) i->st.st_size);

        if (percent == i->last_percent)
                return;

        if (!ratelimit_below(&i->progress_ratelimit))
                return;

        sd_notifyf(false, "X_IMPORT_PROGRESS=%u", percent);
        log_info("Imported %u%%.", percent);

        i->last_percent = percent;
}

static int raw_import_maybe_convert_qcow2(RawImport *i) {
        _cleanup_close_ int converted_fd = -1;
        _cleanup_free_ char *t = NULL;
        int r;

        assert(i);

        r = qcow2_detect(i->output_fd);
        if (r < 0)
                return log_error_errno(r, "Failed to detect whether this is a QCOW2 image: %m");
        if (r == 0)
                return 0;

        /* This is a QCOW2 image, let's convert it */
        r = tempfn_random(i->final_path, NULL, &t);
        if (r < 0)
                return log_oom();

        converted_fd = open(t, O_RDWR|O_CREAT|O_EXCL|O_NOCTTY|O_CLOEXEC, 0664);
        if (converted_fd < 0)
                return log_error_errno(errno, "Failed to create %s: %m", t);

        (void) import_set_nocow_and_log(converted_fd, t);

        log_info("Unpacking QCOW2 file.");

        r = qcow2_convert(i->output_fd, converted_fd);
        if (r < 0) {
                (void) unlink(t);
                return log_error_errno(r, "Failed to convert qcow2 image: %m");
        }

        (void) unlink(i->temp_path);
        free_and_replace(i->temp_path, t);
        CLOSE_AND_REPLACE(i->output_fd, converted_fd);

        return 1;
}

static int raw_import_finish(RawImport *i) {
        int r;

        assert(i);
        assert(i->output_fd >= 0);
        assert(i->temp_path);
        assert(i->final_path);

        /* In case this was a sparse file, make sure the file system is right */
        if (i->written_uncompressed > 0) {
                if (ftruncate(i->output_fd, i->written_uncompressed) < 0)
                        return log_error_errno(errno, "Failed to truncate file: %m");
        }

        r = raw_import_maybe_convert_qcow2(i);
        if (r < 0)
                return r;

        if (S_ISREG(i->st.st_mode)) {
                (void) copy_times(i->input_fd, i->output_fd, COPY_CRTIME);
                (void) copy_xattr(i->input_fd, i->output_fd, 0);
        }

        if (i->flags & IMPORT_READ_ONLY) {
                r = import_make_read_only_fd(i->output_fd);
                if (r < 0)
                        return r;
        }

        if (i->flags & IMPORT_FORCE)
                (void) rm_rf(i->final_path, REMOVE_ROOT|REMOVE_PHYSICAL|REMOVE_SUBVOLUME);

        r = rename_noreplace(AT_FDCWD, i->temp_path, AT_FDCWD, i->final_path);
        if (r < 0)
                return log_error_errno(r, "Failed to move image into place: %m");

        i->temp_path = mfree(i->temp_path);

        return 0;
}

static int raw_import_open_disk(RawImport *i) {
        int r;

        assert(i);

        assert(!i->final_path);
        assert(!i->temp_path);
        assert(i->output_fd < 0);

        i->final_path = strjoin(i->image_root, "/", i->local, ".raw");
        if (!i->final_path)
                return log_oom();

        r = tempfn_random(i->final_path, NULL, &i->temp_path);
        if (r < 0)
                return log_oom();

        (void) mkdir_parents_label(i->temp_path, 0700);

        i->output_fd = open(i->temp_path, O_RDWR|O_CREAT|O_EXCL|O_NOCTTY|O_CLOEXEC, 0664);
        if (i->output_fd < 0)
                return log_error_errno(errno, "Failed to open destination %s: %m", i->temp_path);

        (void) import_set_nocow_and_log(i->output_fd, i->temp_path);
        return 0;
}

static int raw_import_try_reflink(RawImport *i) {
        off_t p;
        int r;

        assert(i);
        assert(i->input_fd >= 0);
        assert(i->output_fd >= 0);

        if (i->compress.type != IMPORT_COMPRESS_UNCOMPRESSED)
                return 0;

        if (!S_ISREG(i->st.st_mode))
                return 0;

        p = lseek(i->input_fd, 0, SEEK_CUR);
        if (p == (off_t) -1)
                return log_error_errno(errno, "Failed to read file offset of input file: %m");

        /* Let's only try a btrfs reflink, if we are reading from the beginning of the file */
        if ((uint64_t) p != (uint64_t) i->buffer_size)
                return 0;

        r = btrfs_reflink(i->input_fd, i->output_fd);
        if (r >= 0)
                return 1;

        return 0;
}

static int raw_import_write(const void *p, size_t sz, void *userdata) {
        RawImport *i = userdata;
        ssize_t n;

        n = sparse_write(i->output_fd, p, sz, 64);
        if (n < 0)
                return (int) n;
        if ((size_t) n < sz)
                return -EIO;

        i->written_uncompressed += sz;

        return 0;
}

static int raw_import_process(RawImport *i) {
        ssize_t l;
        int r;

        assert(i);
        assert(i->buffer_size < sizeof(i->buffer));

        l = read(i->input_fd, i->buffer + i->buffer_size, sizeof(i->buffer) - i->buffer_size);
        if (l < 0) {
                if (errno == EAGAIN)
                        return 0;

                r = log_error_errno(errno, "Failed to read input file: %m");
                goto finish;
        }

        i->buffer_size += l;

        if (i->compress.type == IMPORT_COMPRESS_UNKNOWN) {

                if (l == 0) { /* EOF */
                        log_debug("File too short to be compressed, as no compression signature fits in, thus assuming uncompressed.");
                        import_uncompress_force_off(&i->compress);
                } else {
                        r = import_uncompress_detect(&i->compress, i->buffer, i->buffer_size);
                        if (r < 0) {
                                log_error_errno(r, "Failed to detect file compression: %m");
                                goto finish;
                        }
                        if (r == 0) /* Need more data */
                                return 0;
                }

                r = raw_import_open_disk(i);
                if (r < 0)
                        goto finish;

                r = raw_import_try_reflink(i);
                if (r < 0)
                        goto finish;
                if (r > 0)
                        goto complete;
        }

        r = import_uncompress(&i->compress, i->buffer, i->buffer_size, raw_import_write, i);
        if (r < 0) {
                log_error_errno(r, "Failed to decode and write: %m");
                goto finish;
        }

        i->written_compressed += i->buffer_size;
        i->buffer_size = 0;

        if (l == 0) /* EOF */
                goto complete;

        raw_import_report_progress(i);

        return 0;

complete:
        r = raw_import_finish(i);

finish:
        if (i->on_finished)
                i->on_finished(i, r, i->userdata);
        else
                sd_event_exit(i->event, r);

        return 0;
}

static int raw_import_on_input(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
        RawImport *i = userdata;

        return raw_import_process(i);
}

static int raw_import_on_defer(sd_event_source *s, void *userdata) {
        RawImport *i = userdata;

        return raw_import_process(i);
}

int raw_import_start(RawImport *i, int fd, const char *local, ImportFlags flags) {
        int r;

        assert(i);
        assert(fd >= 0);
        assert(local);
        assert(!(flags & ~IMPORT_FLAGS_MASK));

        if (!hostname_is_valid(local, 0))
                return -EINVAL;

        if (i->input_fd >= 0)
                return -EBUSY;

        r = fd_nonblock(fd, true);
        if (r < 0)
                return r;

        r = free_and_strdup(&i->local, local);
        if (r < 0)
                return r;

        i->flags = flags;

        if (fstat(fd, &i->st) < 0)
                return -errno;

        r = sd_event_add_io(i->event, &i->input_event_source, fd, EPOLLIN, raw_import_on_input, i);
        if (r == -EPERM) {
                /* This fd does not support epoll, for example because it is a regular file. Busy read in that case */
                r = sd_event_add_defer(i->event, &i->input_event_source, raw_import_on_defer, i);
                if (r < 0)
                        return r;

                r = sd_event_source_set_enabled(i->input_event_source, SD_EVENT_ON);
        }
        if (r < 0)
                return r;

        i->input_fd = fd;
        return r;
}
