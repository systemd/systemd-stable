/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/magic.h>
#include <pthread.h>
#include <stddef.h>
#include <sys/mman.h>
#include <sys/statvfs.h>
#include <sys/uio.h>
#include <unistd.h>

#include "sd-event.h"

#include "alloc-util.h"
#include "chattr-util.h"
#include "compress.h"
#include "env-util.h"
#include "fd-util.h"
#include "format-util.h"
#include "fs-util.h"
#include "journal-authenticate.h"
#include "journal-def.h"
#include "journal-file.h"
#include "lookup3.h"
#include "memory-util.h"
#include "path-util.h"
#include "random-util.h"
#include "set.h"
#include "sort-util.h"
#include "stat-util.h"
#include "string-util.h"
#include "strv.h"
#include "xattr-util.h"

#define DEFAULT_DATA_HASH_TABLE_SIZE (2047ULL*sizeof(HashItem))
#define DEFAULT_FIELD_HASH_TABLE_SIZE (333ULL*sizeof(HashItem))

#define DEFAULT_COMPRESS_THRESHOLD (512ULL)
#define MIN_COMPRESS_THRESHOLD (8ULL)

/* This is the minimum journal file size */
#define JOURNAL_FILE_SIZE_MIN (512 * 1024ULL)             /* 512 KiB */

/* These are the lower and upper bounds if we deduce the max_use value
 * from the file system size */
#define MAX_USE_LOWER (1 * 1024 * 1024ULL)                /* 1 MiB */
#define MAX_USE_UPPER (4 * 1024 * 1024 * 1024ULL)         /* 4 GiB */

/* Those are the lower and upper bounds for the minimal use limit,
 * i.e. how much we'll use even if keep_free suggests otherwise. */
#define MIN_USE_LOW (1 * 1024 * 1024ULL)                  /* 1 MiB */
#define MIN_USE_HIGH (16 * 1024 * 1024ULL)                /* 16 MiB */

/* This is the upper bound if we deduce max_size from max_use */
#define MAX_SIZE_UPPER (128 * 1024 * 1024ULL)             /* 128 MiB */

/* This is the upper bound if we deduce the keep_free value from the
 * file system size */
#define KEEP_FREE_UPPER (4 * 1024 * 1024 * 1024ULL)       /* 4 GiB */

/* This is the keep_free value when we can't determine the system
 * size */
#define DEFAULT_KEEP_FREE (1024 * 1024ULL)                /* 1 MB */

/* This is the default maximum number of journal files to keep around. */
#define DEFAULT_N_MAX_FILES 100

/* n_data was the first entry we added after the initial file format design */
#define HEADER_SIZE_MIN ALIGN64(offsetof(Header, n_data))

/* How many entries to keep in the entry array chain cache at max */
#define CHAIN_CACHE_MAX 20

/* How much to increase the journal file size at once each time we allocate something new. */
#define FILE_SIZE_INCREASE (8 * 1024 * 1024ULL)          /* 8MB */

/* Reread fstat() of the file for detecting deletions at least this often */
#define LAST_STAT_REFRESH_USEC (5*USEC_PER_SEC)

/* The mmap context to use for the header we pick as one above the last defined typed */
#define CONTEXT_HEADER _OBJECT_TYPE_MAX

/* Longest hash chain to rotate after */
#define HASH_CHAIN_DEPTH_MAX 100

#ifdef __clang__
#  pragma GCC diagnostic ignored "-Waddress-of-packed-member"
#endif

/* This may be called from a separate thread to prevent blocking the caller for the duration of fsync().
 * As a result we use atomic operations on f->offline_state for inter-thread communications with
 * journal_file_set_offline() and journal_file_set_online(). */
static void journal_file_set_offline_internal(JournalFile *f) {
        assert(f);
        assert(f->fd >= 0);
        assert(f->header);

        for (;;) {
                switch (f->offline_state) {
                case OFFLINE_CANCEL:
                        if (!__sync_bool_compare_and_swap(&f->offline_state, OFFLINE_CANCEL, OFFLINE_DONE))
                                continue;
                        return;

                case OFFLINE_AGAIN_FROM_SYNCING:
                        if (!__sync_bool_compare_and_swap(&f->offline_state, OFFLINE_AGAIN_FROM_SYNCING, OFFLINE_SYNCING))
                                continue;
                        break;

                case OFFLINE_AGAIN_FROM_OFFLINING:
                        if (!__sync_bool_compare_and_swap(&f->offline_state, OFFLINE_AGAIN_FROM_OFFLINING, OFFLINE_SYNCING))
                                continue;
                        break;

                case OFFLINE_SYNCING:
                        (void) fsync(f->fd);

                        if (!__sync_bool_compare_and_swap(&f->offline_state, OFFLINE_SYNCING, OFFLINE_OFFLINING))
                                continue;

                        f->header->state = f->archive ? STATE_ARCHIVED : STATE_OFFLINE;
                        (void) fsync(f->fd);
                        break;

                case OFFLINE_OFFLINING:
                        if (!__sync_bool_compare_and_swap(&f->offline_state, OFFLINE_OFFLINING, OFFLINE_DONE))
                                continue;
                        _fallthrough_;
                case OFFLINE_DONE:
                        return;

                case OFFLINE_JOINED:
                        log_debug("OFFLINE_JOINED unexpected offline state for journal_file_set_offline_internal()");
                        return;
                }
        }
}

static void * journal_file_set_offline_thread(void *arg) {
        JournalFile *f = arg;

        (void) pthread_setname_np(pthread_self(), "journal-offline");

        journal_file_set_offline_internal(f);

        return NULL;
}

static int journal_file_set_offline_thread_join(JournalFile *f) {
        int r;

        assert(f);

        if (f->offline_state == OFFLINE_JOINED)
                return 0;

        r = pthread_join(f->offline_thread, NULL);
        if (r)
                return -r;

        f->offline_state = OFFLINE_JOINED;

        if (mmap_cache_got_sigbus(f->mmap, f->cache_fd))
                return -EIO;

        return 0;
}

/* Trigger a restart if the offline thread is mid-flight in a restartable state. */
static bool journal_file_set_offline_try_restart(JournalFile *f) {
        for (;;) {
                switch (f->offline_state) {
                case OFFLINE_AGAIN_FROM_SYNCING:
                case OFFLINE_AGAIN_FROM_OFFLINING:
                        return true;

                case OFFLINE_CANCEL:
                        if (!__sync_bool_compare_and_swap(&f->offline_state, OFFLINE_CANCEL, OFFLINE_AGAIN_FROM_SYNCING))
                                continue;
                        return true;

                case OFFLINE_SYNCING:
                        if (!__sync_bool_compare_and_swap(&f->offline_state, OFFLINE_SYNCING, OFFLINE_AGAIN_FROM_SYNCING))
                                continue;
                        return true;

                case OFFLINE_OFFLINING:
                        if (!__sync_bool_compare_and_swap(&f->offline_state, OFFLINE_OFFLINING, OFFLINE_AGAIN_FROM_OFFLINING))
                                continue;
                        return true;

                default:
                        return false;
                }
        }
}

/* Sets a journal offline.
 *
 * If wait is false then an offline is dispatched in a separate thread for a
 * subsequent journal_file_set_offline() or journal_file_set_online() of the
 * same journal to synchronize with.
 *
 * If wait is true, then either an existing offline thread will be restarted
 * and joined, or if none exists the offline is simply performed in this
 * context without involving another thread.
 */
int journal_file_set_offline(JournalFile *f, bool wait) {
        int target_state;
        bool restarted;
        int r;

        assert(f);

        if (!f->writable)
                return -EPERM;

        if (f->fd < 0 || !f->header)
                return -EINVAL;

        target_state = f->archive ? STATE_ARCHIVED : STATE_OFFLINE;

        /* An offlining journal is implicitly online and may modify f->header->state,
         * we must also join any potentially lingering offline thread when already in
         * the desired offline state.
         */
        if (!journal_file_is_offlining(f) && f->header->state == target_state)
                return journal_file_set_offline_thread_join(f);

        /* Restart an in-flight offline thread and wait if needed, or join a lingering done one. */
        restarted = journal_file_set_offline_try_restart(f);
        if ((restarted && wait) || !restarted) {
                r = journal_file_set_offline_thread_join(f);
                if (r < 0)
                        return r;
        }

        if (restarted)
                return 0;

        /* Initiate a new offline. */
        f->offline_state = OFFLINE_SYNCING;

        if (wait) /* Without using a thread if waiting. */
                journal_file_set_offline_internal(f);
        else {
                sigset_t ss, saved_ss;
                int k;

                assert_se(sigfillset(&ss) >= 0);
                /* Don't block SIGBUS since the offlining thread accesses a memory mapped file.
                 * Asynchronous SIGBUS signals can safely be handled by either thread. */
                assert_se(sigdelset(&ss, SIGBUS) >= 0);

                r = pthread_sigmask(SIG_BLOCK, &ss, &saved_ss);
                if (r > 0)
                        return -r;

                r = pthread_create(&f->offline_thread, NULL, journal_file_set_offline_thread, f);

                k = pthread_sigmask(SIG_SETMASK, &saved_ss, NULL);
                if (r > 0) {
                        f->offline_state = OFFLINE_JOINED;
                        return -r;
                }
                if (k > 0)
                        return -k;
        }

        return 0;
}

static int journal_file_set_online(JournalFile *f) {
        bool wait = true;

        assert(f);

        if (!f->writable)
                return -EPERM;

        if (f->fd < 0 || !f->header)
                return -EINVAL;

        while (wait) {
                switch (f->offline_state) {
                case OFFLINE_JOINED:
                        /* No offline thread, no need to wait. */
                        wait = false;
                        break;

                case OFFLINE_SYNCING:
                        if (!__sync_bool_compare_and_swap(&f->offline_state, OFFLINE_SYNCING, OFFLINE_CANCEL))
                                continue;
                        /* Canceled syncing prior to offlining, no need to wait. */
                        wait = false;
                        break;

                case OFFLINE_AGAIN_FROM_SYNCING:
                        if (!__sync_bool_compare_and_swap(&f->offline_state, OFFLINE_AGAIN_FROM_SYNCING, OFFLINE_CANCEL))
                                continue;
                        /* Canceled restart from syncing, no need to wait. */
                        wait = false;
                        break;

                case OFFLINE_AGAIN_FROM_OFFLINING:
                        if (!__sync_bool_compare_and_swap(&f->offline_state, OFFLINE_AGAIN_FROM_OFFLINING, OFFLINE_CANCEL))
                                continue;
                        /* Canceled restart from offlining, must wait for offlining to complete however. */
                        _fallthrough_;
                default: {
                        int r;

                        r = journal_file_set_offline_thread_join(f);
                        if (r < 0)
                                return r;

                        wait = false;
                        break;
                }
                }
        }

        if (mmap_cache_got_sigbus(f->mmap, f->cache_fd))
                return -EIO;

        switch (f->header->state) {
                case STATE_ONLINE:
                        return 0;

                case STATE_OFFLINE:
                        f->header->state = STATE_ONLINE;
                        (void) fsync(f->fd);
                        return 0;

                default:
                        return -EINVAL;
        }
}

bool journal_file_is_offlining(JournalFile *f) {
        assert(f);

        __sync_synchronize();

        if (IN_SET(f->offline_state, OFFLINE_DONE, OFFLINE_JOINED))
                return false;

        return true;
}

JournalFile* journal_file_close(JournalFile *f) {
        if (!f)
                return NULL;

#if HAVE_GCRYPT
        /* Write the final tag */
        if (f->seal && f->writable) {
                int r;

                r = journal_file_append_tag(f);
                if (r < 0)
                        log_error_errno(r, "Failed to append tag when closing journal: %m");
        }
#endif

        if (f->post_change_timer) {
                if (sd_event_source_get_enabled(f->post_change_timer, NULL) > 0)
                        journal_file_post_change(f);

                sd_event_source_disable_unref(f->post_change_timer);
        }

        journal_file_set_offline(f, true);

        if (f->mmap && f->cache_fd)
                mmap_cache_free_fd(f->mmap, f->cache_fd);

        if (f->fd >= 0 && f->defrag_on_close) {

                /* Be friendly to btrfs: turn COW back on again now,
                 * and defragment the file. We won't write to the file
                 * ever again, hence remove all fragmentation, and
                 * reenable all the good bits COW usually provides
                 * (such as data checksumming). */

                (void) chattr_fd(f->fd, 0, FS_NOCOW_FL, NULL);
                (void) btrfs_defrag_fd(f->fd);
        }

        if (f->close_fd)
                safe_close(f->fd);
        free(f->path);

        mmap_cache_unref(f->mmap);

        ordered_hashmap_free_free(f->chain_cache);

#if HAVE_COMPRESSION
        free(f->compress_buffer);
#endif

#if HAVE_GCRYPT
        if (f->fss_file)
                munmap(f->fss_file, PAGE_ALIGN(f->fss_file_size));
        else
                free(f->fsprg_state);

        free(f->fsprg_seed);

        if (f->hmac)
                gcry_md_close(f->hmac);
#endif

        return mfree(f);
}

static int journal_file_init_header(JournalFile *f, JournalFile *template) {
        Header h = {};
        ssize_t k;
        int r;

        assert(f);

        memcpy(h.signature, HEADER_SIGNATURE, 8);
        h.header_size = htole64(ALIGN64(sizeof(h)));

        h.incompatible_flags |= htole32(
                f->compress_xz * HEADER_INCOMPATIBLE_COMPRESSED_XZ |
                f->compress_lz4 * HEADER_INCOMPATIBLE_COMPRESSED_LZ4 |
                f->compress_zstd * HEADER_INCOMPATIBLE_COMPRESSED_ZSTD |
                f->keyed_hash * HEADER_INCOMPATIBLE_KEYED_HASH);

        h.compatible_flags = htole32(
                f->seal * HEADER_COMPATIBLE_SEALED);

        r = sd_id128_randomize(&h.file_id);
        if (r < 0)
                return r;

        if (template) {
                h.seqnum_id = template->header->seqnum_id;
                h.tail_entry_seqnum = template->header->tail_entry_seqnum;
        } else
                h.seqnum_id = h.file_id;

        k = pwrite(f->fd, &h, sizeof(h), 0);
        if (k < 0)
                return -errno;

        if (k != sizeof(h))
                return -EIO;

        return 0;
}

static int journal_file_refresh_header(JournalFile *f) {
        int r;

        assert(f);
        assert(f->header);

        r = sd_id128_get_machine(&f->header->machine_id);
        if (IN_SET(r, -ENOENT, -ENOMEDIUM))
                /* We don't have a machine-id, let's continue without */
                zero(f->header->machine_id);
        else if (r < 0)
                return r;

        r = sd_id128_get_boot(&f->header->boot_id);
        if (r < 0)
                return r;

        r = journal_file_set_online(f);

        /* Sync the online state to disk */
        (void) fsync(f->fd);

        /* We likely just created a new file, also sync the directory this file is located in. */
        (void) fsync_directory_of_file(f->fd);

        return r;
}

static bool warn_wrong_flags(const JournalFile *f, bool compatible) {
        const uint32_t any = compatible ? HEADER_COMPATIBLE_ANY : HEADER_INCOMPATIBLE_ANY,
                supported = compatible ? HEADER_COMPATIBLE_SUPPORTED : HEADER_INCOMPATIBLE_SUPPORTED;
        const char *type = compatible ? "compatible" : "incompatible";
        uint32_t flags;

        flags = le32toh(compatible ? f->header->compatible_flags : f->header->incompatible_flags);

        if (flags & ~supported) {
                if (flags & ~any)
                        log_debug("Journal file %s has unknown %s flags 0x%"PRIx32,
                                  f->path, type, flags & ~any);
                flags = (flags & any) & ~supported;
                if (flags) {
                        const char* strv[5];
                        unsigned n = 0;
                        _cleanup_free_ char *t = NULL;

                        if (compatible) {
                                if (flags & HEADER_COMPATIBLE_SEALED)
                                        strv[n++] = "sealed";
                        } else {
                                if (flags & HEADER_INCOMPATIBLE_COMPRESSED_XZ)
                                        strv[n++] = "xz-compressed";
                                if (flags & HEADER_INCOMPATIBLE_COMPRESSED_LZ4)
                                        strv[n++] = "lz4-compressed";
                                if (flags & HEADER_INCOMPATIBLE_COMPRESSED_ZSTD)
                                        strv[n++] = "zstd-compressed";
                                if (flags & HEADER_INCOMPATIBLE_KEYED_HASH)
                                        strv[n++] = "keyed-hash";
                        }
                        strv[n] = NULL;
                        assert(n < ELEMENTSOF(strv));

                        t = strv_join((char**) strv, ", ");
                        log_debug("Journal file %s uses %s %s %s disabled at compilation time.",
                                  f->path, type, n > 1 ? "flags" : "flag", strnull(t));
                }
                return true;
        }

        return false;
}

static int journal_file_verify_header(JournalFile *f) {
        uint64_t arena_size, header_size;

        assert(f);
        assert(f->header);

        if (memcmp(f->header->signature, HEADER_SIGNATURE, 8))
                return -EBADMSG;

        /* In both read and write mode we refuse to open files with incompatible
         * flags we don't know. */
        if (warn_wrong_flags(f, false))
                return -EPROTONOSUPPORT;

        /* When open for writing we refuse to open files with compatible flags, too. */
        if (f->writable && warn_wrong_flags(f, true))
                return -EPROTONOSUPPORT;

        if (f->header->state >= _STATE_MAX)
                return -EBADMSG;

        header_size = le64toh(READ_NOW(f->header->header_size));

        /* The first addition was n_data, so check that we are at least this large */
        if (header_size < HEADER_SIZE_MIN)
                return -EBADMSG;

        if (JOURNAL_HEADER_SEALED(f->header) && !JOURNAL_HEADER_CONTAINS(f->header, n_entry_arrays))
                return -EBADMSG;

        arena_size = le64toh(READ_NOW(f->header->arena_size));

        if (UINT64_MAX - header_size < arena_size || header_size + arena_size > (uint64_t) f->last_stat.st_size)
                return -ENODATA;

        if (le64toh(f->header->tail_object_offset) > header_size + arena_size)
                return -ENODATA;

        if (!VALID64(le64toh(f->header->data_hash_table_offset)) ||
            !VALID64(le64toh(f->header->field_hash_table_offset)) ||
            !VALID64(le64toh(f->header->tail_object_offset)) ||
            !VALID64(le64toh(f->header->entry_array_offset)))
                return -ENODATA;

        if (f->writable) {
                sd_id128_t machine_id;
                uint8_t state;
                int r;

                r = sd_id128_get_machine(&machine_id);
                if (r < 0)
                        return r;

                if (!sd_id128_equal(machine_id, f->header->machine_id))
                        return -EHOSTDOWN;

                state = f->header->state;

                if (state == STATE_ARCHIVED)
                        return -ESHUTDOWN; /* Already archived */
                else if (state == STATE_ONLINE)
                        return log_debug_errno(SYNTHETIC_ERRNO(EBUSY),
                                               "Journal file %s is already online. Assuming unclean closing.",
                                               f->path);
                else if (state != STATE_OFFLINE)
                        return log_debug_errno(SYNTHETIC_ERRNO(EBUSY),
                                               "Journal file %s has unknown state %i.",
                                               f->path, state);

                if (f->header->field_hash_table_size == 0 || f->header->data_hash_table_size == 0)
                        return -EBADMSG;

                /* Don't permit appending to files from the future. Because otherwise the realtime timestamps wouldn't
                 * be strictly ordered in the entries in the file anymore, and we can't have that since it breaks
                 * bisection. */
                if (le64toh(f->header->tail_entry_realtime) > now(CLOCK_REALTIME))
                        return log_debug_errno(SYNTHETIC_ERRNO(ETXTBSY),
                                               "Journal file %s is from the future, refusing to append new data to it that'd be older.",
                                               f->path);
        }

        f->compress_xz = JOURNAL_HEADER_COMPRESSED_XZ(f->header);
        f->compress_lz4 = JOURNAL_HEADER_COMPRESSED_LZ4(f->header);
        f->compress_zstd = JOURNAL_HEADER_COMPRESSED_ZSTD(f->header);

        f->seal = JOURNAL_HEADER_SEALED(f->header);

        f->keyed_hash = JOURNAL_HEADER_KEYED_HASH(f->header);

        return 0;
}

int journal_file_fstat(JournalFile *f) {
        int r;

        assert(f);
        assert(f->fd >= 0);

        if (fstat(f->fd, &f->last_stat) < 0)
                return -errno;

        f->last_stat_usec = now(CLOCK_MONOTONIC);

        /* Refuse dealing with files that aren't regular */
        r = stat_verify_regular(&f->last_stat);
        if (r < 0)
                return r;

        /* Refuse appending to files that are already deleted */
        if (f->last_stat.st_nlink <= 0)
                return -EIDRM;

        return 0;
}

static int journal_file_allocate(JournalFile *f, uint64_t offset, uint64_t size) {
        uint64_t old_size, new_size, old_header_size, old_arena_size;
        int r;

        assert(f);
        assert(f->header);

        /* We assume that this file is not sparse, and we know that for sure, since we always call
         * posix_fallocate() ourselves */

        if (size > PAGE_ALIGN_DOWN(UINT64_MAX) - offset)
                return -EINVAL;

        if (mmap_cache_got_sigbus(f->mmap, f->cache_fd))
                return -EIO;

        old_header_size = le64toh(READ_NOW(f->header->header_size));
        old_arena_size = le64toh(READ_NOW(f->header->arena_size));
        if (old_arena_size > PAGE_ALIGN_DOWN(UINT64_MAX) - old_header_size)
                return -EBADMSG;

        old_size = old_header_size + old_arena_size;

        new_size = MAX(PAGE_ALIGN(offset + size), old_header_size);

        if (new_size <= old_size) {

                /* We already pre-allocated enough space, but before
                 * we write to it, let's check with fstat() if the
                 * file got deleted, in order make sure we don't throw
                 * away the data immediately. Don't check fstat() for
                 * all writes though, but only once ever 10s. */

                if (f->last_stat_usec + LAST_STAT_REFRESH_USEC > now(CLOCK_MONOTONIC))
                        return 0;

                return journal_file_fstat(f);
        }

        /* Allocate more space. */

        if (f->metrics.max_size > 0 && new_size > f->metrics.max_size)
                return -E2BIG;

        if (new_size > f->metrics.min_size && f->metrics.keep_free > 0) {
                struct statvfs svfs;

                if (fstatvfs(f->fd, &svfs) >= 0) {
                        uint64_t available;

                        available = LESS_BY((uint64_t) svfs.f_bfree * (uint64_t) svfs.f_bsize, f->metrics.keep_free);

                        if (new_size - old_size > available)
                                return -E2BIG;
                }
        }

        /* Increase by larger blocks at once */
        new_size = DIV_ROUND_UP(new_size, FILE_SIZE_INCREASE) * FILE_SIZE_INCREASE;
        if (f->metrics.max_size > 0 && new_size > f->metrics.max_size)
                new_size = f->metrics.max_size;

        /* Note that the glibc fallocate() fallback is very
           inefficient, hence we try to minimize the allocation area
           as we can. */
        r = posix_fallocate_loop(f->fd, old_size, new_size - old_size);
        if (r < 0)
                return r;

        f->header->arena_size = htole64(new_size - old_header_size);

        return journal_file_fstat(f);
}

static unsigned type_to_context(ObjectType type) {
        /* One context for each type, plus one catch-all for the rest */
        assert_cc(_OBJECT_TYPE_MAX <= MMAP_CACHE_MAX_CONTEXTS);
        assert_cc(CONTEXT_HEADER < MMAP_CACHE_MAX_CONTEXTS);
        return type > OBJECT_UNUSED && type < _OBJECT_TYPE_MAX ? type : 0;
}

static int journal_file_move_to(
                JournalFile *f,
                ObjectType type,
                bool keep_always,
                uint64_t offset,
                uint64_t size,
                void **ret) {

        int r;

        assert(f);
        assert(ret);

        if (size <= 0)
                return -EINVAL;

        if (size > UINT64_MAX - offset)
                return -EBADMSG;

        /* Avoid SIGBUS on invalid accesses */
        if (offset + size > (uint64_t) f->last_stat.st_size) {
                /* Hmm, out of range? Let's refresh the fstat() data
                 * first, before we trust that check. */

                r = journal_file_fstat(f);
                if (r < 0)
                        return r;

                if (offset + size > (uint64_t) f->last_stat.st_size)
                        return -EADDRNOTAVAIL;
        }

        return mmap_cache_get(f->mmap, f->cache_fd, type_to_context(type), keep_always, offset, size, &f->last_stat, ret);
}

static uint64_t minimum_header_size(Object *o) {

        static const uint64_t table[] = {
                [OBJECT_DATA] = sizeof(DataObject),
                [OBJECT_FIELD] = sizeof(FieldObject),
                [OBJECT_ENTRY] = sizeof(EntryObject),
                [OBJECT_DATA_HASH_TABLE] = sizeof(HashTableObject),
                [OBJECT_FIELD_HASH_TABLE] = sizeof(HashTableObject),
                [OBJECT_ENTRY_ARRAY] = sizeof(EntryArrayObject),
                [OBJECT_TAG] = sizeof(TagObject),
        };

        if (o->object.type >= ELEMENTSOF(table) || table[o->object.type] <= 0)
                return sizeof(ObjectHeader);

        return table[o->object.type];
}

/* Lightweight object checks. We want this to be fast, so that we won't
 * slowdown every journal_file_move_to_object() call too much. */
static int journal_file_check_object(JournalFile *f, uint64_t offset, Object *o) {
        assert(f);
        assert(o);

        switch (o->object.type) {

        case OBJECT_DATA:
                if ((le64toh(o->data.entry_offset) == 0) ^ (le64toh(o->data.n_entries) == 0))
                        return log_debug_errno(SYNTHETIC_ERRNO(EBADMSG),
                                               "Bad n_entries: %" PRIu64 ": %" PRIu64,
                                               le64toh(o->data.n_entries),
                                               offset);

                if (le64toh(o->object.size) <= offsetof(DataObject, payload))
                        return log_debug_errno(SYNTHETIC_ERRNO(EBADMSG),
                                               "Bad object size (<= %zu): %" PRIu64 ": %" PRIu64,
                                               offsetof(DataObject, payload),
                                               le64toh(o->object.size),
                                               offset);

                if (!VALID64(le64toh(o->data.next_hash_offset)) ||
                    !VALID64(le64toh(o->data.next_field_offset)) ||
                    !VALID64(le64toh(o->data.entry_offset)) ||
                    !VALID64(le64toh(o->data.entry_array_offset)))
                        return log_debug_errno(SYNTHETIC_ERRNO(EBADMSG),
                                               "Invalid offset, next_hash_offset=" OFSfmt ", next_field_offset=" OFSfmt ", entry_offset=" OFSfmt ", entry_array_offset=" OFSfmt ": %" PRIu64,
                                               le64toh(o->data.next_hash_offset),
                                               le64toh(o->data.next_field_offset),
                                               le64toh(o->data.entry_offset),
                                               le64toh(o->data.entry_array_offset),
                                               offset);

                break;

        case OBJECT_FIELD:
                if (le64toh(o->object.size) <= offsetof(FieldObject, payload))
                        return log_debug_errno(SYNTHETIC_ERRNO(EBADMSG),
                                               "Bad field size (<= %zu): %" PRIu64 ": %" PRIu64,
                                               offsetof(FieldObject, payload),
                                               le64toh(o->object.size),
                                               offset);

                if (!VALID64(le64toh(o->field.next_hash_offset)) ||
                    !VALID64(le64toh(o->field.head_data_offset)))
                        return log_debug_errno(SYNTHETIC_ERRNO(EBADMSG),
                                               "Invalid offset, next_hash_offset=" OFSfmt ", head_data_offset=" OFSfmt ": %" PRIu64,
                                               le64toh(o->field.next_hash_offset),
                                               le64toh(o->field.head_data_offset),
                                               offset);
                break;

        case OBJECT_ENTRY: {
                uint64_t sz;

                sz = le64toh(READ_NOW(o->object.size));
                if (sz < offsetof(EntryObject, items) ||
                    (sz - offsetof(EntryObject, items)) % sizeof(EntryItem) != 0)
                        return log_debug_errno(SYNTHETIC_ERRNO(EBADMSG),
                                               "Bad entry size (<= %zu): %" PRIu64 ": %" PRIu64,
                                               offsetof(EntryObject, items),
                                               sz,
                                               offset);

                if ((sz - offsetof(EntryObject, items)) / sizeof(EntryItem) <= 0)
                        return log_debug_errno(SYNTHETIC_ERRNO(EBADMSG),
                                               "Invalid number items in entry: %" PRIu64 ": %" PRIu64,
                                               (sz - offsetof(EntryObject, items)) / sizeof(EntryItem),
                                               offset);

                if (le64toh(o->entry.seqnum) <= 0)
                        return log_debug_errno(SYNTHETIC_ERRNO(EBADMSG),
                                               "Invalid entry seqnum: %" PRIx64 ": %" PRIu64,
                                               le64toh(o->entry.seqnum),
                                               offset);

                if (!VALID_REALTIME(le64toh(o->entry.realtime)))
                        return log_debug_errno(SYNTHETIC_ERRNO(EBADMSG),
                                               "Invalid entry realtime timestamp: %" PRIu64 ": %" PRIu64,
                                               le64toh(o->entry.realtime),
                                               offset);

                if (!VALID_MONOTONIC(le64toh(o->entry.monotonic)))
                        return log_debug_errno(SYNTHETIC_ERRNO(EBADMSG),
                                               "Invalid entry monotonic timestamp: %" PRIu64 ": %" PRIu64,
                                               le64toh(o->entry.monotonic),
                                               offset);

                break;
        }

        case OBJECT_DATA_HASH_TABLE:
        case OBJECT_FIELD_HASH_TABLE: {
                uint64_t sz;

                sz = le64toh(READ_NOW(o->object.size));
                if (sz < offsetof(HashTableObject, items) ||
                    (sz - offsetof(HashTableObject, items)) % sizeof(HashItem) != 0 ||
                    (sz - offsetof(HashTableObject, items)) / sizeof(HashItem) <= 0)
                        return log_debug_errno(SYNTHETIC_ERRNO(EBADMSG),
                                               "Invalid %s hash table size: %" PRIu64 ": %" PRIu64,
                                               o->object.type == OBJECT_DATA_HASH_TABLE ? "data" : "field",
                                               sz,
                                               offset);

                break;
        }

        case OBJECT_ENTRY_ARRAY: {
                uint64_t sz;

                sz = le64toh(READ_NOW(o->object.size));
                if (sz < offsetof(EntryArrayObject, items) ||
                    (sz - offsetof(EntryArrayObject, items)) % sizeof(le64_t) != 0 ||
                    (sz - offsetof(EntryArrayObject, items)) / sizeof(le64_t) <= 0)
                        return log_debug_errno(SYNTHETIC_ERRNO(EBADMSG),
                                               "Invalid object entry array size: %" PRIu64 ": %" PRIu64,
                                               sz,
                                               offset);

                if (!VALID64(le64toh(o->entry_array.next_entry_array_offset)))
                        return log_debug_errno(SYNTHETIC_ERRNO(EBADMSG),
                                               "Invalid object entry array next_entry_array_offset: " OFSfmt ": %" PRIu64,
                                               le64toh(o->entry_array.next_entry_array_offset),
                                               offset);

                break;
        }

        case OBJECT_TAG:
                if (le64toh(o->object.size) != sizeof(TagObject))
                        return log_debug_errno(SYNTHETIC_ERRNO(EBADMSG),
                                               "Invalid object tag size: %" PRIu64 ": %" PRIu64,
                                               le64toh(o->object.size),
                                               offset);

                if (!VALID_EPOCH(le64toh(o->tag.epoch)))
                        return log_debug_errno(SYNTHETIC_ERRNO(EBADMSG),
                                               "Invalid object tag epoch: %" PRIu64 ": %" PRIu64,
                                               le64toh(o->tag.epoch), offset);

                break;
        }

        return 0;
}

int journal_file_move_to_object(JournalFile *f, ObjectType type, uint64_t offset, Object **ret) {
        int r;
        void *t;
        Object *o;
        uint64_t s;

        assert(f);
        assert(ret);

        /* Objects may only be located at multiple of 64 bit */
        if (!VALID64(offset))
                return log_debug_errno(SYNTHETIC_ERRNO(EBADMSG),
                                       "Attempt to move to object at non-64bit boundary: %" PRIu64,
                                       offset);

        /* Object may not be located in the file header */
        if (offset < le64toh(f->header->header_size))
                return log_debug_errno(SYNTHETIC_ERRNO(EBADMSG),
                                       "Attempt to move to object located in file header: %" PRIu64,
                                       offset);

        r = journal_file_move_to(f, type, false, offset, sizeof(ObjectHeader), &t);
        if (r < 0)
                return r;

        o = (Object*) t;
        s = le64toh(READ_NOW(o->object.size));

        if (s == 0)
                return log_debug_errno(SYNTHETIC_ERRNO(EBADMSG),
                                       "Attempt to move to uninitialized object: %" PRIu64,
                                       offset);
        if (s < sizeof(ObjectHeader))
                return log_debug_errno(SYNTHETIC_ERRNO(EBADMSG),
                                       "Attempt to move to overly short object: %" PRIu64,
                                       offset);

        if (o->object.type <= OBJECT_UNUSED)
                return log_debug_errno(SYNTHETIC_ERRNO(EBADMSG),
                                       "Attempt to move to object with invalid type: %" PRIu64,
                                       offset);

        if (s < minimum_header_size(o))
                return log_debug_errno(SYNTHETIC_ERRNO(EBADMSG),
                                       "Attempt to move to truncated object: %" PRIu64,
                                       offset);

        if (type > OBJECT_UNUSED && o->object.type != type)
                return log_debug_errno(SYNTHETIC_ERRNO(EBADMSG),
                                       "Attempt to move to object of unexpected type: %" PRIu64,
                                       offset);

        r = journal_file_move_to(f, type, false, offset, s, &t);
        if (r < 0)
                return r;

        o = (Object*) t;

        r = journal_file_check_object(f, offset, o);
        if (r < 0)
                return r;

        *ret = o;
        return 0;
}

static uint64_t journal_file_entry_seqnum(
                JournalFile *f,
                uint64_t *seqnum) {

        uint64_t ret;

        assert(f);
        assert(f->header);

        /* Picks a new sequence number for the entry we are about to add and returns it. */

        ret = le64toh(f->header->tail_entry_seqnum) + 1;

        if (seqnum) {
                /* If an external seqnum counter was passed, we update both the local and the external one,
                 * and set it to the maximum of both */

                if (*seqnum + 1 > ret)
                        ret = *seqnum + 1;

                *seqnum = ret;
        }

        f->header->tail_entry_seqnum = htole64(ret);

        if (f->header->head_entry_seqnum == 0)
                f->header->head_entry_seqnum = htole64(ret);

        return ret;
}

int journal_file_append_object(
                JournalFile *f,
                ObjectType type,
                uint64_t size,
                Object **ret,
                uint64_t *ret_offset) {

        int r;
        uint64_t p;
        Object *tail, *o;
        void *t;

        assert(f);
        assert(f->header);
        assert(type > OBJECT_UNUSED && type < _OBJECT_TYPE_MAX);
        assert(size >= sizeof(ObjectHeader));

        r = journal_file_set_online(f);
        if (r < 0)
                return r;

        p = le64toh(f->header->tail_object_offset);
        if (p == 0)
                p = le64toh(f->header->header_size);
        else {
                uint64_t sz;

                r = journal_file_move_to_object(f, OBJECT_UNUSED, p, &tail);
                if (r < 0)
                        return r;

                sz = le64toh(READ_NOW(tail->object.size));
                if (sz > UINT64_MAX - sizeof(uint64_t) + 1)
                        return -EBADMSG;

                sz = ALIGN64(sz);
                if (p > UINT64_MAX - sz)
                        return -EBADMSG;

                p += sz;
        }

        r = journal_file_allocate(f, p, size);
        if (r < 0)
                return r;

        r = journal_file_move_to(f, type, false, p, size, &t);
        if (r < 0)
                return r;

        o = (Object*) t;
        o->object = (ObjectHeader) {
                .type = type,
                .size = htole64(size),
        };

        f->header->tail_object_offset = htole64(p);
        f->header->n_objects = htole64(le64toh(f->header->n_objects) + 1);

        if (ret)
                *ret = o;

        if (ret_offset)
                *ret_offset = p;

        return 0;
}

static int journal_file_setup_data_hash_table(JournalFile *f) {
        uint64_t s, p;
        Object *o;
        int r;

        assert(f);
        assert(f->header);

        /* We estimate that we need 1 hash table entry per 768 bytes
           of journal file and we want to make sure we never get
           beyond 75% fill level. Calculate the hash table size for
           the maximum file size based on these metrics. */

        s = (f->metrics.max_size * 4 / 768 / 3) * sizeof(HashItem);
        if (s < DEFAULT_DATA_HASH_TABLE_SIZE)
                s = DEFAULT_DATA_HASH_TABLE_SIZE;

        log_debug("Reserving %"PRIu64" entries in data hash table.", s / sizeof(HashItem));

        r = journal_file_append_object(f,
                                       OBJECT_DATA_HASH_TABLE,
                                       offsetof(Object, hash_table.items) + s,
                                       &o, &p);
        if (r < 0)
                return r;

        memzero(o->hash_table.items, s);

        f->header->data_hash_table_offset = htole64(p + offsetof(Object, hash_table.items));
        f->header->data_hash_table_size = htole64(s);

        return 0;
}

static int journal_file_setup_field_hash_table(JournalFile *f) {
        uint64_t s, p;
        Object *o;
        int r;

        assert(f);
        assert(f->header);

        /* We use a fixed size hash table for the fields as this
         * number should grow very slowly only */

        s = DEFAULT_FIELD_HASH_TABLE_SIZE;
        log_debug("Reserving %"PRIu64" entries in field hash table.", s / sizeof(HashItem));

        r = journal_file_append_object(f,
                                       OBJECT_FIELD_HASH_TABLE,
                                       offsetof(Object, hash_table.items) + s,
                                       &o, &p);
        if (r < 0)
                return r;

        memzero(o->hash_table.items, s);

        f->header->field_hash_table_offset = htole64(p + offsetof(Object, hash_table.items));
        f->header->field_hash_table_size = htole64(s);

        return 0;
}

int journal_file_map_data_hash_table(JournalFile *f) {
        uint64_t s, p;
        void *t;
        int r;

        assert(f);
        assert(f->header);

        if (f->data_hash_table)
                return 0;

        p = le64toh(f->header->data_hash_table_offset);
        s = le64toh(f->header->data_hash_table_size);

        r = journal_file_move_to(f,
                                 OBJECT_DATA_HASH_TABLE,
                                 true,
                                 p, s,
                                 &t);
        if (r < 0)
                return r;

        f->data_hash_table = t;
        return 0;
}

int journal_file_map_field_hash_table(JournalFile *f) {
        uint64_t s, p;
        void *t;
        int r;

        assert(f);
        assert(f->header);

        if (f->field_hash_table)
                return 0;

        p = le64toh(f->header->field_hash_table_offset);
        s = le64toh(f->header->field_hash_table_size);

        r = journal_file_move_to(f,
                                 OBJECT_FIELD_HASH_TABLE,
                                 true,
                                 p, s,
                                 &t);
        if (r < 0)
                return r;

        f->field_hash_table = t;
        return 0;
}

static int journal_file_link_field(
                JournalFile *f,
                Object *o,
                uint64_t offset,
                uint64_t hash) {

        uint64_t p, h, m;
        int r;

        assert(f);
        assert(f->header);
        assert(f->field_hash_table);
        assert(o);
        assert(offset > 0);

        if (o->object.type != OBJECT_FIELD)
                return -EINVAL;

        m = le64toh(READ_NOW(f->header->field_hash_table_size)) / sizeof(HashItem);
        if (m <= 0)
                return -EBADMSG;

        /* This might alter the window we are looking at */
        o->field.next_hash_offset = o->field.head_data_offset = 0;

        h = hash % m;
        p = le64toh(f->field_hash_table[h].tail_hash_offset);
        if (p == 0)
                f->field_hash_table[h].head_hash_offset = htole64(offset);
        else {
                r = journal_file_move_to_object(f, OBJECT_FIELD, p, &o);
                if (r < 0)
                        return r;

                o->field.next_hash_offset = htole64(offset);
        }

        f->field_hash_table[h].tail_hash_offset = htole64(offset);

        if (JOURNAL_HEADER_CONTAINS(f->header, n_fields))
                f->header->n_fields = htole64(le64toh(f->header->n_fields) + 1);

        return 0;
}

static int journal_file_link_data(
                JournalFile *f,
                Object *o,
                uint64_t offset,
                uint64_t hash) {

        uint64_t p, h, m;
        int r;

        assert(f);
        assert(f->header);
        assert(f->data_hash_table);
        assert(o);
        assert(offset > 0);

        if (o->object.type != OBJECT_DATA)
                return -EINVAL;

        m = le64toh(READ_NOW(f->header->data_hash_table_size)) / sizeof(HashItem);
        if (m <= 0)
                return -EBADMSG;

        /* This might alter the window we are looking at */
        o->data.next_hash_offset = o->data.next_field_offset = 0;
        o->data.entry_offset = o->data.entry_array_offset = 0;
        o->data.n_entries = 0;

        h = hash % m;
        p = le64toh(f->data_hash_table[h].tail_hash_offset);
        if (p == 0)
                /* Only entry in the hash table is easy */
                f->data_hash_table[h].head_hash_offset = htole64(offset);
        else {
                /* Move back to the previous data object, to patch in
                 * pointer */

                r = journal_file_move_to_object(f, OBJECT_DATA, p, &o);
                if (r < 0)
                        return r;

                o->data.next_hash_offset = htole64(offset);
        }

        f->data_hash_table[h].tail_hash_offset = htole64(offset);

        if (JOURNAL_HEADER_CONTAINS(f->header, n_data))
                f->header->n_data = htole64(le64toh(f->header->n_data) + 1);

        return 0;
}

static int next_hash_offset(
                JournalFile *f,
                uint64_t *p,
                le64_t *next_hash_offset,
                uint64_t *depth,
                le64_t *header_max_depth) {

        uint64_t nextp;

        nextp = le64toh(READ_NOW(*next_hash_offset));
        if (nextp > 0) {
                if (nextp <= *p) /* Refuse going in loops */
                        return log_debug_errno(SYNTHETIC_ERRNO(EBADMSG),
                                               "Detected hash item loop in %s, refusing.", f->path);

                (*depth)++;

                /* If the depth of this hash chain is larger than all others we have seen so far, record it */
                if (header_max_depth && f->writable)
                        *header_max_depth = htole64(MAX(*depth, le64toh(*header_max_depth)));
        }

        *p = nextp;
        return 0;
}

int journal_file_find_field_object_with_hash(
                JournalFile *f,
                const void *field, uint64_t size, uint64_t hash,
                Object **ret, uint64_t *ret_offset) {

        uint64_t p, osize, h, m, depth = 0;
        int r;

        assert(f);
        assert(f->header);
        assert(field && size > 0);

        /* If the field hash table is empty, we can't find anything */
        if (le64toh(f->header->field_hash_table_size) <= 0)
                return 0;

        /* Map the field hash table, if it isn't mapped yet. */
        r = journal_file_map_field_hash_table(f);
        if (r < 0)
                return r;

        osize = offsetof(Object, field.payload) + size;

        m = le64toh(READ_NOW(f->header->field_hash_table_size)) / sizeof(HashItem);
        if (m <= 0)
                return -EBADMSG;

        h = hash % m;
        p = le64toh(f->field_hash_table[h].head_hash_offset);
        while (p > 0) {
                Object *o;

                r = journal_file_move_to_object(f, OBJECT_FIELD, p, &o);
                if (r < 0)
                        return r;

                if (le64toh(o->field.hash) == hash &&
                    le64toh(o->object.size) == osize &&
                    memcmp(o->field.payload, field, size) == 0) {

                        if (ret)
                                *ret = o;
                        if (ret_offset)
                                *ret_offset = p;

                        return 1;
                }

                r = next_hash_offset(
                                f,
                                &p,
                                &o->field.next_hash_offset,
                                &depth,
                                JOURNAL_HEADER_CONTAINS(f->header, field_hash_chain_depth) ? &f->header->field_hash_chain_depth : NULL);
                if (r < 0)
                        return r;
        }

        return 0;
}

uint64_t journal_file_hash_data(
                JournalFile *f,
                const void *data,
                size_t sz) {

        assert(f);
        assert(data || sz == 0);

        /* We try to unify our codebase on siphash, hence new-styled journal files utilizing the keyed hash
         * function use siphash. Old journal files use the Jenkins hash. */

        if (JOURNAL_HEADER_KEYED_HASH(f->header))
                return siphash24(data, sz, f->header->file_id.bytes);

        return jenkins_hash64(data, sz);
}

int journal_file_find_field_object(
                JournalFile *f,
                const void *field, uint64_t size,
                Object **ret, uint64_t *ret_offset) {

        assert(f);
        assert(field && size > 0);

        return journal_file_find_field_object_with_hash(
                        f,
                        field, size,
                        journal_file_hash_data(f, field, size),
                        ret, ret_offset);
}

int journal_file_find_data_object_with_hash(
                JournalFile *f,
                const void *data, uint64_t size, uint64_t hash,
                Object **ret, uint64_t *ret_offset) {

        uint64_t p, osize, h, m, depth = 0;
        int r;

        assert(f);
        assert(f->header);
        assert(data || size == 0);

        /* If there's no data hash table, then there's no entry. */
        if (le64toh(f->header->data_hash_table_size) <= 0)
                return 0;

        /* Map the data hash table, if it isn't mapped yet. */
        r = journal_file_map_data_hash_table(f);
        if (r < 0)
                return r;

        osize = offsetof(Object, data.payload) + size;

        m = le64toh(READ_NOW(f->header->data_hash_table_size)) / sizeof(HashItem);
        if (m <= 0)
                return -EBADMSG;

        h = hash % m;
        p = le64toh(f->data_hash_table[h].head_hash_offset);

        while (p > 0) {
                Object *o;

                r = journal_file_move_to_object(f, OBJECT_DATA, p, &o);
                if (r < 0)
                        return r;

                if (le64toh(o->data.hash) != hash)
                        goto next;

                if (o->object.flags & OBJECT_COMPRESSION_MASK) {
#if HAVE_COMPRESSION
                        uint64_t l;
                        size_t rsize = 0;

                        l = le64toh(READ_NOW(o->object.size));
                        if (l <= offsetof(Object, data.payload))
                                return -EBADMSG;

                        l -= offsetof(Object, data.payload);

                        r = decompress_blob(o->object.flags & OBJECT_COMPRESSION_MASK,
                                            o->data.payload, l, &f->compress_buffer, &rsize, 0);
                        if (r < 0)
                                return r;

                        if (rsize == size &&
                            memcmp(f->compress_buffer, data, size) == 0) {

                                if (ret)
                                        *ret = o;

                                if (ret_offset)
                                        *ret_offset = p;

                                return 1;
                        }
#else
                        return -EPROTONOSUPPORT;
#endif
                } else if (le64toh(o->object.size) == osize &&
                           memcmp(o->data.payload, data, size) == 0) {

                        if (ret)
                                *ret = o;

                        if (ret_offset)
                                *ret_offset = p;

                        return 1;
                }

        next:
                r = next_hash_offset(
                                f,
                                &p,
                                &o->data.next_hash_offset,
                                &depth,
                                JOURNAL_HEADER_CONTAINS(f->header, data_hash_chain_depth) ? &f->header->data_hash_chain_depth : NULL);
                if (r < 0)
                        return r;
        }

        return 0;
}

int journal_file_find_data_object(
                JournalFile *f,
                const void *data, uint64_t size,
                Object **ret, uint64_t *ret_offset) {

        assert(f);
        assert(data || size == 0);

        return journal_file_find_data_object_with_hash(
                        f,
                        data, size,
                        journal_file_hash_data(f, data, size),
                        ret, ret_offset);
}

bool journal_field_valid(const char *p, size_t l, bool allow_protected) {
        /* We kinda enforce POSIX syntax recommendations for
           environment variables here, but make a couple of additional
           requirements.

           http://pubs.opengroup.org/onlinepubs/000095399/basedefs/xbd_chap08.html */

        if (l == SIZE_MAX)
                l = strlen(p);

        /* No empty field names */
        if (l <= 0)
                return false;

        /* Don't allow names longer than 64 chars */
        if (l > 64)
                return false;

        /* Variables starting with an underscore are protected */
        if (!allow_protected && p[0] == '_')
                return false;

        /* Don't allow digits as first character */
        if (p[0] >= '0' && p[0] <= '9')
                return false;

        /* Only allow A-Z0-9 and '_' */
        for (const char *a = p; a < p + l; a++)
                if ((*a < 'A' || *a > 'Z') &&
                    (*a < '0' || *a > '9') &&
                    *a != '_')
                        return false;

        return true;
}

static int journal_file_append_field(
                JournalFile *f,
                const void *field, uint64_t size,
                Object **ret, uint64_t *ret_offset) {

        uint64_t hash, p;
        uint64_t osize;
        Object *o;
        int r;

        assert(f);
        assert(field && size > 0);

        if (!journal_field_valid(field, size, true))
                return -EBADMSG;

        hash = journal_file_hash_data(f, field, size);

        r = journal_file_find_field_object_with_hash(f, field, size, hash, &o, &p);
        if (r < 0)
                return r;
        if (r > 0) {

                if (ret)
                        *ret = o;

                if (ret_offset)
                        *ret_offset = p;

                return 0;
        }

        osize = offsetof(Object, field.payload) + size;
        r = journal_file_append_object(f, OBJECT_FIELD, osize, &o, &p);
        if (r < 0)
                return r;

        o->field.hash = htole64(hash);
        memcpy(o->field.payload, field, size);

        r = journal_file_link_field(f, o, p, hash);
        if (r < 0)
                return r;

        /* The linking might have altered the window, so let's
         * refresh our pointer */
        r = journal_file_move_to_object(f, OBJECT_FIELD, p, &o);
        if (r < 0)
                return r;

#if HAVE_GCRYPT
        r = journal_file_hmac_put_object(f, OBJECT_FIELD, o, p);
        if (r < 0)
                return r;
#endif

        if (ret)
                *ret = o;

        if (ret_offset)
                *ret_offset = p;

        return 0;
}

static int journal_file_append_data(
                JournalFile *f,
                const void *data, uint64_t size,
                Object **ret, uint64_t *ret_offset) {

        uint64_t hash, p;
        uint64_t osize;
        Object *o;
        int r, compression = 0;
        const void *eq;

        assert(f);
        assert(data || size == 0);

        hash = journal_file_hash_data(f, data, size);

        r = journal_file_find_data_object_with_hash(f, data, size, hash, &o, &p);
        if (r < 0)
                return r;
        if (r > 0) {

                if (ret)
                        *ret = o;

                if (ret_offset)
                        *ret_offset = p;

                return 0;
        }

        osize = offsetof(Object, data.payload) + size;
        r = journal_file_append_object(f, OBJECT_DATA, osize, &o, &p);
        if (r < 0)
                return r;

        o->data.hash = htole64(hash);

#if HAVE_COMPRESSION
        if (JOURNAL_FILE_COMPRESS(f) && size >= f->compress_threshold_bytes) {
                size_t rsize = 0;

                compression = compress_blob(data, size, o->data.payload, size - 1, &rsize);

                if (compression >= 0) {
                        o->object.size = htole64(offsetof(Object, data.payload) + rsize);
                        o->object.flags |= compression;

                        log_debug("Compressed data object %"PRIu64" -> %zu using %s",
                                  size, rsize, object_compressed_to_string(compression));
                } else
                        /* Compression didn't work, we don't really care why, let's continue without compression */
                        compression = 0;
        }
#endif

        if (compression == 0)
                memcpy_safe(o->data.payload, data, size);

        r = journal_file_link_data(f, o, p, hash);
        if (r < 0)
                return r;

#if HAVE_GCRYPT
        r = journal_file_hmac_put_object(f, OBJECT_DATA, o, p);
        if (r < 0)
                return r;
#endif

        /* The linking might have altered the window, so let's
         * refresh our pointer */
        r = journal_file_move_to_object(f, OBJECT_DATA, p, &o);
        if (r < 0)
                return r;

        if (!data)
                eq = NULL;
        else
                eq = memchr(data, '=', size);
        if (eq && eq > data) {
                Object *fo = NULL;
                uint64_t fp;

                /* Create field object ... */
                r = journal_file_append_field(f, data, (uint8_t*) eq - (uint8_t*) data, &fo, &fp);
                if (r < 0)
                        return r;

                /* ... and link it in. */
                o->data.next_field_offset = fo->field.head_data_offset;
                fo->field.head_data_offset = le64toh(p);
        }

        if (ret)
                *ret = o;

        if (ret_offset)
                *ret_offset = p;

        return 0;
}

uint64_t journal_file_entry_n_items(Object *o) {
        uint64_t sz;
        assert(o);

        if (o->object.type != OBJECT_ENTRY)
                return 0;

        sz = le64toh(READ_NOW(o->object.size));
        if (sz < offsetof(Object, entry.items))
                return 0;

        return (sz - offsetof(Object, entry.items)) / sizeof(EntryItem);
}

uint64_t journal_file_entry_array_n_items(Object *o) {
        uint64_t sz;

        assert(o);

        if (o->object.type != OBJECT_ENTRY_ARRAY)
                return 0;

        sz = le64toh(READ_NOW(o->object.size));
        if (sz < offsetof(Object, entry_array.items))
                return 0;

        return (sz - offsetof(Object, entry_array.items)) / sizeof(uint64_t);
}

uint64_t journal_file_hash_table_n_items(Object *o) {
        uint64_t sz;

        assert(o);

        if (!IN_SET(o->object.type, OBJECT_DATA_HASH_TABLE, OBJECT_FIELD_HASH_TABLE))
                return 0;

        sz = le64toh(READ_NOW(o->object.size));
        if (sz < offsetof(Object, hash_table.items))
                return 0;

        return (sz - offsetof(Object, hash_table.items)) / sizeof(HashItem);
}

static int link_entry_into_array(JournalFile *f,
                                 le64_t *first,
                                 le64_t *idx,
                                 uint64_t p) {
        int r;
        uint64_t n = 0, ap = 0, q, i, a, hidx;
        Object *o;

        assert(f);
        assert(f->header);
        assert(first);
        assert(idx);
        assert(p > 0);

        a = le64toh(*first);
        i = hidx = le64toh(READ_NOW(*idx));
        while (a > 0) {

                r = journal_file_move_to_object(f, OBJECT_ENTRY_ARRAY, a, &o);
                if (r < 0)
                        return r;

                n = journal_file_entry_array_n_items(o);
                if (i < n) {
                        o->entry_array.items[i] = htole64(p);
                        *idx = htole64(hidx + 1);
                        return 0;
                }

                i -= n;
                ap = a;
                a = le64toh(o->entry_array.next_entry_array_offset);
        }

        if (hidx > n)
                n = (hidx+1) * 2;
        else
                n = n * 2;

        if (n < 4)
                n = 4;

        r = journal_file_append_object(f, OBJECT_ENTRY_ARRAY,
                                       offsetof(Object, entry_array.items) + n * sizeof(uint64_t),
                                       &o, &q);
        if (r < 0)
                return r;

#if HAVE_GCRYPT
        r = journal_file_hmac_put_object(f, OBJECT_ENTRY_ARRAY, o, q);
        if (r < 0)
                return r;
#endif

        o->entry_array.items[i] = htole64(p);

        if (ap == 0)
                *first = htole64(q);
        else {
                r = journal_file_move_to_object(f, OBJECT_ENTRY_ARRAY, ap, &o);
                if (r < 0)
                        return r;

                o->entry_array.next_entry_array_offset = htole64(q);
        }

        if (JOURNAL_HEADER_CONTAINS(f->header, n_entry_arrays))
                f->header->n_entry_arrays = htole64(le64toh(f->header->n_entry_arrays) + 1);

        *idx = htole64(hidx + 1);

        return 0;
}

static int link_entry_into_array_plus_one(JournalFile *f,
                                          le64_t *extra,
                                          le64_t *first,
                                          le64_t *idx,
                                          uint64_t p) {

        uint64_t hidx;
        int r;

        assert(f);
        assert(extra);
        assert(first);
        assert(idx);
        assert(p > 0);

        hidx = le64toh(READ_NOW(*idx));
        if (hidx == UINT64_MAX)
                return -EBADMSG;
        if (hidx == 0)
                *extra = htole64(p);
        else {
                le64_t i;

                i = htole64(hidx - 1);
                r = link_entry_into_array(f, first, &i, p);
                if (r < 0)
                        return r;
        }

        *idx = htole64(hidx + 1);
        return 0;
}

static int journal_file_link_entry_item(JournalFile *f, Object *o, uint64_t offset, uint64_t i) {
        uint64_t p;
        int r;

        assert(f);
        assert(o);
        assert(offset > 0);

        p = le64toh(o->entry.items[i].object_offset);
        r = journal_file_move_to_object(f, OBJECT_DATA, p, &o);
        if (r < 0)
                return r;

        return link_entry_into_array_plus_one(f,
                                              &o->data.entry_offset,
                                              &o->data.entry_array_offset,
                                              &o->data.n_entries,
                                              offset);
}

static int journal_file_link_entry(JournalFile *f, Object *o, uint64_t offset) {
        uint64_t n;
        int r;

        assert(f);
        assert(f->header);
        assert(o);
        assert(offset > 0);

        if (o->object.type != OBJECT_ENTRY)
                return -EINVAL;

        __sync_synchronize();

        /* Link up the entry itself */
        r = link_entry_into_array(f,
                                  &f->header->entry_array_offset,
                                  &f->header->n_entries,
                                  offset);
        if (r < 0)
                return r;

        /* log_debug("=> %s seqnr=%"PRIu64" n_entries=%"PRIu64, f->path, o->entry.seqnum, f->header->n_entries); */

        if (f->header->head_entry_realtime == 0)
                f->header->head_entry_realtime = o->entry.realtime;

        f->header->tail_entry_realtime = o->entry.realtime;
        f->header->tail_entry_monotonic = o->entry.monotonic;

        /* Link up the items */
        n = journal_file_entry_n_items(o);
        for (uint64_t i = 0; i < n; i++) {
                r = journal_file_link_entry_item(f, o, offset, i);
                if (r < 0)
                        return r;
        }

        return 0;
}

static int journal_file_append_entry_internal(
                JournalFile *f,
                const dual_timestamp *ts,
                const sd_id128_t *boot_id,
                uint64_t xor_hash,
                const EntryItem items[], unsigned n_items,
                uint64_t *seqnum,
                Object **ret, uint64_t *ret_offset) {
        uint64_t np;
        uint64_t osize;
        Object *o;
        int r;

        assert(f);
        assert(f->header);
        assert(items || n_items == 0);
        assert(ts);

        osize = offsetof(Object, entry.items) + (n_items * sizeof(EntryItem));

        r = journal_file_append_object(f, OBJECT_ENTRY, osize, &o, &np);
        if (r < 0)
                return r;

        o->entry.seqnum = htole64(journal_file_entry_seqnum(f, seqnum));
        memcpy_safe(o->entry.items, items, n_items * sizeof(EntryItem));
        o->entry.realtime = htole64(ts->realtime);
        o->entry.monotonic = htole64(ts->monotonic);
        o->entry.xor_hash = htole64(xor_hash);
        if (boot_id)
                f->header->boot_id = *boot_id;
        o->entry.boot_id = f->header->boot_id;

#if HAVE_GCRYPT
        r = journal_file_hmac_put_object(f, OBJECT_ENTRY, o, np);
        if (r < 0)
                return r;
#endif

        r = journal_file_link_entry(f, o, np);
        if (r < 0)
                return r;

        if (ret)
                *ret = o;

        if (ret_offset)
                *ret_offset = np;

        return r;
}

void journal_file_post_change(JournalFile *f) {
        assert(f);

        if (f->fd < 0)
                return;

        /* inotify() does not receive IN_MODIFY events from file
         * accesses done via mmap(). After each access we hence
         * trigger IN_MODIFY by truncating the journal file to its
         * current size which triggers IN_MODIFY. */

        __sync_synchronize();

        if (ftruncate(f->fd, f->last_stat.st_size) < 0)
                log_debug_errno(errno, "Failed to truncate file to its own size: %m");
}

static int post_change_thunk(sd_event_source *timer, uint64_t usec, void *userdata) {
        assert(userdata);

        journal_file_post_change(userdata);

        return 1;
}

static void schedule_post_change(JournalFile *f) {
        sd_event *e;
        int r;

        assert(f);
        assert(f->post_change_timer);

        assert_se(e = sd_event_source_get_event(f->post_change_timer));

        /* If we are aleady going down, post the change immediately. */
        if (IN_SET(sd_event_get_state(e), SD_EVENT_EXITING, SD_EVENT_FINISHED))
                goto fail;

        r = sd_event_source_get_enabled(f->post_change_timer, NULL);
        if (r < 0) {
                log_debug_errno(r, "Failed to get ftruncate timer state: %m");
                goto fail;
        }
        if (r > 0)
                return;

        r = sd_event_source_set_time_relative(f->post_change_timer, f->post_change_timer_period);
        if (r < 0) {
                log_debug_errno(r, "Failed to set time for scheduling ftruncate: %m");
                goto fail;
        }

        r = sd_event_source_set_enabled(f->post_change_timer, SD_EVENT_ONESHOT);
        if (r < 0) {
                log_debug_errno(r, "Failed to enable scheduled ftruncate: %m");
                goto fail;
        }

        return;

fail:
        /* On failure, let's simply post the change immediately. */
        journal_file_post_change(f);
}

/* Enable coalesced change posting in a timer on the provided sd_event instance */
int journal_file_enable_post_change_timer(JournalFile *f, sd_event *e, usec_t t) {
        _cleanup_(sd_event_source_unrefp) sd_event_source *timer = NULL;
        int r;

        assert(f);
        assert_return(!f->post_change_timer, -EINVAL);
        assert(e);
        assert(t);

        r = sd_event_add_time(e, &timer, CLOCK_MONOTONIC, 0, 0, post_change_thunk, f);
        if (r < 0)
                return r;

        r = sd_event_source_set_enabled(timer, SD_EVENT_OFF);
        if (r < 0)
                return r;

        f->post_change_timer = TAKE_PTR(timer);
        f->post_change_timer_period = t;

        return r;
}

static int entry_item_cmp(const EntryItem *a, const EntryItem *b) {
        return CMP(le64toh(a->object_offset), le64toh(b->object_offset));
}

static size_t remove_duplicate_entry_items(EntryItem items[], size_t n) {

        /* This function relies on the items array being sorted. */
        size_t j = 1;

        if (n <= 1)
                return n;

        for (size_t i = 1; i < n; i++)
                if (items[i].object_offset != items[j - 1].object_offset)
                        items[j++] = items[i];

        return j;
}

int journal_file_append_entry(
                JournalFile *f,
                const dual_timestamp *ts,
                const sd_id128_t *boot_id,
                const struct iovec iovec[], unsigned n_iovec,
                uint64_t *seqnum,
                Object **ret, uint64_t *ret_offset) {

        EntryItem *items;
        int r;
        uint64_t xor_hash = 0;
        struct dual_timestamp _ts;

        assert(f);
        assert(f->header);
        assert(iovec || n_iovec == 0);

        if (ts) {
                if (!VALID_REALTIME(ts->realtime))
                        return log_debug_errno(SYNTHETIC_ERRNO(EBADMSG),
                                               "Invalid realtime timestamp %" PRIu64 ", refusing entry.",
                                               ts->realtime);
                if (!VALID_MONOTONIC(ts->monotonic))
                        return log_debug_errno(SYNTHETIC_ERRNO(EBADMSG),
                                               "Invalid monotomic timestamp %" PRIu64 ", refusing entry.",
                                               ts->monotonic);
        } else {
                dual_timestamp_get(&_ts);
                ts = &_ts;
        }

#if HAVE_GCRYPT
        r = journal_file_maybe_append_tag(f, ts->realtime);
        if (r < 0)
                return r;
#endif

        /* alloca() can't take 0, hence let's allocate at least one */
        items = newa(EntryItem, MAX(1u, n_iovec));

        for (unsigned i = 0; i < n_iovec; i++) {
                uint64_t p;
                Object *o;

                r = journal_file_append_data(f, iovec[i].iov_base, iovec[i].iov_len, &o, &p);
                if (r < 0)
                        return r;

                /* When calculating the XOR hash field, we need to take special care if the "keyed-hash"
                 * journal file flag is on. We use the XOR hash field to quickly determine the identity of a
                 * specific record, and give records with otherwise identical position (i.e. match in seqno,
                 * timestamp, …) a stable ordering. But for that we can't have it that the hash of the
                 * objects in each file is different since they are keyed. Hence let's calculate the Jenkins
                 * hash here for that. This also has the benefit that cursors for old and new journal files
                 * are completely identical (they include the XOR hash after all). For classic Jenkins-hash
                 * files things are easier, we can just take the value from the stored record directly. */

                if (JOURNAL_HEADER_KEYED_HASH(f->header))
                        xor_hash ^= jenkins_hash64(iovec[i].iov_base, iovec[i].iov_len);
                else
                        xor_hash ^= le64toh(o->data.hash);

                items[i].object_offset = htole64(p);
                items[i].hash = o->data.hash;
        }

        /* Order by the position on disk, in order to improve seek
         * times for rotating media. */
        typesafe_qsort(items, n_iovec, entry_item_cmp);
        n_iovec = remove_duplicate_entry_items(items, n_iovec);

        r = journal_file_append_entry_internal(f, ts, boot_id, xor_hash, items, n_iovec, seqnum, ret, ret_offset);

        /* If the memory mapping triggered a SIGBUS then we return an
         * IO error and ignore the error code passed down to us, since
         * it is very likely just an effect of a nullified replacement
         * mapping page */

        if (mmap_cache_got_sigbus(f->mmap, f->cache_fd))
                r = -EIO;

        if (f->post_change_timer)
                schedule_post_change(f);
        else
                journal_file_post_change(f);

        return r;
}

typedef struct ChainCacheItem {
        uint64_t first; /* the array at the beginning of the chain */
        uint64_t array; /* the cached array */
        uint64_t begin; /* the first item in the cached array */
        uint64_t total; /* the total number of items in all arrays before this one in the chain */
        uint64_t last_index; /* the last index we looked at, to optimize locality when bisecting */
} ChainCacheItem;

static void chain_cache_put(
                OrderedHashmap *h,
                ChainCacheItem *ci,
                uint64_t first,
                uint64_t array,
                uint64_t begin,
                uint64_t total,
                uint64_t last_index) {

        if (!ci) {
                /* If the chain item to cache for this chain is the
                 * first one it's not worth caching anything */
                if (array == first)
                        return;

                if (ordered_hashmap_size(h) >= CHAIN_CACHE_MAX) {
                        ci = ordered_hashmap_steal_first(h);
                        assert(ci);
                } else {
                        ci = new(ChainCacheItem, 1);
                        if (!ci)
                                return;
                }

                ci->first = first;

                if (ordered_hashmap_put(h, &ci->first, ci) < 0) {
                        free(ci);
                        return;
                }
        } else
                assert(ci->first == first);

        ci->array = array;
        ci->begin = begin;
        ci->total = total;
        ci->last_index = last_index;
}

static int generic_array_get(
                JournalFile *f,
                uint64_t first,
                uint64_t i,
                Object **ret, uint64_t *ret_offset) {

        Object *o;
        uint64_t p = 0, a, t = 0;
        int r;
        ChainCacheItem *ci;

        assert(f);

        a = first;

        /* Try the chain cache first */
        ci = ordered_hashmap_get(f->chain_cache, &first);
        if (ci && i > ci->total) {
                a = ci->array;
                i -= ci->total;
                t = ci->total;
        }

        while (a > 0) {
                uint64_t k;

                r = journal_file_move_to_object(f, OBJECT_ENTRY_ARRAY, a, &o);
                if (r < 0)
                        return r;

                k = journal_file_entry_array_n_items(o);
                if (i < k) {
                        p = le64toh(o->entry_array.items[i]);
                        goto found;
                }

                i -= k;
                t += k;
                a = le64toh(o->entry_array.next_entry_array_offset);
        }

        return 0;

found:
        /* Let's cache this item for the next invocation */
        chain_cache_put(f->chain_cache, ci, first, a, le64toh(o->entry_array.items[0]), t, i);

        r = journal_file_move_to_object(f, OBJECT_ENTRY, p, &o);
        if (r < 0)
                return r;

        if (ret)
                *ret = o;

        if (ret_offset)
                *ret_offset = p;

        return 1;
}

static int generic_array_get_plus_one(
                JournalFile *f,
                uint64_t extra,
                uint64_t first,
                uint64_t i,
                Object **ret, uint64_t *ret_offset) {

        Object *o;

        assert(f);

        if (i == 0) {
                int r;

                r = journal_file_move_to_object(f, OBJECT_ENTRY, extra, &o);
                if (r < 0)
                        return r;

                if (ret)
                        *ret = o;

                if (ret_offset)
                        *ret_offset = extra;

                return 1;
        }

        return generic_array_get(f, first, i-1, ret, ret_offset);
}

enum {
        TEST_FOUND,
        TEST_LEFT,
        TEST_RIGHT
};

static int generic_array_bisect(
                JournalFile *f,
                uint64_t first,
                uint64_t n,
                uint64_t needle,
                int (*test_object)(JournalFile *f, uint64_t p, uint64_t needle),
                direction_t direction,
                Object **ret,
                uint64_t *ret_offset,
                uint64_t *ret_idx) {

        uint64_t a, p, t = 0, i = 0, last_p = 0, last_index = UINT64_MAX;
        bool subtract_one = false;
        Object *o, *array = NULL;
        int r;
        ChainCacheItem *ci;

        assert(f);
        assert(test_object);

        /* Start with the first array in the chain */
        a = first;

        ci = ordered_hashmap_get(f->chain_cache, &first);
        if (ci && n > ci->total && ci->begin != 0) {
                /* Ah, we have iterated this bisection array chain
                 * previously! Let's see if we can skip ahead in the
                 * chain, as far as the last time. But we can't jump
                 * backwards in the chain, so let's check that
                 * first. */

                r = test_object(f, ci->begin, needle);
                if (r < 0)
                        return r;

                if (r == TEST_LEFT) {
                        /* OK, what we are looking for is right of the
                         * begin of this EntryArray, so let's jump
                         * straight to previously cached array in the
                         * chain */

                        a = ci->array;
                        n -= ci->total;
                        t = ci->total;
                        last_index = ci->last_index;
                }
        }

        while (a > 0) {
                uint64_t left, right, k, lp;

                r = journal_file_move_to_object(f, OBJECT_ENTRY_ARRAY, a, &array);
                if (r < 0)
                        return r;

                k = journal_file_entry_array_n_items(array);
                right = MIN(k, n);
                if (right <= 0)
                        return 0;

                i = right - 1;
                lp = p = le64toh(array->entry_array.items[i]);
                if (p <= 0)
                        r = -EBADMSG;
                else
                        r = test_object(f, p, needle);
                if (r == -EBADMSG) {
                        log_debug_errno(r, "Encountered invalid entry while bisecting, cutting algorithm short. (1)");
                        n = i;
                        continue;
                }
                if (r < 0)
                        return r;

                if (r == TEST_FOUND)
                        r = direction == DIRECTION_DOWN ? TEST_RIGHT : TEST_LEFT;

                if (r == TEST_RIGHT) {
                        left = 0;
                        right -= 1;

                        if (last_index != UINT64_MAX) {
                                assert(last_index <= right);

                                /* If we cached the last index we
                                 * looked at, let's try to not to jump
                                 * too wildly around and see if we can
                                 * limit the range to look at early to
                                 * the immediate neighbors of the last
                                 * index we looked at. */

                                if (last_index > 0) {
                                        uint64_t x = last_index - 1;

                                        p = le64toh(array->entry_array.items[x]);
                                        if (p <= 0)
                                                return -EBADMSG;

                                        r = test_object(f, p, needle);
                                        if (r < 0)
                                                return r;

                                        if (r == TEST_FOUND)
                                                r = direction == DIRECTION_DOWN ? TEST_RIGHT : TEST_LEFT;

                                        if (r == TEST_RIGHT)
                                                right = x;
                                        else
                                                left = x + 1;
                                }

                                if (last_index < right) {
                                        uint64_t y = last_index + 1;

                                        p = le64toh(array->entry_array.items[y]);
                                        if (p <= 0)
                                                return -EBADMSG;

                                        r = test_object(f, p, needle);
                                        if (r < 0)
                                                return r;

                                        if (r == TEST_FOUND)
                                                r = direction == DIRECTION_DOWN ? TEST_RIGHT : TEST_LEFT;

                                        if (r == TEST_RIGHT)
                                                right = y;
                                        else
                                                left = y + 1;
                                }
                        }

                        for (;;) {
                                if (left == right) {
                                        if (direction == DIRECTION_UP)
                                                subtract_one = true;

                                        i = left;
                                        goto found;
                                }

                                assert(left < right);
                                i = (left + right) / 2;

                                p = le64toh(array->entry_array.items[i]);
                                if (p <= 0)
                                        r = -EBADMSG;
                                else
                                        r = test_object(f, p, needle);
                                if (r == -EBADMSG) {
                                        log_debug_errno(r, "Encountered invalid entry while bisecting, cutting algorithm short. (2)");
                                        right = n = i;
                                        continue;
                                }
                                if (r < 0)
                                        return r;

                                if (r == TEST_FOUND)
                                        r = direction == DIRECTION_DOWN ? TEST_RIGHT : TEST_LEFT;

                                if (r == TEST_RIGHT)
                                        right = i;
                                else
                                        left = i + 1;
                        }
                }

                if (k >= n) {
                        if (direction == DIRECTION_UP) {
                                i = n;
                                subtract_one = true;
                                goto found;
                        }

                        return 0;
                }

                last_p = lp;

                n -= k;
                t += k;
                last_index = UINT64_MAX;
                a = le64toh(array->entry_array.next_entry_array_offset);
        }

        return 0;

found:
        if (subtract_one && t == 0 && i == 0)
                return 0;

        /* Let's cache this item for the next invocation */
        chain_cache_put(f->chain_cache, ci, first, a, le64toh(array->entry_array.items[0]), t, subtract_one ? (i > 0 ? i-1 : UINT64_MAX) : i);

        if (subtract_one && i == 0)
                p = last_p;
        else if (subtract_one)
                p = le64toh(array->entry_array.items[i-1]);
        else
                p = le64toh(array->entry_array.items[i]);

        r = journal_file_move_to_object(f, OBJECT_ENTRY, p, &o);
        if (r < 0)
                return r;

        if (ret)
                *ret = o;

        if (ret_offset)
                *ret_offset = p;

        if (ret_idx)
                *ret_idx = t + i + (subtract_one ? -1 : 0);

        return 1;
}

static int generic_array_bisect_plus_one(
                JournalFile *f,
                uint64_t extra,
                uint64_t first,
                uint64_t n,
                uint64_t needle,
                int (*test_object)(JournalFile *f, uint64_t p, uint64_t needle),
                direction_t direction,
                Object **ret,
                uint64_t *ret_offset,
                uint64_t *ret_idx) {

        int r;
        bool step_back = false;
        Object *o;

        assert(f);
        assert(test_object);

        if (n <= 0)
                return 0;

        /* This bisects the array in object 'first', but first checks
         * an extra  */
        r = test_object(f, extra, needle);
        if (r < 0)
                return r;

        if (r == TEST_FOUND)
                r = direction == DIRECTION_DOWN ? TEST_RIGHT : TEST_LEFT;

        /* if we are looking with DIRECTION_UP then we need to first
           see if in the actual array there is a matching entry, and
           return the last one of that. But if there isn't any we need
           to return this one. Hence remember this, and return it
           below. */
        if (r == TEST_LEFT)
                step_back = direction == DIRECTION_UP;

        if (r == TEST_RIGHT) {
                if (direction == DIRECTION_DOWN)
                        goto found;
                else
                        return 0;
        }

        r = generic_array_bisect(f, first, n-1, needle, test_object, direction, ret, ret_offset, ret_idx);

        if (r == 0 && step_back)
                goto found;

        if (r > 0 && ret_idx)
                (*ret_idx)++;

        return r;

found:
        r = journal_file_move_to_object(f, OBJECT_ENTRY, extra, &o);
        if (r < 0)
                return r;

        if (ret)
                *ret = o;

        if (ret_offset)
                *ret_offset = extra;

        if (ret_idx)
                *ret_idx = 0;

        return 1;
}

_pure_ static int test_object_offset(JournalFile *f, uint64_t p, uint64_t needle) {
        assert(f);
        assert(p > 0);

        if (p == needle)
                return TEST_FOUND;
        else if (p < needle)
                return TEST_LEFT;
        else
                return TEST_RIGHT;
}

static int test_object_seqnum(JournalFile *f, uint64_t p, uint64_t needle) {
        uint64_t sq;
        Object *o;
        int r;

        assert(f);
        assert(p > 0);

        r = journal_file_move_to_object(f, OBJECT_ENTRY, p, &o);
        if (r < 0)
                return r;

        sq = le64toh(READ_NOW(o->entry.seqnum));
        if (sq == needle)
                return TEST_FOUND;
        else if (sq < needle)
                return TEST_LEFT;
        else
                return TEST_RIGHT;
}

int journal_file_move_to_entry_by_seqnum(
                JournalFile *f,
                uint64_t seqnum,
                direction_t direction,
                Object **ret,
                uint64_t *ret_offset) {
        assert(f);
        assert(f->header);

        return generic_array_bisect(
                        f,
                        le64toh(f->header->entry_array_offset),
                        le64toh(f->header->n_entries),
                        seqnum,
                        test_object_seqnum,
                        direction,
                        ret, ret_offset, NULL);
}

static int test_object_realtime(JournalFile *f, uint64_t p, uint64_t needle) {
        Object *o;
        uint64_t rt;
        int r;

        assert(f);
        assert(p > 0);

        r = journal_file_move_to_object(f, OBJECT_ENTRY, p, &o);
        if (r < 0)
                return r;

        rt = le64toh(READ_NOW(o->entry.realtime));
        if (rt == needle)
                return TEST_FOUND;
        else if (rt < needle)
                return TEST_LEFT;
        else
                return TEST_RIGHT;
}

int journal_file_move_to_entry_by_realtime(
                JournalFile *f,
                uint64_t realtime,
                direction_t direction,
                Object **ret,
                uint64_t *ret_offset) {
        assert(f);
        assert(f->header);

        return generic_array_bisect(
                        f,
                        le64toh(f->header->entry_array_offset),
                        le64toh(f->header->n_entries),
                        realtime,
                        test_object_realtime,
                        direction,
                        ret, ret_offset, NULL);
}

static int test_object_monotonic(JournalFile *f, uint64_t p, uint64_t needle) {
        Object *o;
        uint64_t m;
        int r;

        assert(f);
        assert(p > 0);

        r = journal_file_move_to_object(f, OBJECT_ENTRY, p, &o);
        if (r < 0)
                return r;

        m = le64toh(READ_NOW(o->entry.monotonic));
        if (m == needle)
                return TEST_FOUND;
        else if (m < needle)
                return TEST_LEFT;
        else
                return TEST_RIGHT;
}

static int find_data_object_by_boot_id(
                JournalFile *f,
                sd_id128_t boot_id,
                Object **o,
                uint64_t *b) {

        char t[STRLEN("_BOOT_ID=") + 32 + 1] = "_BOOT_ID=";

        sd_id128_to_string(boot_id, t + 9);
        return journal_file_find_data_object(f, t, sizeof(t) - 1, o, b);
}

int journal_file_move_to_entry_by_monotonic(
                JournalFile *f,
                sd_id128_t boot_id,
                uint64_t monotonic,
                direction_t direction,
                Object **ret,
                uint64_t *ret_offset) {

        Object *o;
        int r;

        assert(f);

        r = find_data_object_by_boot_id(f, boot_id, &o, NULL);
        if (r < 0)
                return r;
        if (r == 0)
                return -ENOENT;

        return generic_array_bisect_plus_one(
                        f,
                        le64toh(o->data.entry_offset),
                        le64toh(o->data.entry_array_offset),
                        le64toh(o->data.n_entries),
                        monotonic,
                        test_object_monotonic,
                        direction,
                        ret, ret_offset, NULL);
}

void journal_file_reset_location(JournalFile *f) {
        f->location_type = LOCATION_HEAD;
        f->current_offset = 0;
        f->current_seqnum = 0;
        f->current_realtime = 0;
        f->current_monotonic = 0;
        zero(f->current_boot_id);
        f->current_xor_hash = 0;
}

void journal_file_save_location(JournalFile *f, Object *o, uint64_t offset) {
        f->location_type = LOCATION_SEEK;
        f->current_offset = offset;
        f->current_seqnum = le64toh(o->entry.seqnum);
        f->current_realtime = le64toh(o->entry.realtime);
        f->current_monotonic = le64toh(o->entry.monotonic);
        f->current_boot_id = o->entry.boot_id;
        f->current_xor_hash = le64toh(o->entry.xor_hash);
}

int journal_file_compare_locations(JournalFile *af, JournalFile *bf) {
        int r;

        assert(af);
        assert(af->header);
        assert(bf);
        assert(bf->header);
        assert(af->location_type == LOCATION_SEEK);
        assert(bf->location_type == LOCATION_SEEK);

        /* If contents, timestamps and seqnum match, these entries are
         * identical. */
        if (sd_id128_equal(af->current_boot_id, bf->current_boot_id) &&
            af->current_monotonic == bf->current_monotonic &&
            af->current_realtime == bf->current_realtime &&
            af->current_xor_hash == bf->current_xor_hash &&
            sd_id128_equal(af->header->seqnum_id, bf->header->seqnum_id) &&
            af->current_seqnum == bf->current_seqnum)
                return 0;

        if (sd_id128_equal(af->header->seqnum_id, bf->header->seqnum_id)) {

                /* If this is from the same seqnum source, compare
                 * seqnums */
                r = CMP(af->current_seqnum, bf->current_seqnum);
                if (r != 0)
                        return r;

                /* Wow! This is weird, different data but the same
                 * seqnums? Something is borked, but let's make the
                 * best of it and compare by time. */
        }

        if (sd_id128_equal(af->current_boot_id, bf->current_boot_id)) {

                /* If the boot id matches, compare monotonic time */
                r = CMP(af->current_monotonic, bf->current_monotonic);
                if (r != 0)
                        return r;
        }

        /* Otherwise, compare UTC time */
        r = CMP(af->current_realtime, bf->current_realtime);
        if (r != 0)
                return r;

        /* Finally, compare by contents */
        return CMP(af->current_xor_hash, bf->current_xor_hash);
}

static int bump_array_index(uint64_t *i, direction_t direction, uint64_t n) {

        /* Increase or decrease the specified index, in the right direction. */

        if (direction == DIRECTION_DOWN) {
                if (*i >= n - 1)
                        return 0;

                (*i) ++;
        } else {
                if (*i <= 0)
                        return 0;

                (*i) --;
        }

        return 1;
}

static bool check_properly_ordered(uint64_t new_offset, uint64_t old_offset, direction_t direction) {

        /* Consider it an error if any of the two offsets is uninitialized */
        if (old_offset == 0 || new_offset == 0)
                return false;

        /* If we go down, the new offset must be larger than the old one. */
        return direction == DIRECTION_DOWN ?
                new_offset > old_offset  :
                new_offset < old_offset;
}

int journal_file_next_entry(
                JournalFile *f,
                uint64_t p,
                direction_t direction,
                Object **ret, uint64_t *ret_offset) {

        uint64_t i, n, ofs;
        int r;

        assert(f);
        assert(f->header);

        n = le64toh(READ_NOW(f->header->n_entries));
        if (n <= 0)
                return 0;

        if (p == 0)
                i = direction == DIRECTION_DOWN ? 0 : n - 1;
        else {
                r = generic_array_bisect(f,
                                         le64toh(f->header->entry_array_offset),
                                         le64toh(f->header->n_entries),
                                         p,
                                         test_object_offset,
                                         DIRECTION_DOWN,
                                         NULL, NULL,
                                         &i);
                if (r <= 0)
                        return r;

                r = bump_array_index(&i, direction, n);
                if (r <= 0)
                        return r;
        }

        /* And jump to it */
        for (;;) {
                r = generic_array_get(f,
                                      le64toh(f->header->entry_array_offset),
                                      i,
                                      ret, &ofs);
                if (r > 0)
                        break;
                if (r != -EBADMSG)
                        return r;

                /* OK, so this entry is borked. Most likely some entry didn't get synced to disk properly, let's see if
                 * the next one might work for us instead. */
                log_debug_errno(r, "Entry item %" PRIu64 " is bad, skipping over it.", i);

                r = bump_array_index(&i, direction, n);
                if (r <= 0)
                        return r;
        }

        /* Ensure our array is properly ordered. */
        if (p > 0 && !check_properly_ordered(ofs, p, direction))
                return log_debug_errno(SYNTHETIC_ERRNO(EBADMSG),
                                       "%s: entry array not properly ordered at entry %" PRIu64,
                                       f->path, i);

        if (ret_offset)
                *ret_offset = ofs;

        return 1;
}

int journal_file_next_entry_for_data(
                JournalFile *f,
                Object *o, uint64_t p,
                uint64_t data_offset,
                direction_t direction,
                Object **ret, uint64_t *ret_offset) {

        uint64_t i, n, ofs;
        Object *d;
        int r;

        assert(f);
        assert(p > 0 || !o);

        r = journal_file_move_to_object(f, OBJECT_DATA, data_offset, &d);
        if (r < 0)
                return r;

        n = le64toh(READ_NOW(d->data.n_entries));
        if (n <= 0)
                return n;

        if (!o)
                i = direction == DIRECTION_DOWN ? 0 : n - 1;
        else {
                if (o->object.type != OBJECT_ENTRY)
                        return -EINVAL;

                r = generic_array_bisect_plus_one(f,
                                                  le64toh(d->data.entry_offset),
                                                  le64toh(d->data.entry_array_offset),
                                                  le64toh(d->data.n_entries),
                                                  p,
                                                  test_object_offset,
                                                  DIRECTION_DOWN,
                                                  NULL, NULL,
                                                  &i);

                if (r <= 0)
                        return r;

                r = bump_array_index(&i, direction, n);
                if (r <= 0)
                        return r;
        }

        for (;;) {
                r = generic_array_get_plus_one(f,
                                               le64toh(d->data.entry_offset),
                                               le64toh(d->data.entry_array_offset),
                                               i,
                                               ret, &ofs);
                if (r > 0)
                        break;
                if (r != -EBADMSG)
                        return r;

                log_debug_errno(r, "Data entry item %" PRIu64 " is bad, skipping over it.", i);

                r = bump_array_index(&i, direction, n);
                if (r <= 0)
                        return r;
        }

        /* Ensure our array is properly ordered. */
        if (p > 0 && check_properly_ordered(ofs, p, direction))
                return log_debug_errno(SYNTHETIC_ERRNO(EBADMSG),
                                       "%s data entry array not properly ordered at entry %" PRIu64,
                                       f->path, i);

        if (ret_offset)
                *ret_offset = ofs;

        return 1;
}

int journal_file_move_to_entry_by_offset_for_data(
                JournalFile *f,
                uint64_t data_offset,
                uint64_t p,
                direction_t direction,
                Object **ret, uint64_t *ret_offset) {

        int r;
        Object *d;

        assert(f);

        r = journal_file_move_to_object(f, OBJECT_DATA, data_offset, &d);
        if (r < 0)
                return r;

        return generic_array_bisect_plus_one(
                        f,
                        le64toh(d->data.entry_offset),
                        le64toh(d->data.entry_array_offset),
                        le64toh(d->data.n_entries),
                        p,
                        test_object_offset,
                        direction,
                        ret, ret_offset, NULL);
}

int journal_file_move_to_entry_by_monotonic_for_data(
                JournalFile *f,
                uint64_t data_offset,
                sd_id128_t boot_id,
                uint64_t monotonic,
                direction_t direction,
                Object **ret, uint64_t *ret_offset) {

        Object *o, *d;
        int r;
        uint64_t b, z;

        assert(f);

        /* First, seek by time */
        r = find_data_object_by_boot_id(f, boot_id, &o, &b);
        if (r < 0)
                return r;
        if (r == 0)
                return -ENOENT;

        r = generic_array_bisect_plus_one(f,
                                          le64toh(o->data.entry_offset),
                                          le64toh(o->data.entry_array_offset),
                                          le64toh(o->data.n_entries),
                                          monotonic,
                                          test_object_monotonic,
                                          direction,
                                          NULL, &z, NULL);
        if (r <= 0)
                return r;

        /* And now, continue seeking until we find an entry that
         * exists in both bisection arrays */

        for (;;) {
                Object *qo;
                uint64_t p, q;

                r = journal_file_move_to_object(f, OBJECT_DATA, data_offset, &d);
                if (r < 0)
                        return r;

                r = generic_array_bisect_plus_one(f,
                                                  le64toh(d->data.entry_offset),
                                                  le64toh(d->data.entry_array_offset),
                                                  le64toh(d->data.n_entries),
                                                  z,
                                                  test_object_offset,
                                                  direction,
                                                  NULL, &p, NULL);
                if (r <= 0)
                        return r;

                r = journal_file_move_to_object(f, OBJECT_DATA, b, &o);
                if (r < 0)
                        return r;

                r = generic_array_bisect_plus_one(f,
                                                  le64toh(o->data.entry_offset),
                                                  le64toh(o->data.entry_array_offset),
                                                  le64toh(o->data.n_entries),
                                                  p,
                                                  test_object_offset,
                                                  direction,
                                                  &qo, &q, NULL);

                if (r <= 0)
                        return r;

                if (p == q) {
                        if (ret)
                                *ret = qo;
                        if (ret_offset)
                                *ret_offset = q;

                        return 1;
                }

                z = q;
        }
}

int journal_file_move_to_entry_by_seqnum_for_data(
                JournalFile *f,
                uint64_t data_offset,
                uint64_t seqnum,
                direction_t direction,
                Object **ret, uint64_t *ret_offset) {

        Object *d;
        int r;

        assert(f);

        r = journal_file_move_to_object(f, OBJECT_DATA, data_offset, &d);
        if (r < 0)
                return r;

        return generic_array_bisect_plus_one(
                        f,
                        le64toh(d->data.entry_offset),
                        le64toh(d->data.entry_array_offset),
                        le64toh(d->data.n_entries),
                        seqnum,
                        test_object_seqnum,
                        direction,
                        ret, ret_offset, NULL);
}

int journal_file_move_to_entry_by_realtime_for_data(
                JournalFile *f,
                uint64_t data_offset,
                uint64_t realtime,
                direction_t direction,
                Object **ret, uint64_t *ret_offset) {

        Object *d;
        int r;

        assert(f);

        r = journal_file_move_to_object(f, OBJECT_DATA, data_offset, &d);
        if (r < 0)
                return r;

        return generic_array_bisect_plus_one(
                        f,
                        le64toh(d->data.entry_offset),
                        le64toh(d->data.entry_array_offset),
                        le64toh(d->data.n_entries),
                        realtime,
                        test_object_realtime,
                        direction,
                        ret, ret_offset, NULL);
}

void journal_file_dump(JournalFile *f) {
        Object *o;
        int r;
        uint64_t p;

        assert(f);
        assert(f->header);

        journal_file_print_header(f);

        p = le64toh(READ_NOW(f->header->header_size));
        while (p != 0) {
                r = journal_file_move_to_object(f, OBJECT_UNUSED, p, &o);
                if (r < 0)
                        goto fail;

                switch (o->object.type) {

                case OBJECT_UNUSED:
                        printf("Type: OBJECT_UNUSED\n");
                        break;

                case OBJECT_DATA:
                        printf("Type: OBJECT_DATA\n");
                        break;

                case OBJECT_FIELD:
                        printf("Type: OBJECT_FIELD\n");
                        break;

                case OBJECT_ENTRY:
                        printf("Type: OBJECT_ENTRY seqnum=%"PRIu64" monotonic=%"PRIu64" realtime=%"PRIu64"\n",
                               le64toh(o->entry.seqnum),
                               le64toh(o->entry.monotonic),
                               le64toh(o->entry.realtime));
                        break;

                case OBJECT_FIELD_HASH_TABLE:
                        printf("Type: OBJECT_FIELD_HASH_TABLE\n");
                        break;

                case OBJECT_DATA_HASH_TABLE:
                        printf("Type: OBJECT_DATA_HASH_TABLE\n");
                        break;

                case OBJECT_ENTRY_ARRAY:
                        printf("Type: OBJECT_ENTRY_ARRAY\n");
                        break;

                case OBJECT_TAG:
                        printf("Type: OBJECT_TAG seqnum=%"PRIu64" epoch=%"PRIu64"\n",
                               le64toh(o->tag.seqnum),
                               le64toh(o->tag.epoch));
                        break;

                default:
                        printf("Type: unknown (%i)\n", o->object.type);
                        break;
                }

                if (o->object.flags & OBJECT_COMPRESSION_MASK)
                        printf("Flags: %s\n",
                               object_compressed_to_string(o->object.flags & OBJECT_COMPRESSION_MASK));

                if (p == le64toh(f->header->tail_object_offset))
                        p = 0;
                else
                        p += ALIGN64(le64toh(o->object.size));
        }

        return;
fail:
        log_error("File corrupt");
}

static const char* format_timestamp_safe(char *buf, size_t l, usec_t t) {
        const char *x;

        x = format_timestamp(buf, l, t);
        if (x)
                return x;
        return " --- ";
}

void journal_file_print_header(JournalFile *f) {
        char a[SD_ID128_STRING_MAX], b[SD_ID128_STRING_MAX], c[SD_ID128_STRING_MAX], d[SD_ID128_STRING_MAX];
        char x[FORMAT_TIMESTAMP_MAX], y[FORMAT_TIMESTAMP_MAX], z[FORMAT_TIMESTAMP_MAX];
        struct stat st;
        char bytes[FORMAT_BYTES_MAX];

        assert(f);
        assert(f->header);

        printf("File path: %s\n"
               "File ID: %s\n"
               "Machine ID: %s\n"
               "Boot ID: %s\n"
               "Sequential number ID: %s\n"
               "State: %s\n"
               "Compatible flags:%s%s\n"
               "Incompatible flags:%s%s%s%s%s\n"
               "Header size: %"PRIu64"\n"
               "Arena size: %"PRIu64"\n"
               "Data hash table size: %"PRIu64"\n"
               "Field hash table size: %"PRIu64"\n"
               "Rotate suggested: %s\n"
               "Head sequential number: %"PRIu64" (%"PRIx64")\n"
               "Tail sequential number: %"PRIu64" (%"PRIx64")\n"
               "Head realtime timestamp: %s (%"PRIx64")\n"
               "Tail realtime timestamp: %s (%"PRIx64")\n"
               "Tail monotonic timestamp: %s (%"PRIx64")\n"
               "Objects: %"PRIu64"\n"
               "Entry objects: %"PRIu64"\n",
               f->path,
               sd_id128_to_string(f->header->file_id, a),
               sd_id128_to_string(f->header->machine_id, b),
               sd_id128_to_string(f->header->boot_id, c),
               sd_id128_to_string(f->header->seqnum_id, d),
               f->header->state == STATE_OFFLINE ? "OFFLINE" :
               f->header->state == STATE_ONLINE ? "ONLINE" :
               f->header->state == STATE_ARCHIVED ? "ARCHIVED" : "UNKNOWN",
               JOURNAL_HEADER_SEALED(f->header) ? " SEALED" : "",
               (le32toh(f->header->compatible_flags) & ~HEADER_COMPATIBLE_ANY) ? " ???" : "",
               JOURNAL_HEADER_COMPRESSED_XZ(f->header) ? " COMPRESSED-XZ" : "",
               JOURNAL_HEADER_COMPRESSED_LZ4(f->header) ? " COMPRESSED-LZ4" : "",
               JOURNAL_HEADER_COMPRESSED_ZSTD(f->header) ? " COMPRESSED-ZSTD" : "",
               JOURNAL_HEADER_KEYED_HASH(f->header) ? " KEYED-HASH" : "",
               (le32toh(f->header->incompatible_flags) & ~HEADER_INCOMPATIBLE_ANY) ? " ???" : "",
               le64toh(f->header->header_size),
               le64toh(f->header->arena_size),
               le64toh(f->header->data_hash_table_size) / sizeof(HashItem),
               le64toh(f->header->field_hash_table_size) / sizeof(HashItem),
               yes_no(journal_file_rotate_suggested(f, 0)),
               le64toh(f->header->head_entry_seqnum), le64toh(f->header->head_entry_seqnum),
               le64toh(f->header->tail_entry_seqnum), le64toh(f->header->tail_entry_seqnum),
               format_timestamp_safe(x, sizeof(x), le64toh(f->header->head_entry_realtime)), le64toh(f->header->head_entry_realtime),
               format_timestamp_safe(y, sizeof(y), le64toh(f->header->tail_entry_realtime)), le64toh(f->header->tail_entry_realtime),
               format_timespan(z, sizeof(z), le64toh(f->header->tail_entry_monotonic), USEC_PER_MSEC), le64toh(f->header->tail_entry_monotonic),
               le64toh(f->header->n_objects),
               le64toh(f->header->n_entries));

        if (JOURNAL_HEADER_CONTAINS(f->header, n_data))
                printf("Data objects: %"PRIu64"\n"
                       "Data hash table fill: %.1f%%\n",
                       le64toh(f->header->n_data),
                       100.0 * (double) le64toh(f->header->n_data) / ((double) (le64toh(f->header->data_hash_table_size) / sizeof(HashItem))));

        if (JOURNAL_HEADER_CONTAINS(f->header, n_fields))
                printf("Field objects: %"PRIu64"\n"
                       "Field hash table fill: %.1f%%\n",
                       le64toh(f->header->n_fields),
                       100.0 * (double) le64toh(f->header->n_fields) / ((double) (le64toh(f->header->field_hash_table_size) / sizeof(HashItem))));

        if (JOURNAL_HEADER_CONTAINS(f->header, n_tags))
                printf("Tag objects: %"PRIu64"\n",
                       le64toh(f->header->n_tags));
        if (JOURNAL_HEADER_CONTAINS(f->header, n_entry_arrays))
                printf("Entry array objects: %"PRIu64"\n",
                       le64toh(f->header->n_entry_arrays));

        if (JOURNAL_HEADER_CONTAINS(f->header, field_hash_chain_depth))
                printf("Deepest field hash chain: %" PRIu64"\n",
                       f->header->field_hash_chain_depth);

        if (JOURNAL_HEADER_CONTAINS(f->header, data_hash_chain_depth))
                printf("Deepest data hash chain: %" PRIu64"\n",
                       f->header->data_hash_chain_depth);

        if (fstat(f->fd, &st) >= 0)
                printf("Disk usage: %s\n", format_bytes(bytes, sizeof(bytes), (uint64_t) st.st_blocks * 512ULL));
}

static int journal_file_warn_btrfs(JournalFile *f) {
        unsigned attrs;
        int r;

        assert(f);

        /* Before we write anything, check if the COW logic is turned
         * off on btrfs. Given our write pattern that is quite
         * unfriendly to COW file systems this should greatly improve
         * performance on COW file systems, such as btrfs, at the
         * expense of data integrity features (which shouldn't be too
         * bad, given that we do our own checksumming). */

        r = fd_is_fs_type(f->fd, BTRFS_SUPER_MAGIC);
        if (r < 0)
                return log_warning_errno(r, "Failed to determine if journal is on btrfs: %m");
        if (!r)
                return 0;

        r = read_attr_fd(f->fd, &attrs);
        if (r < 0)
                return log_warning_errno(r, "Failed to read file attributes: %m");

        if (attrs & FS_NOCOW_FL) {
                log_debug("Detected btrfs file system with copy-on-write disabled, all is good.");
                return 0;
        }

        log_notice("Creating journal file %s on a btrfs file system, and copy-on-write is enabled. "
                   "This is likely to slow down journal access substantially, please consider turning "
                   "off the copy-on-write file attribute on the journal directory, using chattr +C.", f->path);

        return 1;
}

int journal_file_open(
                int fd,
                const char *fname,
                int flags,
                mode_t mode,
                bool compress,
                uint64_t compress_threshold_bytes,
                bool seal,
                JournalMetrics *metrics,
                MMapCache *mmap_cache,
                Set *deferred_closes,
                JournalFile *template,
                JournalFile **ret) {

        bool newly_created = false;
        JournalFile *f;
        void *h;
        int r;

        assert(ret);
        assert(fd >= 0 || fname);

        if (!IN_SET((flags & O_ACCMODE), O_RDONLY, O_RDWR))
                return -EINVAL;

        if (fname && (flags & O_CREAT) && !endswith(fname, ".journal"))
                return -EINVAL;

        f = new(JournalFile, 1);
        if (!f)
                return -ENOMEM;

        *f = (JournalFile) {
                .fd = fd,
                .mode = mode,

                .flags = flags,
                .writable = (flags & O_ACCMODE) != O_RDONLY,

#if HAVE_ZSTD
                .compress_zstd = compress,
#elif HAVE_LZ4
                .compress_lz4 = compress,
#elif HAVE_XZ
                .compress_xz = compress,
#endif
                .compress_threshold_bytes = compress_threshold_bytes == UINT64_MAX ?
                                            DEFAULT_COMPRESS_THRESHOLD :
                                            MAX(MIN_COMPRESS_THRESHOLD, compress_threshold_bytes),
#if HAVE_GCRYPT
                .seal = seal,
#endif
        };

        /* We turn on keyed hashes by default, but provide an environment variable to turn them off, if
         * people really want that */
        r = getenv_bool("SYSTEMD_JOURNAL_KEYED_HASH");
        if (r < 0) {
                if (r != -ENXIO)
                        log_debug_errno(r, "Failed to parse $SYSTEMD_JOURNAL_KEYED_HASH environment variable, ignoring.");
                f->keyed_hash = true;
        } else
                f->keyed_hash = r;

        if (DEBUG_LOGGING) {
                static int last_seal = -1, last_compress = -1, last_keyed_hash = -1;
                static uint64_t last_bytes = UINT64_MAX;
                char bytes[FORMAT_BYTES_MAX];

                if (last_seal != f->seal ||
                    last_keyed_hash != f->keyed_hash ||
                    last_compress != JOURNAL_FILE_COMPRESS(f) ||
                    last_bytes != f->compress_threshold_bytes) {

                        log_debug("Journal effective settings seal=%s keyed_hash=%s compress=%s compress_threshold_bytes=%s",
                                  yes_no(f->seal), yes_no(f->keyed_hash), yes_no(JOURNAL_FILE_COMPRESS(f)),
                                  format_bytes(bytes, sizeof bytes, f->compress_threshold_bytes));
                        last_seal = f->seal;
                        last_keyed_hash = f->keyed_hash;
                        last_compress = JOURNAL_FILE_COMPRESS(f);
                        last_bytes = f->compress_threshold_bytes;
                }
        }

        if (mmap_cache)
                f->mmap = mmap_cache_ref(mmap_cache);
        else {
                f->mmap = mmap_cache_new();
                if (!f->mmap) {
                        r = -ENOMEM;
                        goto fail;
                }
        }

        if (fname) {
                f->path = strdup(fname);
                if (!f->path) {
                        r = -ENOMEM;
                        goto fail;
                }
        } else {
                assert(fd >= 0);

                /* If we don't know the path, fill in something explanatory and vaguely useful */
                if (asprintf(&f->path, "/proc/self/%i", fd) < 0) {
                        r = -ENOMEM;
                        goto fail;
                }
        }

        f->chain_cache = ordered_hashmap_new(&uint64_hash_ops);
        if (!f->chain_cache) {
                r = -ENOMEM;
                goto fail;
        }

        if (f->fd < 0) {
                /* We pass O_NONBLOCK here, so that in case somebody pointed us to some character device node or FIFO
                 * or so, we likely fail quickly than block for long. For regular files O_NONBLOCK has no effect, hence
                 * it doesn't hurt in that case. */

                f->fd = open(f->path, f->flags|O_CLOEXEC|O_NONBLOCK, f->mode);
                if (f->fd < 0) {
                        r = -errno;
                        goto fail;
                }

                /* fds we opened here by us should also be closed by us. */
                f->close_fd = true;

                r = fd_nonblock(f->fd, false);
                if (r < 0)
                        goto fail;
        }

        f->cache_fd = mmap_cache_add_fd(f->mmap, f->fd, prot_from_flags(flags));
        if (!f->cache_fd) {
                r = -ENOMEM;
                goto fail;
        }

        r = journal_file_fstat(f);
        if (r < 0)
                goto fail;

        if (f->last_stat.st_size == 0 && f->writable) {

                (void) journal_file_warn_btrfs(f);

                /* Let's attach the creation time to the journal file, so that the vacuuming code knows the age of this
                 * file even if the file might end up corrupted one day... Ideally we'd just use the creation time many
                 * file systems maintain for each file, but the API to query this is very new, hence let's emulate this
                 * via extended attributes. If extended attributes are not supported we'll just skip this, and rely
                 * solely on mtime/atime/ctime of the file. */
                (void) fd_setcrtime(f->fd, 0);

#if HAVE_GCRYPT
                /* Try to load the FSPRG state, and if we can't, then
                 * just don't do sealing */
                if (f->seal) {
                        r = journal_file_fss_load(f);
                        if (r < 0)
                                f->seal = false;
                }
#endif

                r = journal_file_init_header(f, template);
                if (r < 0)
                        goto fail;

                r = journal_file_fstat(f);
                if (r < 0)
                        goto fail;

                newly_created = true;
        }

        if (f->last_stat.st_size < (off_t) HEADER_SIZE_MIN) {
                r = -ENODATA;
                goto fail;
        }

        r = mmap_cache_get(f->mmap, f->cache_fd, CONTEXT_HEADER, true, 0, PAGE_ALIGN(sizeof(Header)), &f->last_stat, &h);
        if (r == -EINVAL) {
                /* Some file systems (jffs2 or p9fs) don't support mmap() properly (or only read-only
                 * mmap()), and return EINVAL in that case. Let's propagate that as a more recognizable error
                 * code. */
                r = -EAFNOSUPPORT;
                goto fail;
        }
        if (r < 0)
                goto fail;

        f->header = h;

        if (!newly_created) {
                set_clear_with_destructor(deferred_closes, journal_file_close);

                r = journal_file_verify_header(f);
                if (r < 0)
                        goto fail;
        }

#if HAVE_GCRYPT
        if (!newly_created && f->writable) {
                r = journal_file_fss_load(f);
                if (r < 0)
                        goto fail;
        }
#endif

        if (f->writable) {
                if (metrics) {
                        journal_default_metrics(metrics, f->fd);
                        f->metrics = *metrics;
                } else if (template)
                        f->metrics = template->metrics;

                r = journal_file_refresh_header(f);
                if (r < 0)
                        goto fail;
        }

#if HAVE_GCRYPT
        r = journal_file_hmac_setup(f);
        if (r < 0)
                goto fail;
#endif

        if (newly_created) {
                r = journal_file_setup_field_hash_table(f);
                if (r < 0)
                        goto fail;

                r = journal_file_setup_data_hash_table(f);
                if (r < 0)
                        goto fail;

#if HAVE_GCRYPT
                r = journal_file_append_first_tag(f);
                if (r < 0)
                        goto fail;
#endif
        }

        if (mmap_cache_got_sigbus(f->mmap, f->cache_fd)) {
                r = -EIO;
                goto fail;
        }

        if (template && template->post_change_timer) {
                r = journal_file_enable_post_change_timer(
                                f,
                                sd_event_source_get_event(template->post_change_timer),
                                template->post_change_timer_period);

                if (r < 0)
                        goto fail;
        }

        /* The file is opened now successfully, thus we take possession of any passed in fd. */
        f->close_fd = true;

        *ret = f;
        return 0;

fail:
        if (f->cache_fd && mmap_cache_got_sigbus(f->mmap, f->cache_fd))
                r = -EIO;

        (void) journal_file_close(f);

        return r;
}

int journal_file_archive(JournalFile *f) {
        _cleanup_free_ char *p = NULL;

        assert(f);

        if (!f->writable)
                return -EINVAL;

        /* Is this a journal file that was passed to us as fd? If so, we synthesized a path name for it, and we refuse
         * rotation, since we don't know the actual path, and couldn't rename the file hence. */
        if (path_startswith(f->path, "/proc/self/fd"))
                return -EINVAL;

        if (!endswith(f->path, ".journal"))
                return -EINVAL;

        if (asprintf(&p, "%.*s@" SD_ID128_FORMAT_STR "-%016"PRIx64"-%016"PRIx64".journal",
                     (int) strlen(f->path) - 8, f->path,
                     SD_ID128_FORMAT_VAL(f->header->seqnum_id),
                     le64toh(f->header->head_entry_seqnum),
                     le64toh(f->header->head_entry_realtime)) < 0)
                return -ENOMEM;

        /* Try to rename the file to the archived version. If the file already was deleted, we'll get ENOENT, let's
         * ignore that case. */
        if (rename(f->path, p) < 0 && errno != ENOENT)
                return -errno;

        /* Sync the rename to disk */
        (void) fsync_directory_of_file(f->fd);

        /* Set as archive so offlining commits w/state=STATE_ARCHIVED. Previously we would set old_file->header->state
         * to STATE_ARCHIVED directly here, but journal_file_set_offline() short-circuits when state != STATE_ONLINE,
         * which would result in the rotated journal never getting fsync() called before closing.  Now we simply queue
         * the archive state by setting an archive bit, leaving the state as STATE_ONLINE so proper offlining
         * occurs. */
        f->archive = true;

        /* Currently, btrfs is not very good with out write patterns and fragments heavily. Let's defrag our journal
         * files when we archive them */
        f->defrag_on_close = true;

        return 0;
}

JournalFile* journal_initiate_close(
                JournalFile *f,
                Set *deferred_closes) {

        int r;

        assert(f);

        if (deferred_closes) {

                r = set_put(deferred_closes, f);
                if (r < 0)
                        log_debug_errno(r, "Failed to add file to deferred close set, closing immediately.");
                else {
                        (void) journal_file_set_offline(f, false);
                        return NULL;
                }
        }

        return journal_file_close(f);
}

int journal_file_rotate(
                JournalFile **f,
                bool compress,
                uint64_t compress_threshold_bytes,
                bool seal,
                Set *deferred_closes) {

        JournalFile *new_file = NULL;
        int r;

        assert(f);
        assert(*f);

        r = journal_file_archive(*f);
        if (r < 0)
                return r;

        r = journal_file_open(
                        -1,
                        (*f)->path,
                        (*f)->flags,
                        (*f)->mode,
                        compress,
                        compress_threshold_bytes,
                        seal,
                        NULL,            /* metrics */
                        (*f)->mmap,
                        deferred_closes,
                        *f,              /* template */
                        &new_file);

        journal_initiate_close(*f, deferred_closes);
        *f = new_file;

        return r;
}

int journal_file_dispose(int dir_fd, const char *fname) {
        _cleanup_free_ char *p = NULL;
        _cleanup_close_ int fd = -1;

        assert(fname);

        /* Renames a journal file to *.journal~, i.e. to mark it as corrupted or otherwise uncleanly shutdown. Note that
         * this is done without looking into the file or changing any of its contents. The idea is that this is called
         * whenever something is suspicious and we want to move the file away and make clear that it is not accessed
         * for writing anymore. */

        if (!endswith(fname, ".journal"))
                return -EINVAL;

        if (asprintf(&p, "%.*s@%016" PRIx64 "-%016" PRIx64 ".journal~",
                     (int) strlen(fname) - 8, fname,
                     now(CLOCK_REALTIME),
                     random_u64()) < 0)
                return -ENOMEM;

        if (renameat(dir_fd, fname, dir_fd, p) < 0)
                return -errno;

        /* btrfs doesn't cope well with our write pattern and fragments heavily. Let's defrag all files we rotate */
        fd = openat(dir_fd, p, O_RDONLY|O_CLOEXEC|O_NOCTTY|O_NOFOLLOW);
        if (fd < 0)
                log_debug_errno(errno, "Failed to open file for defragmentation/FS_NOCOW_FL, ignoring: %m");
        else {
                (void) chattr_fd(fd, 0, FS_NOCOW_FL, NULL);
                (void) btrfs_defrag_fd(fd);
        }

        return 0;
}

int journal_file_open_reliably(
                const char *fname,
                int flags,
                mode_t mode,
                bool compress,
                uint64_t compress_threshold_bytes,
                bool seal,
                JournalMetrics *metrics,
                MMapCache *mmap_cache,
                Set *deferred_closes,
                JournalFile *template,
                JournalFile **ret) {

        int r;

        r = journal_file_open(-1, fname, flags, mode, compress, compress_threshold_bytes, seal, metrics, mmap_cache,
                              deferred_closes, template, ret);
        if (!IN_SET(r,
                    -EBADMSG,           /* Corrupted */
                    -ENODATA,           /* Truncated */
                    -EHOSTDOWN,         /* Other machine */
                    -EPROTONOSUPPORT,   /* Incompatible feature */
                    -EBUSY,             /* Unclean shutdown */
                    -ESHUTDOWN,         /* Already archived */
                    -EIO,               /* IO error, including SIGBUS on mmap */
                    -EIDRM,             /* File has been deleted */
                    -ETXTBSY))          /* File is from the future */
                return r;

        if ((flags & O_ACCMODE) == O_RDONLY)
                return r;

        if (!(flags & O_CREAT))
                return r;

        if (!endswith(fname, ".journal"))
                return r;

        /* The file is corrupted. Rotate it away and try it again (but only once) */
        log_warning_errno(r, "File %s corrupted or uncleanly shut down, renaming and replacing.", fname);

        r = journal_file_dispose(AT_FDCWD, fname);
        if (r < 0)
                return r;

        return journal_file_open(-1, fname, flags, mode, compress, compress_threshold_bytes, seal, metrics, mmap_cache,
                                 deferred_closes, template, ret);
}

int journal_file_copy_entry(JournalFile *from, JournalFile *to, Object *o, uint64_t p) {
        uint64_t q, n, xor_hash = 0;
        const sd_id128_t *boot_id;
        dual_timestamp ts;
        EntryItem *items;
        int r;

        assert(from);
        assert(to);
        assert(o);
        assert(p);

        if (!to->writable)
                return -EPERM;

        ts.monotonic = le64toh(o->entry.monotonic);
        ts.realtime = le64toh(o->entry.realtime);
        boot_id = &o->entry.boot_id;

        n = journal_file_entry_n_items(o);
        /* alloca() can't take 0, hence let's allocate at least one */
        items = newa(EntryItem, MAX(1u, n));

        for (uint64_t i = 0; i < n; i++) {
                uint64_t l, h;
                le64_t le_hash;
                size_t t;
                void *data;
                Object *u;

                q = le64toh(o->entry.items[i].object_offset);
                le_hash = o->entry.items[i].hash;

                r = journal_file_move_to_object(from, OBJECT_DATA, q, &o);
                if (r < 0)
                        return r;

                if (le_hash != o->data.hash)
                        return -EBADMSG;

                l = le64toh(READ_NOW(o->object.size));
                if (l < offsetof(Object, data.payload))
                        return -EBADMSG;

                l -= offsetof(Object, data.payload);
                t = (size_t) l;

                /* We hit the limit on 32bit machines */
                if ((uint64_t) t != l)
                        return -E2BIG;

                if (o->object.flags & OBJECT_COMPRESSION_MASK) {
#if HAVE_COMPRESSION
                        size_t rsize = 0;

                        r = decompress_blob(
                                        o->object.flags & OBJECT_COMPRESSION_MASK,
                                        o->data.payload, l,
                                        &from->compress_buffer, &rsize,
                                        0);
                        if (r < 0)
                                return r;

                        data = from->compress_buffer;
                        l = rsize;
#else
                        return -EPROTONOSUPPORT;
#endif
                } else
                        data = o->data.payload;

                r = journal_file_append_data(to, data, l, &u, &h);
                if (r < 0)
                        return r;

                if (JOURNAL_HEADER_KEYED_HASH(to->header))
                        xor_hash ^= jenkins_hash64(data, l);
                else
                        xor_hash ^= le64toh(u->data.hash);

                items[i].object_offset = htole64(h);
                items[i].hash = u->data.hash;

                r = journal_file_move_to_object(from, OBJECT_ENTRY, p, &o);
                if (r < 0)
                        return r;
        }

        r = journal_file_append_entry_internal(to, &ts, boot_id, xor_hash, items, n,
                                               NULL, NULL, NULL);

        if (mmap_cache_got_sigbus(to->mmap, to->cache_fd))
                return -EIO;

        return r;
}

void journal_reset_metrics(JournalMetrics *m) {
        assert(m);

        /* Set everything to "pick automatic values". */

        *m = (JournalMetrics) {
                .min_use = UINT64_MAX,
                .max_use = UINT64_MAX,
                .min_size = UINT64_MAX,
                .max_size = UINT64_MAX,
                .keep_free = UINT64_MAX,
                .n_max_files = UINT64_MAX,
        };
}

void journal_default_metrics(JournalMetrics *m, int fd) {
        char a[FORMAT_BYTES_MAX], b[FORMAT_BYTES_MAX], c[FORMAT_BYTES_MAX], d[FORMAT_BYTES_MAX], e[FORMAT_BYTES_MAX];
        struct statvfs ss;
        uint64_t fs_size = 0;

        assert(m);
        assert(fd >= 0);

        if (fstatvfs(fd, &ss) >= 0)
                fs_size = ss.f_frsize * ss.f_blocks;
        else
                log_debug_errno(errno, "Failed to determine disk size: %m");

        if (m->max_use == UINT64_MAX) {

                if (fs_size > 0)
                        m->max_use = CLAMP(PAGE_ALIGN(fs_size / 10), /* 10% of file system size */
                                           MAX_USE_LOWER, MAX_USE_UPPER);
                else
                        m->max_use = MAX_USE_LOWER;
        } else {
                m->max_use = PAGE_ALIGN(m->max_use);

                if (m->max_use != 0 && m->max_use < JOURNAL_FILE_SIZE_MIN*2)
                        m->max_use = JOURNAL_FILE_SIZE_MIN*2;
        }

        if (m->min_use == UINT64_MAX) {
                if (fs_size > 0)
                        m->min_use = CLAMP(PAGE_ALIGN(fs_size / 50), /* 2% of file system size */
                                           MIN_USE_LOW, MIN_USE_HIGH);
                else
                        m->min_use = MIN_USE_LOW;
        }

        if (m->min_use > m->max_use)
                m->min_use = m->max_use;

        if (m->max_size == UINT64_MAX)
                m->max_size = MIN(PAGE_ALIGN(m->max_use / 8), /* 8 chunks */
                                  MAX_SIZE_UPPER);
        else
                m->max_size = PAGE_ALIGN(m->max_size);

        if (m->max_size != 0) {
                if (m->max_size < JOURNAL_FILE_SIZE_MIN)
                        m->max_size = JOURNAL_FILE_SIZE_MIN;

                if (m->max_use != 0 && m->max_size*2 > m->max_use)
                        m->max_use = m->max_size*2;
        }

        if (m->min_size == UINT64_MAX)
                m->min_size = JOURNAL_FILE_SIZE_MIN;
        else
                m->min_size = CLAMP(PAGE_ALIGN(m->min_size),
                                    JOURNAL_FILE_SIZE_MIN,
                                    m->max_size ?: UINT64_MAX);

        if (m->keep_free == UINT64_MAX) {
                if (fs_size > 0)
                        m->keep_free = MIN(PAGE_ALIGN(fs_size / 20), /* 5% of file system size */
                                           KEEP_FREE_UPPER);
                else
                        m->keep_free = DEFAULT_KEEP_FREE;
        }

        if (m->n_max_files == UINT64_MAX)
                m->n_max_files = DEFAULT_N_MAX_FILES;

        log_debug("Fixed min_use=%s max_use=%s max_size=%s min_size=%s keep_free=%s n_max_files=%" PRIu64,
                  format_bytes(a, sizeof(a), m->min_use),
                  format_bytes(b, sizeof(b), m->max_use),
                  format_bytes(c, sizeof(c), m->max_size),
                  format_bytes(d, sizeof(d), m->min_size),
                  format_bytes(e, sizeof(e), m->keep_free),
                  m->n_max_files);
}

int journal_file_get_cutoff_realtime_usec(JournalFile *f, usec_t *from, usec_t *to) {
        assert(f);
        assert(f->header);
        assert(from || to);

        if (from) {
                if (f->header->head_entry_realtime == 0)
                        return -ENOENT;

                *from = le64toh(f->header->head_entry_realtime);
        }

        if (to) {
                if (f->header->tail_entry_realtime == 0)
                        return -ENOENT;

                *to = le64toh(f->header->tail_entry_realtime);
        }

        return 1;
}

int journal_file_get_cutoff_monotonic_usec(JournalFile *f, sd_id128_t boot_id, usec_t *from, usec_t *to) {
        Object *o;
        uint64_t p;
        int r;

        assert(f);
        assert(from || to);

        r = find_data_object_by_boot_id(f, boot_id, &o, &p);
        if (r <= 0)
                return r;

        if (le64toh(o->data.n_entries) <= 0)
                return 0;

        if (from) {
                r = journal_file_move_to_object(f, OBJECT_ENTRY, le64toh(o->data.entry_offset), &o);
                if (r < 0)
                        return r;

                *from = le64toh(o->entry.monotonic);
        }

        if (to) {
                r = journal_file_move_to_object(f, OBJECT_DATA, p, &o);
                if (r < 0)
                        return r;

                r = generic_array_get_plus_one(f,
                                               le64toh(o->data.entry_offset),
                                               le64toh(o->data.entry_array_offset),
                                               le64toh(o->data.n_entries)-1,
                                               &o, NULL);
                if (r <= 0)
                        return r;

                *to = le64toh(o->entry.monotonic);
        }

        return 1;
}

bool journal_file_rotate_suggested(JournalFile *f, usec_t max_file_usec) {
        assert(f);
        assert(f->header);

        /* If we gained new header fields we gained new features,
         * hence suggest a rotation */
        if (le64toh(f->header->header_size) < sizeof(Header)) {
                log_debug("%s uses an outdated header, suggesting rotation.", f->path);
                return true;
        }

        /* Let's check if the hash tables grew over a certain fill level (75%, borrowing this value from
         * Java's hash table implementation), and if so suggest a rotation. To calculate the fill level we
         * need the n_data field, which only exists in newer versions. */

        if (JOURNAL_HEADER_CONTAINS(f->header, n_data))
                if (le64toh(f->header->n_data) * 4ULL > (le64toh(f->header->data_hash_table_size) / sizeof(HashItem)) * 3ULL) {
                        log_debug("Data hash table of %s has a fill level at %.1f (%"PRIu64" of %"PRIu64" items, %llu file size, %"PRIu64" bytes per hash table item), suggesting rotation.",
                                  f->path,
                                  100.0 * (double) le64toh(f->header->n_data) / ((double) (le64toh(f->header->data_hash_table_size) / sizeof(HashItem))),
                                  le64toh(f->header->n_data),
                                  le64toh(f->header->data_hash_table_size) / sizeof(HashItem),
                                  (unsigned long long) f->last_stat.st_size,
                                  f->last_stat.st_size / le64toh(f->header->n_data));
                        return true;
                }

        if (JOURNAL_HEADER_CONTAINS(f->header, n_fields))
                if (le64toh(f->header->n_fields) * 4ULL > (le64toh(f->header->field_hash_table_size) / sizeof(HashItem)) * 3ULL) {
                        log_debug("Field hash table of %s has a fill level at %.1f (%"PRIu64" of %"PRIu64" items), suggesting rotation.",
                                  f->path,
                                  100.0 * (double) le64toh(f->header->n_fields) / ((double) (le64toh(f->header->field_hash_table_size) / sizeof(HashItem))),
                                  le64toh(f->header->n_fields),
                                  le64toh(f->header->field_hash_table_size) / sizeof(HashItem));
                        return true;
                }

        /* If there are too many hash collisions somebody is most likely playing games with us. Hence, if our
         * longest chain is longer than some threshold, let's suggest rotation. */
        if (JOURNAL_HEADER_CONTAINS(f->header, data_hash_chain_depth) &&
            le64toh(f->header->data_hash_chain_depth) > HASH_CHAIN_DEPTH_MAX) {
                log_debug("Data hash table of %s has deepest hash chain of length %" PRIu64 ", suggesting rotation.",
                          f->path, le64toh(f->header->data_hash_chain_depth));
                return true;
        }

        if (JOURNAL_HEADER_CONTAINS(f->header, field_hash_chain_depth) &&
            le64toh(f->header->field_hash_chain_depth) > HASH_CHAIN_DEPTH_MAX) {
                log_debug("Field hash table of %s has deepest hash chain of length at %" PRIu64 ", suggesting rotation.",
                          f->path, le64toh(f->header->field_hash_chain_depth));
                return true;
        }

        /* Are the data objects properly indexed by field objects? */
        if (JOURNAL_HEADER_CONTAINS(f->header, n_data) &&
            JOURNAL_HEADER_CONTAINS(f->header, n_fields) &&
            le64toh(f->header->n_data) > 0 &&
            le64toh(f->header->n_fields) == 0)
                return true;

        if (max_file_usec > 0) {
                usec_t t, h;

                h = le64toh(f->header->head_entry_realtime);
                t = now(CLOCK_REALTIME);

                if (h > 0 && t > h + max_file_usec)
                        return true;
        }

        return false;
}
