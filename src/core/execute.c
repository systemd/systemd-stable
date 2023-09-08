/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/personality.h>
#include <sys/prctl.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <utmpx.h>

#if HAVE_PAM
#include <security/pam_appl.h>
#endif

#if HAVE_SELINUX
#include <selinux/selinux.h>
#endif

#if HAVE_SECCOMP
#include <seccomp.h>
#endif

#if HAVE_APPARMOR
#include <sys/apparmor.h>
#endif

#include "sd-messages.h"

#include "acl-util.h"
#include "af-list.h"
#include "alloc-util.h"
#if HAVE_APPARMOR
#include "apparmor-util.h"
#endif
#include "async.h"
#include "barrier.h"
#include "bpf-lsm.h"
#include "cap-list.h"
#include "capability-util.h"
#include "cgroup-setup.h"
#include "chase-symlinks.h"
#include "chown-recursive.h"
#include "cpu-set-util.h"
#include "creds-util.h"
#include "data-fd-util.h"
#include "def.h"
#include "env-file.h"
#include "env-util.h"
#include "errno-list.h"
#include "escape.h"
#include "execute.h"
#include "exit-status.h"
#include "fd-util.h"
#include "fileio.h"
#include "format-util.h"
#include "glob-util.h"
#include "hexdecoct.h"
#include "io-util.h"
#include "ioprio-util.h"
#include "label.h"
#include "log.h"
#include "macro.h"
#include "manager.h"
#include "manager-dump.h"
#include "memory-util.h"
#include "missing_fs.h"
#include "missing_ioprio.h"
#include "mkdir-label.h"
#include "mount-util.h"
#include "mountpoint-util.h"
#include "namespace.h"
#include "parse-util.h"
#include "path-util.h"
#include "process-util.h"
#include "random-util.h"
#include "recurse-dir.h"
#include "rlimit-util.h"
#include "rm-rf.h"
#if HAVE_SECCOMP
#include "seccomp-util.h"
#endif
#include "securebits-util.h"
#include "selinux-util.h"
#include "signal-util.h"
#include "smack-util.h"
#include "socket-util.h"
#include "sort-util.h"
#include "special.h"
#include "stat-util.h"
#include "string-table.h"
#include "string-util.h"
#include "strv.h"
#include "syslog-util.h"
#include "terminal-util.h"
#include "tmpfile-util.h"
#include "umask-util.h"
#include "unit-serialize.h"
#include "user-util.h"
#include "utmp-wtmp.h"

#define IDLE_TIMEOUT_USEC (5*USEC_PER_SEC)
#define IDLE_TIMEOUT2_USEC (1*USEC_PER_SEC)

#define SNDBUF_SIZE (8*1024*1024)

static int shift_fds(int fds[], size_t n_fds) {
        if (n_fds <= 0)
                return 0;

        /* Modifies the fds array! (sorts it) */

        assert(fds);

        for (int start = 0;;) {
                int restart_from = -1;

                for (int i = start; i < (int) n_fds; i++) {
                        int nfd;

                        /* Already at right index? */
                        if (fds[i] == i+3)
                                continue;

                        nfd = fcntl(fds[i], F_DUPFD, i + 3);
                        if (nfd < 0)
                                return -errno;

                        safe_close(fds[i]);
                        fds[i] = nfd;

                        /* Hmm, the fd we wanted isn't free? Then
                         * let's remember that and try again from here */
                        if (nfd != i+3 && restart_from < 0)
                                restart_from = i;
                }

                if (restart_from < 0)
                        break;

                start = restart_from;
        }

        return 0;
}

static int flags_fds(const int fds[], size_t n_socket_fds, size_t n_storage_fds, bool nonblock) {
        size_t n_fds;
        int r;

        n_fds = n_socket_fds + n_storage_fds;
        if (n_fds <= 0)
                return 0;

        assert(fds);

        /* Drops/Sets O_NONBLOCK and FD_CLOEXEC from the file flags.
         * O_NONBLOCK only applies to socket activation though. */

        for (size_t i = 0; i < n_fds; i++) {

                if (i < n_socket_fds) {
                        r = fd_nonblock(fds[i], nonblock);
                        if (r < 0)
                                return r;
                }

                /* We unconditionally drop FD_CLOEXEC from the fds,
                 * since after all we want to pass these fds to our
                 * children */

                r = fd_cloexec(fds[i], false);
                if (r < 0)
                        return r;
        }

        return 0;
}

static const char *exec_context_tty_path(const ExecContext *context) {
        assert(context);

        if (context->stdio_as_fds)
                return NULL;

        if (context->tty_path)
                return context->tty_path;

        return "/dev/console";
}

static void exec_context_tty_reset(const ExecContext *context, const ExecParameters *p) {
        const char *path;

        assert(context);

        path = exec_context_tty_path(context);

        if (context->tty_vhangup) {
                if (p && p->stdin_fd >= 0)
                        (void) terminal_vhangup_fd(p->stdin_fd);
                else if (path)
                        (void) terminal_vhangup(path);
        }

        if (context->tty_reset) {
                if (p && p->stdin_fd >= 0)
                        (void) reset_terminal_fd(p->stdin_fd, true);
                else if (path)
                        (void) reset_terminal(path);
        }

        if (p && p->stdin_fd >= 0)
                (void) terminal_set_size_fd(p->stdin_fd, path, context->tty_rows, context->tty_cols);

        if (context->tty_vt_disallocate && path)
                (void) vt_disallocate(path);
}

static bool is_terminal_input(ExecInput i) {
        return IN_SET(i,
                      EXEC_INPUT_TTY,
                      EXEC_INPUT_TTY_FORCE,
                      EXEC_INPUT_TTY_FAIL);
}

static bool is_terminal_output(ExecOutput o) {
        return IN_SET(o,
                      EXEC_OUTPUT_TTY,
                      EXEC_OUTPUT_KMSG_AND_CONSOLE,
                      EXEC_OUTPUT_JOURNAL_AND_CONSOLE);
}

static bool is_kmsg_output(ExecOutput o) {
        return IN_SET(o,
                      EXEC_OUTPUT_KMSG,
                      EXEC_OUTPUT_KMSG_AND_CONSOLE);
}

static bool exec_context_needs_term(const ExecContext *c) {
        assert(c);

        /* Return true if the execution context suggests we should set $TERM to something useful. */

        if (is_terminal_input(c->std_input))
                return true;

        if (is_terminal_output(c->std_output))
                return true;

        if (is_terminal_output(c->std_error))
                return true;

        return !!c->tty_path;
}

static int open_null_as(int flags, int nfd) {
        int fd;

        assert(nfd >= 0);

        fd = open("/dev/null", flags|O_NOCTTY);
        if (fd < 0)
                return -errno;

        return move_fd(fd, nfd, false);
}

static int connect_journal_socket(
                int fd,
                const char *log_namespace,
                uid_t uid,
                gid_t gid) {

        uid_t olduid = UID_INVALID;
        gid_t oldgid = GID_INVALID;
        const char *j;
        int r;

        j = log_namespace ?
                strjoina("/run/systemd/journal.", log_namespace, "/stdout") :
                "/run/systemd/journal/stdout";

        if (gid_is_valid(gid)) {
                oldgid = getgid();

                if (setegid(gid) < 0)
                        return -errno;
        }

        if (uid_is_valid(uid)) {
                olduid = getuid();

                if (seteuid(uid) < 0) {
                        r = -errno;
                        goto restore_gid;
                }
        }

        r = connect_unix_path(fd, AT_FDCWD, j);

        /* If we fail to restore the uid or gid, things will likely fail later on. This should only happen if
           an LSM interferes. */

        if (uid_is_valid(uid))
                (void) seteuid(olduid);

 restore_gid:
        if (gid_is_valid(gid))
                (void) setegid(oldgid);

        return r;
}

static int connect_logger_as(
                const Unit *unit,
                const ExecContext *context,
                const ExecParameters *params,
                ExecOutput output,
                const char *ident,
                int nfd,
                uid_t uid,
                gid_t gid) {

        _cleanup_close_ int fd = -1;
        int r;

        assert(context);
        assert(params);
        assert(output < _EXEC_OUTPUT_MAX);
        assert(ident);
        assert(nfd >= 0);

        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
                return -errno;

        r = connect_journal_socket(fd, context->log_namespace, uid, gid);
        if (r < 0)
                return r;

        if (shutdown(fd, SHUT_RD) < 0)
                return -errno;

        (void) fd_inc_sndbuf(fd, SNDBUF_SIZE);

        if (dprintf(fd,
                "%s\n"
                "%s\n"
                "%i\n"
                "%i\n"
                "%i\n"
                "%i\n"
                "%i\n",
                context->syslog_identifier ?: ident,
                params->flags & EXEC_PASS_LOG_UNIT ? unit->id : "",
                context->syslog_priority,
                !!context->syslog_level_prefix,
                false,
                is_kmsg_output(output),
                is_terminal_output(output)) < 0)
                return -errno;

        return move_fd(TAKE_FD(fd), nfd, false);
}

static int open_terminal_as(const char *path, int flags, int nfd) {
        int fd;

        assert(path);
        assert(nfd >= 0);

        fd = open_terminal(path, flags | O_NOCTTY);
        if (fd < 0)
                return fd;

        return move_fd(fd, nfd, false);
}

static int acquire_path(const char *path, int flags, mode_t mode) {
        _cleanup_close_ int fd = -1;
        int r;

        assert(path);

        if (IN_SET(flags & O_ACCMODE, O_WRONLY, O_RDWR))
                flags |= O_CREAT;

        fd = open(path, flags|O_NOCTTY, mode);
        if (fd >= 0)
                return TAKE_FD(fd);

        if (errno != ENXIO) /* ENXIO is returned when we try to open() an AF_UNIX file system socket on Linux */
                return -errno;

        /* So, it appears the specified path could be an AF_UNIX socket. Let's see if we can connect to it. */

        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
                return -errno;

        r = connect_unix_path(fd, AT_FDCWD, path);
        if (IN_SET(r, -ENOTSOCK, -EINVAL))
                /* Propagate initial error if we get ENOTSOCK or EINVAL, i.e. we have indication that this
                 * wasn't an AF_UNIX socket after all */
                return -ENXIO;
        if (r < 0)
                return r;

        if ((flags & O_ACCMODE) == O_RDONLY)
                r = shutdown(fd, SHUT_WR);
        else if ((flags & O_ACCMODE) == O_WRONLY)
                r = shutdown(fd, SHUT_RD);
        else
                r = 0;
        if (r < 0)
                return -errno;

        return TAKE_FD(fd);
}

static int fixup_input(
                const ExecContext *context,
                int socket_fd,
                bool apply_tty_stdin) {

        ExecInput std_input;

        assert(context);

        std_input = context->std_input;

        if (is_terminal_input(std_input) && !apply_tty_stdin)
                return EXEC_INPUT_NULL;

        if (std_input == EXEC_INPUT_SOCKET && socket_fd < 0)
                return EXEC_INPUT_NULL;

        if (std_input == EXEC_INPUT_DATA && context->stdin_data_size == 0)
                return EXEC_INPUT_NULL;

        return std_input;
}

static int fixup_output(ExecOutput output, int socket_fd) {

        if (output == EXEC_OUTPUT_SOCKET && socket_fd < 0)
                return EXEC_OUTPUT_INHERIT;

        return output;
}

static int setup_input(
                const ExecContext *context,
                const ExecParameters *params,
                int socket_fd,
                const int named_iofds[static 3]) {

        ExecInput i;
        int r;

        assert(context);
        assert(params);
        assert(named_iofds);

        if (params->stdin_fd >= 0) {
                if (dup2(params->stdin_fd, STDIN_FILENO) < 0)
                        return -errno;

                /* Try to make this the controlling tty, if it is a tty, and reset it */
                if (isatty(STDIN_FILENO)) {
                        (void) ioctl(STDIN_FILENO, TIOCSCTTY, context->std_input == EXEC_INPUT_TTY_FORCE);
                        (void) reset_terminal_fd(STDIN_FILENO, true);
                        (void) terminal_set_size_fd(STDIN_FILENO, NULL, context->tty_rows, context->tty_cols);
                }

                return STDIN_FILENO;
        }

        i = fixup_input(context, socket_fd, params->flags & EXEC_APPLY_TTY_STDIN);

        switch (i) {

        case EXEC_INPUT_NULL:
                return open_null_as(O_RDONLY, STDIN_FILENO);

        case EXEC_INPUT_TTY:
        case EXEC_INPUT_TTY_FORCE:
        case EXEC_INPUT_TTY_FAIL: {
                int fd;

                fd = acquire_terminal(exec_context_tty_path(context),
                                      i == EXEC_INPUT_TTY_FAIL  ? ACQUIRE_TERMINAL_TRY :
                                      i == EXEC_INPUT_TTY_FORCE ? ACQUIRE_TERMINAL_FORCE :
                                                                  ACQUIRE_TERMINAL_WAIT,
                                      USEC_INFINITY);
                if (fd < 0)
                        return fd;

                r = terminal_set_size_fd(fd, exec_context_tty_path(context), context->tty_rows, context->tty_cols);
                if (r < 0)
                        return r;

                return move_fd(fd, STDIN_FILENO, false);
        }

        case EXEC_INPUT_SOCKET:
                assert(socket_fd >= 0);

                return RET_NERRNO(dup2(socket_fd, STDIN_FILENO));

        case EXEC_INPUT_NAMED_FD:
                assert(named_iofds[STDIN_FILENO] >= 0);

                (void) fd_nonblock(named_iofds[STDIN_FILENO], false);
                return RET_NERRNO(dup2(named_iofds[STDIN_FILENO], STDIN_FILENO));

        case EXEC_INPUT_DATA: {
                int fd;

                fd = acquire_data_fd(context->stdin_data, context->stdin_data_size, 0);
                if (fd < 0)
                        return fd;

                return move_fd(fd, STDIN_FILENO, false);
        }

        case EXEC_INPUT_FILE: {
                bool rw;
                int fd;

                assert(context->stdio_file[STDIN_FILENO]);

                rw = (context->std_output == EXEC_OUTPUT_FILE && streq_ptr(context->stdio_file[STDIN_FILENO], context->stdio_file[STDOUT_FILENO])) ||
                        (context->std_error == EXEC_OUTPUT_FILE && streq_ptr(context->stdio_file[STDIN_FILENO], context->stdio_file[STDERR_FILENO]));

                fd = acquire_path(context->stdio_file[STDIN_FILENO], rw ? O_RDWR : O_RDONLY, 0666 & ~context->umask);
                if (fd < 0)
                        return fd;

                return move_fd(fd, STDIN_FILENO, false);
        }

        default:
                assert_not_reached();
        }
}

static bool can_inherit_stderr_from_stdout(
                const ExecContext *context,
                ExecOutput o,
                ExecOutput e) {

        assert(context);

        /* Returns true, if given the specified STDERR and STDOUT output we can directly dup() the stdout fd to the
         * stderr fd */

        if (e == EXEC_OUTPUT_INHERIT)
                return true;
        if (e != o)
                return false;

        if (e == EXEC_OUTPUT_NAMED_FD)
                return streq_ptr(context->stdio_fdname[STDOUT_FILENO], context->stdio_fdname[STDERR_FILENO]);

        if (IN_SET(e, EXEC_OUTPUT_FILE, EXEC_OUTPUT_FILE_APPEND, EXEC_OUTPUT_FILE_TRUNCATE))
                return streq_ptr(context->stdio_file[STDOUT_FILENO], context->stdio_file[STDERR_FILENO]);

        return true;
}

static int setup_output(
                const Unit *unit,
                const ExecContext *context,
                const ExecParameters *params,
                int fileno,
                int socket_fd,
                const int named_iofds[static 3],
                const char *ident,
                uid_t uid,
                gid_t gid,
                dev_t *journal_stream_dev,
                ino_t *journal_stream_ino) {

        ExecOutput o;
        ExecInput i;
        int r;

        assert(unit);
        assert(context);
        assert(params);
        assert(ident);
        assert(journal_stream_dev);
        assert(journal_stream_ino);

        if (fileno == STDOUT_FILENO && params->stdout_fd >= 0) {

                if (dup2(params->stdout_fd, STDOUT_FILENO) < 0)
                        return -errno;

                return STDOUT_FILENO;
        }

        if (fileno == STDERR_FILENO && params->stderr_fd >= 0) {
                if (dup2(params->stderr_fd, STDERR_FILENO) < 0)
                        return -errno;

                return STDERR_FILENO;
        }

        i = fixup_input(context, socket_fd, params->flags & EXEC_APPLY_TTY_STDIN);
        o = fixup_output(context->std_output, socket_fd);

        if (fileno == STDERR_FILENO) {
                ExecOutput e;
                e = fixup_output(context->std_error, socket_fd);

                /* This expects the input and output are already set up */

                /* Don't change the stderr file descriptor if we inherit all
                 * the way and are not on a tty */
                if (e == EXEC_OUTPUT_INHERIT &&
                    o == EXEC_OUTPUT_INHERIT &&
                    i == EXEC_INPUT_NULL &&
                    !is_terminal_input(context->std_input) &&
                    getppid() != 1)
                        return fileno;

                /* Duplicate from stdout if possible */
                if (can_inherit_stderr_from_stdout(context, o, e))
                        return RET_NERRNO(dup2(STDOUT_FILENO, fileno));

                o = e;

        } else if (o == EXEC_OUTPUT_INHERIT) {
                /* If input got downgraded, inherit the original value */
                if (i == EXEC_INPUT_NULL && is_terminal_input(context->std_input))
                        return open_terminal_as(exec_context_tty_path(context), O_WRONLY, fileno);

                /* If the input is connected to anything that's not a /dev/null or a data fd, inherit that... */
                if (!IN_SET(i, EXEC_INPUT_NULL, EXEC_INPUT_DATA))
                        return RET_NERRNO(dup2(STDIN_FILENO, fileno));

                /* If we are not started from PID 1 we just inherit STDOUT from our parent process. */
                if (getppid() != 1)
                        return fileno;

                /* We need to open /dev/null here anew, to get the right access mode. */
                return open_null_as(O_WRONLY, fileno);
        }

        switch (o) {

        case EXEC_OUTPUT_NULL:
                return open_null_as(O_WRONLY, fileno);

        case EXEC_OUTPUT_TTY:
                if (is_terminal_input(i))
                        return RET_NERRNO(dup2(STDIN_FILENO, fileno));

                /* We don't reset the terminal if this is just about output */
                return open_terminal_as(exec_context_tty_path(context), O_WRONLY, fileno);

        case EXEC_OUTPUT_KMSG:
        case EXEC_OUTPUT_KMSG_AND_CONSOLE:
        case EXEC_OUTPUT_JOURNAL:
        case EXEC_OUTPUT_JOURNAL_AND_CONSOLE:
                r = connect_logger_as(unit, context, params, o, ident, fileno, uid, gid);
                if (r < 0) {
                        log_unit_warning_errno(unit, r, "Failed to connect %s to the journal socket, ignoring: %m",
                                               fileno == STDOUT_FILENO ? "stdout" : "stderr");
                        r = open_null_as(O_WRONLY, fileno);
                } else {
                        struct stat st;

                        /* If we connected this fd to the journal via a stream, patch the device/inode into the passed
                         * parameters, but only then. This is useful so that we can set $JOURNAL_STREAM that permits
                         * services to detect whether they are connected to the journal or not.
                         *
                         * If both stdout and stderr are connected to a stream then let's make sure to store the data
                         * about STDERR as that's usually the best way to do logging. */

                        if (fstat(fileno, &st) >= 0 &&
                            (*journal_stream_ino == 0 || fileno == STDERR_FILENO)) {
                                *journal_stream_dev = st.st_dev;
                                *journal_stream_ino = st.st_ino;
                        }
                }
                return r;

        case EXEC_OUTPUT_SOCKET:
                assert(socket_fd >= 0);

                return RET_NERRNO(dup2(socket_fd, fileno));

        case EXEC_OUTPUT_NAMED_FD:
                assert(named_iofds[fileno] >= 0);

                (void) fd_nonblock(named_iofds[fileno], false);
                return RET_NERRNO(dup2(named_iofds[fileno], fileno));

        case EXEC_OUTPUT_FILE:
        case EXEC_OUTPUT_FILE_APPEND:
        case EXEC_OUTPUT_FILE_TRUNCATE: {
                bool rw;
                int fd, flags;

                assert(context->stdio_file[fileno]);

                rw = context->std_input == EXEC_INPUT_FILE &&
                        streq_ptr(context->stdio_file[fileno], context->stdio_file[STDIN_FILENO]);

                if (rw)
                        return RET_NERRNO(dup2(STDIN_FILENO, fileno));

                flags = O_WRONLY;
                if (o == EXEC_OUTPUT_FILE_APPEND)
                        flags |= O_APPEND;
                else if (o == EXEC_OUTPUT_FILE_TRUNCATE)
                        flags |= O_TRUNC;

                fd = acquire_path(context->stdio_file[fileno], flags, 0666 & ~context->umask);
                if (fd < 0)
                        return fd;

                return move_fd(fd, fileno, 0);
        }

        default:
                assert_not_reached();
        }
}

static int chown_terminal(int fd, uid_t uid) {
        int r;

        assert(fd >= 0);

        /* Before we chown/chmod the TTY, let's ensure this is actually a tty */
        if (isatty(fd) < 1) {
                if (IN_SET(errno, EINVAL, ENOTTY))
                        return 0; /* not a tty */

                return -errno;
        }

        /* This might fail. What matters are the results. */
        r = fchmod_and_chown(fd, TTY_MODE, uid, GID_INVALID);
        if (r < 0)
                return r;

        return 1;
}

static int setup_confirm_stdio(
                const ExecContext *context,
                const char *vc,
                int *ret_saved_stdin,
                int *ret_saved_stdout) {

        _cleanup_close_ int fd = -1, saved_stdin = -1, saved_stdout = -1;
        int r;

        assert(ret_saved_stdin);
        assert(ret_saved_stdout);

        saved_stdin = fcntl(STDIN_FILENO, F_DUPFD, 3);
        if (saved_stdin < 0)
                return -errno;

        saved_stdout = fcntl(STDOUT_FILENO, F_DUPFD, 3);
        if (saved_stdout < 0)
                return -errno;

        fd = acquire_terminal(vc, ACQUIRE_TERMINAL_WAIT, DEFAULT_CONFIRM_USEC);
        if (fd < 0)
                return fd;

        r = chown_terminal(fd, getuid());
        if (r < 0)
                return r;

        r = reset_terminal_fd(fd, true);
        if (r < 0)
                return r;

        r = terminal_set_size_fd(fd, vc, context->tty_rows, context->tty_cols);
        if (r < 0)
                return r;

        r = rearrange_stdio(fd, fd, STDERR_FILENO); /* Invalidates 'fd' also on failure */
        TAKE_FD(fd);
        if (r < 0)
                return r;

        *ret_saved_stdin = TAKE_FD(saved_stdin);
        *ret_saved_stdout = TAKE_FD(saved_stdout);
        return 0;
}

static void write_confirm_error_fd(int err, int fd, const Unit *u) {
        assert(err < 0);

        if (err == -ETIMEDOUT)
                dprintf(fd, "Confirmation question timed out for %s, assuming positive response.\n", u->id);
        else {
                errno = -err;
                dprintf(fd, "Couldn't ask confirmation for %s: %m, assuming positive response.\n", u->id);
        }
}

static void write_confirm_error(int err, const char *vc, const Unit *u) {
        _cleanup_close_ int fd = -1;

        assert(vc);

        fd = open_terminal(vc, O_WRONLY|O_NOCTTY|O_CLOEXEC);
        if (fd < 0)
                return;

        write_confirm_error_fd(err, fd, u);
}

static int restore_confirm_stdio(int *saved_stdin, int *saved_stdout) {
        int r = 0;

        assert(saved_stdin);
        assert(saved_stdout);

        release_terminal();

        if (*saved_stdin >= 0)
                if (dup2(*saved_stdin, STDIN_FILENO) < 0)
                        r = -errno;

        if (*saved_stdout >= 0)
                if (dup2(*saved_stdout, STDOUT_FILENO) < 0)
                        r = -errno;

        *saved_stdin = safe_close(*saved_stdin);
        *saved_stdout = safe_close(*saved_stdout);

        return r;
}

enum {
        CONFIRM_PRETEND_FAILURE = -1,
        CONFIRM_PRETEND_SUCCESS =  0,
        CONFIRM_EXECUTE = 1,
};

static int ask_for_confirmation(const ExecContext *context, const char *vc, Unit *u, const char *cmdline) {
        int saved_stdout = -1, saved_stdin = -1, r;
        _cleanup_free_ char *e = NULL;
        char c;

        /* For any internal errors, assume a positive response. */
        r = setup_confirm_stdio(context, vc, &saved_stdin, &saved_stdout);
        if (r < 0) {
                write_confirm_error(r, vc, u);
                return CONFIRM_EXECUTE;
        }

        /* confirm_spawn might have been disabled while we were sleeping. */
        if (manager_is_confirm_spawn_disabled(u->manager)) {
                r = 1;
                goto restore_stdio;
        }

        e = ellipsize(cmdline, 60, 100);
        if (!e) {
                log_oom();
                r = CONFIRM_EXECUTE;
                goto restore_stdio;
        }

        for (;;) {
                r = ask_char(&c, "yfshiDjcn", "Execute %s? [y, f, s – h for help] ", e);
                if (r < 0) {
                        write_confirm_error_fd(r, STDOUT_FILENO, u);
                        r = CONFIRM_EXECUTE;
                        goto restore_stdio;
                }

                switch (c) {
                case 'c':
                        printf("Resuming normal execution.\n");
                        manager_disable_confirm_spawn();
                        r = 1;
                        break;
                case 'D':
                        unit_dump(u, stdout, "  ");
                        continue; /* ask again */
                case 'f':
                        printf("Failing execution.\n");
                        r = CONFIRM_PRETEND_FAILURE;
                        break;
                case 'h':
                        printf("  c - continue, proceed without asking anymore\n"
                               "  D - dump, show the state of the unit\n"
                               "  f - fail, don't execute the command and pretend it failed\n"
                               "  h - help\n"
                               "  i - info, show a short summary of the unit\n"
                               "  j - jobs, show jobs that are in progress\n"
                               "  s - skip, don't execute the command and pretend it succeeded\n"
                               "  y - yes, execute the command\n");
                        continue; /* ask again */
                case 'i':
                        printf("  Description: %s\n"
                               "  Unit:        %s\n"
                               "  Command:     %s\n",
                               u->id, u->description, cmdline);
                        continue; /* ask again */
                case 'j':
                        manager_dump_jobs(u->manager, stdout, /* patterns= */ NULL, "  ");
                        continue; /* ask again */
                case 'n':
                        /* 'n' was removed in favor of 'f'. */
                        printf("Didn't understand 'n', did you mean 'f'?\n");
                        continue; /* ask again */
                case 's':
                        printf("Skipping execution.\n");
                        r = CONFIRM_PRETEND_SUCCESS;
                        break;
                case 'y':
                        r = CONFIRM_EXECUTE;
                        break;
                default:
                        assert_not_reached();
                }
                break;
        }

restore_stdio:
        restore_confirm_stdio(&saved_stdin, &saved_stdout);
        return r;
}

static int get_fixed_user(const ExecContext *c, const char **user,
                          uid_t *uid, gid_t *gid,
                          const char **home, const char **shell) {
        int r;
        const char *name;

        assert(c);

        if (!c->user)
                return 0;

        /* Note that we don't set $HOME or $SHELL if they are not particularly enlightening anyway
         * (i.e. are "/" or "/bin/nologin"). */

        name = c->user;
        r = get_user_creds(&name, uid, gid, home, shell, USER_CREDS_CLEAN);
        if (r < 0)
                return r;

        *user = name;
        return 0;
}

static int get_fixed_group(const ExecContext *c, const char **group, gid_t *gid) {
        int r;
        const char *name;

        assert(c);

        if (!c->group)
                return 0;

        name = c->group;
        r = get_group_creds(&name, gid, 0);
        if (r < 0)
                return r;

        *group = name;
        return 0;
}

static int get_supplementary_groups(const ExecContext *c, const char *user,
                                    const char *group, gid_t gid,
                                    gid_t **supplementary_gids, int *ngids) {
        int r, k = 0;
        int ngroups_max;
        bool keep_groups = false;
        gid_t *groups = NULL;
        _cleanup_free_ gid_t *l_gids = NULL;

        assert(c);

        /*
         * If user is given, then lookup GID and supplementary groups list.
         * We avoid NSS lookups for gid=0. Also we have to initialize groups
         * here and as early as possible so we keep the list of supplementary
         * groups of the caller.
         */
        if (user && gid_is_valid(gid) && gid != 0) {
                /* First step, initialize groups from /etc/groups */
                if (initgroups(user, gid) < 0)
                        return -errno;

                keep_groups = true;
        }

        if (strv_isempty(c->supplementary_groups))
                return 0;

        /*
         * If SupplementaryGroups= was passed then NGROUPS_MAX has to
         * be positive, otherwise fail.
         */
        errno = 0;
        ngroups_max = (int) sysconf(_SC_NGROUPS_MAX);
        if (ngroups_max <= 0)
                return errno_or_else(EOPNOTSUPP);

        l_gids = new(gid_t, ngroups_max);
        if (!l_gids)
                return -ENOMEM;

        if (keep_groups) {
                /*
                 * Lookup the list of groups that the user belongs to, we
                 * avoid NSS lookups here too for gid=0.
                 */
                k = ngroups_max;
                if (getgrouplist(user, gid, l_gids, &k) < 0)
                        return -EINVAL;
        } else
                k = 0;

        STRV_FOREACH(i, c->supplementary_groups) {
                const char *g;

                if (k >= ngroups_max)
                        return -E2BIG;

                g = *i;
                r = get_group_creds(&g, l_gids+k, 0);
                if (r < 0)
                        return r;

                k++;
        }

        /*
         * Sets ngids to zero to drop all supplementary groups, happens
         * when we are under root and SupplementaryGroups= is empty.
         */
        if (k == 0) {
                *ngids = 0;
                return 0;
        }

        /* Otherwise get the final list of supplementary groups */
        groups = memdup(l_gids, sizeof(gid_t) * k);
        if (!groups)
                return -ENOMEM;

        *supplementary_gids = groups;
        *ngids = k;

        groups = NULL;

        return 0;
}

static int enforce_groups(gid_t gid, const gid_t *supplementary_gids, int ngids) {
        int r;

        /* Handle SupplementaryGroups= if it is not empty */
        if (ngids > 0) {
                r = maybe_setgroups(ngids, supplementary_gids);
                if (r < 0)
                        return r;
        }

        if (gid_is_valid(gid)) {
                /* Then set our gids */
                if (setresgid(gid, gid, gid) < 0)
                        return -errno;
        }

        return 0;
}

static int set_securebits(int bits, int mask) {
        int current, applied;
        current = prctl(PR_GET_SECUREBITS);
        if (current < 0)
                return -errno;
        /* Clear all securebits defined in mask and set bits */
        applied = (current & ~mask) | bits;
        if (current == applied)
                return 0;
        if (prctl(PR_SET_SECUREBITS, applied) < 0)
                return -errno;
        return 1;
}

static int enforce_user(const ExecContext *context, uid_t uid) {
        assert(context);
        int r;

        if (!uid_is_valid(uid))
                return 0;

        /* Sets (but doesn't look up) the uid and make sure we keep the
         * capabilities while doing so. For setting secure bits the capability CAP_SETPCAP is
         * required, so we also need keep-caps in this case.
         */

        if (context->capability_ambient_set != 0 || context->secure_bits != 0) {

                /* First step: If we need to keep capabilities but
                 * drop privileges we need to make sure we keep our
                 * caps, while we drop privileges. */
                if (uid != 0) {
                        /* Add KEEP_CAPS to the securebits */
                        r = set_securebits(1<<SECURE_KEEP_CAPS, 0);
                        if (r < 0)
                                return r;
                }
        }

        /* Second step: actually set the uids */
        if (setresuid(uid, uid, uid) < 0)
                return -errno;

        /* At this point we should have all necessary capabilities but
           are otherwise a normal user. However, the caps might got
           corrupted due to the setresuid() so we need clean them up
           later. This is done outside of this call. */

        return 0;
}

#if HAVE_PAM

static int null_conv(
                int num_msg,
                const struct pam_message **msg,
                struct pam_response **resp,
                void *appdata_ptr) {

        /* We don't support conversations */

        return PAM_CONV_ERR;
}

#endif

static int setup_pam(
                const char *name,
                const char *user,
                uid_t uid,
                gid_t gid,
                const char *tty,
                char ***env, /* updated on success */
                const int fds[], size_t n_fds) {

#if HAVE_PAM

        static const struct pam_conv conv = {
                .conv = null_conv,
                .appdata_ptr = NULL
        };

        _cleanup_(barrier_destroy) Barrier barrier = BARRIER_NULL;
        _cleanup_strv_free_ char **e = NULL;
        pam_handle_t *handle = NULL;
        sigset_t old_ss;
        int pam_code = PAM_SUCCESS, r;
        bool close_session = false;
        pid_t pam_pid = 0, parent_pid;
        int flags = 0;

        assert(name);
        assert(user);
        assert(env);

        /* We set up PAM in the parent process, then fork. The child
         * will then stay around until killed via PR_GET_PDEATHSIG or
         * systemd via the cgroup logic. It will then remove the PAM
         * session again. The parent process will exec() the actual
         * daemon. We do things this way to ensure that the main PID
         * of the daemon is the one we initially fork()ed. */

        r = barrier_create(&barrier);
        if (r < 0)
                goto fail;

        if (log_get_max_level() < LOG_DEBUG)
                flags |= PAM_SILENT;

        pam_code = pam_start(name, user, &conv, &handle);
        if (pam_code != PAM_SUCCESS) {
                handle = NULL;
                goto fail;
        }

        if (!tty) {
                _cleanup_free_ char *q = NULL;

                /* Hmm, so no TTY was explicitly passed, but an fd passed to us directly might be a TTY. Let's figure
                 * out if that's the case, and read the TTY off it. */

                if (getttyname_malloc(STDIN_FILENO, &q) >= 0)
                        tty = strjoina("/dev/", q);
        }

        if (tty) {
                pam_code = pam_set_item(handle, PAM_TTY, tty);
                if (pam_code != PAM_SUCCESS)
                        goto fail;
        }

        STRV_FOREACH(nv, *env) {
                pam_code = pam_putenv(handle, *nv);
                if (pam_code != PAM_SUCCESS)
                        goto fail;
        }

        pam_code = pam_acct_mgmt(handle, flags);
        if (pam_code != PAM_SUCCESS)
                goto fail;

        pam_code = pam_setcred(handle, PAM_ESTABLISH_CRED | flags);
        if (pam_code != PAM_SUCCESS)
                log_debug("pam_setcred() failed, ignoring: %s", pam_strerror(handle, pam_code));

        pam_code = pam_open_session(handle, flags);
        if (pam_code != PAM_SUCCESS)
                goto fail;

        close_session = true;

        e = pam_getenvlist(handle);
        if (!e) {
                pam_code = PAM_BUF_ERR;
                goto fail;
        }

        /* Block SIGTERM, so that we know that it won't get lost in the child */

        assert_se(sigprocmask_many(SIG_BLOCK, &old_ss, SIGTERM, -1) >= 0);

        parent_pid = getpid_cached();

        r = safe_fork("(sd-pam)", 0, &pam_pid);
        if (r < 0)
                goto fail;
        if (r == 0) {
                int sig, ret = EXIT_PAM;

                /* The child's job is to reset the PAM session on termination */
                barrier_set_role(&barrier, BARRIER_CHILD);

                /* Make sure we don't keep open the passed fds in this child. We assume that otherwise only
                 * those fds are open here that have been opened by PAM. */
                (void) close_many(fds, n_fds);

                /* Drop privileges - we don't need any to pam_close_session and this will make
                 * PR_SET_PDEATHSIG work in most cases.  If this fails, ignore the error - but expect sd-pam
                 * threads to fail to exit normally */

                r = maybe_setgroups(0, NULL);
                if (r < 0)
                        log_warning_errno(r, "Failed to setgroups() in sd-pam: %m");
                if (setresgid(gid, gid, gid) < 0)
                        log_warning_errno(errno, "Failed to setresgid() in sd-pam: %m");
                if (setresuid(uid, uid, uid) < 0)
                        log_warning_errno(errno, "Failed to setresuid() in sd-pam: %m");

                (void) ignore_signals(SIGPIPE);

                /* Wait until our parent died. This will only work if the above setresuid() succeeds,
                 * otherwise the kernel will not allow unprivileged parents kill their privileged children
                 * this way. We rely on the control groups kill logic to do the rest for us. */
                if (prctl(PR_SET_PDEATHSIG, SIGTERM) < 0)
                        goto child_finish;

                /* Tell the parent that our setup is done. This is especially important regarding dropping
                 * privileges. Otherwise, unit setup might race against our setresuid(2) call.
                 *
                 * If the parent aborted, we'll detect this below, hence ignore return failure here. */
                (void) barrier_place(&barrier);

                /* Check if our parent process might already have died? */
                if (getppid() == parent_pid) {
                        sigset_t ss;

                        assert_se(sigemptyset(&ss) >= 0);
                        assert_se(sigaddset(&ss, SIGTERM) >= 0);

                        for (;;) {
                                if (sigwait(&ss, &sig) < 0) {
                                        if (errno == EINTR)
                                                continue;

                                        goto child_finish;
                                }

                                assert(sig == SIGTERM);
                                break;
                        }
                }

                pam_code = pam_setcred(handle, PAM_DELETE_CRED | flags);
                if (pam_code != PAM_SUCCESS)
                        goto child_finish;

                /* If our parent died we'll end the session */
                if (getppid() != parent_pid) {
                        pam_code = pam_close_session(handle, flags);
                        if (pam_code != PAM_SUCCESS)
                                goto child_finish;
                }

                ret = 0;

        child_finish:
                /* NB: pam_end() when called in child processes should set PAM_DATA_SILENT to let the module
                 * know about this. See pam_end(3) */
                (void) pam_end(handle, pam_code | flags | PAM_DATA_SILENT);
                _exit(ret);
        }

        barrier_set_role(&barrier, BARRIER_PARENT);

        /* If the child was forked off successfully it will do all the cleanups, so forget about the handle
         * here. */
        handle = NULL;

        /* Unblock SIGTERM again in the parent */
        assert_se(sigprocmask(SIG_SETMASK, &old_ss, NULL) >= 0);

        /* We close the log explicitly here, since the PAM modules might have opened it, but we don't want
         * this fd around. */
        closelog();

        /* Synchronously wait for the child to initialize. We don't care for errors as we cannot
         * recover. However, warn loudly if it happens. */
        if (!barrier_place_and_sync(&barrier))
                log_error("PAM initialization failed");

        return strv_free_and_replace(*env, e);

fail:
        if (pam_code != PAM_SUCCESS) {
                log_error("PAM failed: %s", pam_strerror(handle, pam_code));
                r = -EPERM;  /* PAM errors do not map to errno */
        } else
                log_error_errno(r, "PAM failed: %m");

        if (handle) {
                if (close_session)
                        pam_code = pam_close_session(handle, flags);

                (void) pam_end(handle, pam_code | flags);
        }

        closelog();
        return r;
#else
        return 0;
#endif
}

static void rename_process_from_path(const char *path) {
        char process_name[11];
        const char *p;
        size_t l;

        /* This resulting string must fit in 10 chars (i.e. the length
         * of "/sbin/init") to look pretty in /bin/ps */

        p = basename(path);
        if (isempty(p)) {
                rename_process("(...)");
                return;
        }

        l = strlen(p);
        if (l > 8) {
                /* The end of the process name is usually more
                 * interesting, since the first bit might just be
                 * "systemd-" */
                p = p + l - 8;
                l = 8;
        }

        process_name[0] = '(';
        memcpy(process_name+1, p, l);
        process_name[1+l] = ')';
        process_name[1+l+1] = 0;

        rename_process(process_name);
}

static bool context_has_address_families(const ExecContext *c) {
        assert(c);

        return c->address_families_allow_list ||
                !set_isempty(c->address_families);
}

static bool context_has_syscall_filters(const ExecContext *c) {
        assert(c);

        return c->syscall_allow_list ||
                !hashmap_isempty(c->syscall_filter);
}

static bool context_has_syscall_logs(const ExecContext *c) {
        assert(c);

        return c->syscall_log_allow_list ||
                !hashmap_isempty(c->syscall_log);
}

static bool context_has_no_new_privileges(const ExecContext *c) {
        assert(c);

        if (c->no_new_privileges)
                return true;

        if (have_effective_cap(CAP_SYS_ADMIN)) /* if we are privileged, we don't need NNP */
                return false;

        /* We need NNP if we have any form of seccomp and are unprivileged */
        return c->lock_personality ||
                c->memory_deny_write_execute ||
                c->private_devices ||
                c->protect_clock ||
                c->protect_hostname ||
                c->protect_kernel_tunables ||
                c->protect_kernel_modules ||
                c->protect_kernel_logs ||
                context_has_address_families(c) ||
                exec_context_restrict_namespaces_set(c) ||
                c->restrict_realtime ||
                c->restrict_suid_sgid ||
                !set_isempty(c->syscall_archs) ||
                context_has_syscall_filters(c) ||
                context_has_syscall_logs(c);
}

static bool exec_context_has_credentials(const ExecContext *context) {

        assert(context);

        return !hashmap_isempty(context->set_credentials) ||
                !hashmap_isempty(context->load_credentials);
}

#if HAVE_SECCOMP

static bool skip_seccomp_unavailable(const Unit* u, const char* msg) {

        if (is_seccomp_available())
                return false;

        log_unit_debug(u, "SECCOMP features not detected in the kernel, skipping %s", msg);
        return true;
}

static int apply_syscall_filter(const Unit* u, const ExecContext *c, bool needs_ambient_hack) {
        uint32_t negative_action, default_action, action;
        int r;

        assert(u);
        assert(c);

        if (!context_has_syscall_filters(c))
                return 0;

        if (skip_seccomp_unavailable(u, "SystemCallFilter="))
                return 0;

        negative_action = c->syscall_errno == SECCOMP_ERROR_NUMBER_KILL ? scmp_act_kill_process() : SCMP_ACT_ERRNO(c->syscall_errno);

        if (c->syscall_allow_list) {
                default_action = negative_action;
                action = SCMP_ACT_ALLOW;
        } else {
                default_action = SCMP_ACT_ALLOW;
                action = negative_action;
        }

        if (needs_ambient_hack) {
                r = seccomp_filter_set_add(c->syscall_filter, c->syscall_allow_list, syscall_filter_sets + SYSCALL_FILTER_SET_SETUID);
                if (r < 0)
                        return r;
        }

        return seccomp_load_syscall_filter_set_raw(default_action, c->syscall_filter, action, false);
}

static int apply_syscall_log(const Unit* u, const ExecContext *c) {
#ifdef SCMP_ACT_LOG
        uint32_t default_action, action;
#endif

        assert(u);
        assert(c);

        if (!context_has_syscall_logs(c))
                return 0;

#ifdef SCMP_ACT_LOG
        if (skip_seccomp_unavailable(u, "SystemCallLog="))
                return 0;

        if (c->syscall_log_allow_list) {
                /* Log nothing but the ones listed */
                default_action = SCMP_ACT_ALLOW;
                action = SCMP_ACT_LOG;
        } else {
                /* Log everything but the ones listed */
                default_action = SCMP_ACT_LOG;
                action = SCMP_ACT_ALLOW;
        }

        return seccomp_load_syscall_filter_set_raw(default_action, c->syscall_log, action, false);
#else
        /* old libseccomp */
        log_unit_debug(u, "SECCOMP feature SCMP_ACT_LOG not available, skipping SystemCallLog=");
        return 0;
#endif
}

static int apply_syscall_archs(const Unit *u, const ExecContext *c) {
        assert(u);
        assert(c);

        if (set_isempty(c->syscall_archs))
                return 0;

        if (skip_seccomp_unavailable(u, "SystemCallArchitectures="))
                return 0;

        return seccomp_restrict_archs(c->syscall_archs);
}

static int apply_address_families(const Unit* u, const ExecContext *c) {
        assert(u);
        assert(c);

        if (!context_has_address_families(c))
                return 0;

        if (skip_seccomp_unavailable(u, "RestrictAddressFamilies="))
                return 0;

        return seccomp_restrict_address_families(c->address_families, c->address_families_allow_list);
}

static int apply_memory_deny_write_execute(const Unit* u, const ExecContext *c) {
        assert(u);
        assert(c);

        if (!c->memory_deny_write_execute)
                return 0;

        if (skip_seccomp_unavailable(u, "MemoryDenyWriteExecute="))
                return 0;

        return seccomp_memory_deny_write_execute();
}

static int apply_restrict_realtime(const Unit* u, const ExecContext *c) {
        assert(u);
        assert(c);

        if (!c->restrict_realtime)
                return 0;

        if (skip_seccomp_unavailable(u, "RestrictRealtime="))
                return 0;

        return seccomp_restrict_realtime();
}

static int apply_restrict_suid_sgid(const Unit* u, const ExecContext *c) {
        assert(u);
        assert(c);

        if (!c->restrict_suid_sgid)
                return 0;

        if (skip_seccomp_unavailable(u, "RestrictSUIDSGID="))
                return 0;

        return seccomp_restrict_suid_sgid();
}

static int apply_protect_sysctl(const Unit *u, const ExecContext *c) {
        assert(u);
        assert(c);

        /* Turn off the legacy sysctl() system call. Many distributions turn this off while building the kernel, but
         * let's protect even those systems where this is left on in the kernel. */

        if (!c->protect_kernel_tunables)
                return 0;

        if (skip_seccomp_unavailable(u, "ProtectKernelTunables="))
                return 0;

        return seccomp_protect_sysctl();
}

static int apply_protect_kernel_modules(const Unit *u, const ExecContext *c) {
        assert(u);
        assert(c);

        /* Turn off module syscalls on ProtectKernelModules=yes */

        if (!c->protect_kernel_modules)
                return 0;

        if (skip_seccomp_unavailable(u, "ProtectKernelModules="))
                return 0;

        return seccomp_load_syscall_filter_set(SCMP_ACT_ALLOW, syscall_filter_sets + SYSCALL_FILTER_SET_MODULE, SCMP_ACT_ERRNO(EPERM), false);
}

static int apply_protect_kernel_logs(const Unit *u, const ExecContext *c) {
        assert(u);
        assert(c);

        if (!c->protect_kernel_logs)
                return 0;

        if (skip_seccomp_unavailable(u, "ProtectKernelLogs="))
                return 0;

        return seccomp_protect_syslog();
}

static int apply_protect_clock(const Unit *u, const ExecContext *c) {
        assert(u);
        assert(c);

        if (!c->protect_clock)
                return 0;

        if (skip_seccomp_unavailable(u, "ProtectClock="))
                return 0;

        return seccomp_load_syscall_filter_set(SCMP_ACT_ALLOW, syscall_filter_sets + SYSCALL_FILTER_SET_CLOCK, SCMP_ACT_ERRNO(EPERM), false);
}

static int apply_private_devices(const Unit *u, const ExecContext *c) {
        assert(u);
        assert(c);

        /* If PrivateDevices= is set, also turn off iopl and all @raw-io syscalls. */

        if (!c->private_devices)
                return 0;

        if (skip_seccomp_unavailable(u, "PrivateDevices="))
                return 0;

        return seccomp_load_syscall_filter_set(SCMP_ACT_ALLOW, syscall_filter_sets + SYSCALL_FILTER_SET_RAW_IO, SCMP_ACT_ERRNO(EPERM), false);
}

static int apply_restrict_namespaces(const Unit *u, const ExecContext *c) {
        assert(u);
        assert(c);

        if (!exec_context_restrict_namespaces_set(c))
                return 0;

        if (skip_seccomp_unavailable(u, "RestrictNamespaces="))
                return 0;

        return seccomp_restrict_namespaces(c->restrict_namespaces);
}

static int apply_lock_personality(const Unit* u, const ExecContext *c) {
        unsigned long personality;
        int r;

        assert(u);
        assert(c);

        if (!c->lock_personality)
                return 0;

        if (skip_seccomp_unavailable(u, "LockPersonality="))
                return 0;

        personality = c->personality;

        /* If personality is not specified, use either PER_LINUX or PER_LINUX32 depending on what is currently set. */
        if (personality == PERSONALITY_INVALID) {

                r = opinionated_personality(&personality);
                if (r < 0)
                        return r;
        }

        return seccomp_lock_personality(personality);
}

#endif

#if HAVE_LIBBPF
static int apply_restrict_filesystems(Unit *u, const ExecContext *c) {
        assert(u);
        assert(c);

        if (!exec_context_restrict_filesystems_set(c))
                return 0;

        if (!u->manager->restrict_fs) {
                /* LSM BPF is unsupported or lsm_bpf_setup failed */
                log_unit_debug(u, "LSM BPF not supported, skipping RestrictFileSystems=");
                return 0;
        }

        return lsm_bpf_unit_restrict_filesystems(u, c->restrict_filesystems, c->restrict_filesystems_allow_list);
}
#endif

static int apply_protect_hostname(const Unit *u, const ExecContext *c, int *ret_exit_status) {
        assert(u);
        assert(c);

        if (!c->protect_hostname)
                return 0;

        if (ns_type_supported(NAMESPACE_UTS)) {
                if (unshare(CLONE_NEWUTS) < 0) {
                        if (!ERRNO_IS_NOT_SUPPORTED(errno) && !ERRNO_IS_PRIVILEGE(errno)) {
                                *ret_exit_status = EXIT_NAMESPACE;
                                return log_unit_error_errno(u, errno, "Failed to set up UTS namespacing: %m");
                        }

                        log_unit_warning(u, "ProtectHostname=yes is configured, but UTS namespace setup is prohibited (container manager?), ignoring namespace setup.");
                }
        } else
                log_unit_warning(u, "ProtectHostname=yes is configured, but the kernel does not support UTS namespaces, ignoring namespace setup.");

#if HAVE_SECCOMP
        int r;

        if (skip_seccomp_unavailable(u, "ProtectHostname="))
                return 0;

        r = seccomp_protect_hostname();
        if (r < 0) {
                *ret_exit_status = EXIT_SECCOMP;
                return log_unit_error_errno(u, r, "Failed to apply hostname restrictions: %m");
        }
#endif

        return 0;
}

static void do_idle_pipe_dance(int idle_pipe[static 4]) {
        assert(idle_pipe);

        idle_pipe[1] = safe_close(idle_pipe[1]);
        idle_pipe[2] = safe_close(idle_pipe[2]);

        if (idle_pipe[0] >= 0) {
                int r;

                r = fd_wait_for_event(idle_pipe[0], POLLHUP, IDLE_TIMEOUT_USEC);

                if (idle_pipe[3] >= 0 && r == 0 /* timeout */) {
                        ssize_t n;

                        /* Signal systemd that we are bored and want to continue. */
                        n = write(idle_pipe[3], "x", 1);
                        if (n > 0)
                                /* Wait for systemd to react to the signal above. */
                                (void) fd_wait_for_event(idle_pipe[0], POLLHUP, IDLE_TIMEOUT2_USEC);
                }

                idle_pipe[0] = safe_close(idle_pipe[0]);

        }

        idle_pipe[3] = safe_close(idle_pipe[3]);
}

static const char *exec_directory_env_name_to_string(ExecDirectoryType t);

static int build_environment(
                const Unit *u,
                const ExecContext *c,
                const ExecParameters *p,
                size_t n_fds,
                const char *home,
                const char *username,
                const char *shell,
                dev_t journal_stream_dev,
                ino_t journal_stream_ino,
                char ***ret) {

        _cleanup_strv_free_ char **our_env = NULL;
        size_t n_env = 0;
        char *x;

        assert(u);
        assert(c);
        assert(p);
        assert(ret);

#define N_ENV_VARS 17
        our_env = new0(char*, N_ENV_VARS + _EXEC_DIRECTORY_TYPE_MAX);
        if (!our_env)
                return -ENOMEM;

        if (n_fds > 0) {
                _cleanup_free_ char *joined = NULL;

                if (asprintf(&x, "LISTEN_PID="PID_FMT, getpid_cached()) < 0)
                        return -ENOMEM;
                our_env[n_env++] = x;

                if (asprintf(&x, "LISTEN_FDS=%zu", n_fds) < 0)
                        return -ENOMEM;
                our_env[n_env++] = x;

                joined = strv_join(p->fd_names, ":");
                if (!joined)
                        return -ENOMEM;

                x = strjoin("LISTEN_FDNAMES=", joined);
                if (!x)
                        return -ENOMEM;
                our_env[n_env++] = x;
        }

        if ((p->flags & EXEC_SET_WATCHDOG) && p->watchdog_usec > 0) {
                if (asprintf(&x, "WATCHDOG_PID="PID_FMT, getpid_cached()) < 0)
                        return -ENOMEM;
                our_env[n_env++] = x;

                if (asprintf(&x, "WATCHDOG_USEC="USEC_FMT, p->watchdog_usec) < 0)
                        return -ENOMEM;
                our_env[n_env++] = x;
        }

        /* If this is D-Bus, tell the nss-systemd module, since it relies on being able to use blocking
         * Varlink calls back to us for look up dynamic users in PID 1. Break the deadlock between D-Bus and
         * PID 1 by disabling use of PID1' NSS interface for looking up dynamic users. */
        if (p->flags & EXEC_NSS_DYNAMIC_BYPASS) {
                x = strdup("SYSTEMD_NSS_DYNAMIC_BYPASS=1");
                if (!x)
                        return -ENOMEM;
                our_env[n_env++] = x;
        }

        if (home) {
                x = strjoin("HOME=", home);
                if (!x)
                        return -ENOMEM;

                path_simplify(x + 5);
                our_env[n_env++] = x;
        }

        if (username) {
                x = strjoin("LOGNAME=", username);
                if (!x)
                        return -ENOMEM;
                our_env[n_env++] = x;

                x = strjoin("USER=", username);
                if (!x)
                        return -ENOMEM;
                our_env[n_env++] = x;
        }

        if (shell) {
                x = strjoin("SHELL=", shell);
                if (!x)
                        return -ENOMEM;

                path_simplify(x + 6);
                our_env[n_env++] = x;
        }

        if (!sd_id128_is_null(u->invocation_id)) {
                if (asprintf(&x, "INVOCATION_ID=" SD_ID128_FORMAT_STR, SD_ID128_FORMAT_VAL(u->invocation_id)) < 0)
                        return -ENOMEM;

                our_env[n_env++] = x;
        }

        if (exec_context_needs_term(c)) {
                const char *tty_path, *term = NULL;

                tty_path = exec_context_tty_path(c);

                /* If we are forked off PID 1 and we are supposed to operate on /dev/console, then let's try
                 * to inherit the $TERM set for PID 1. This is useful for containers so that the $TERM the
                 * container manager passes to PID 1 ends up all the way in the console login shown. */

                if (path_equal_ptr(tty_path, "/dev/console") && getppid() == 1)
                        term = getenv("TERM");

                if (!term)
                        term = default_term_for_tty(tty_path);

                x = strjoin("TERM=", term);
                if (!x)
                        return -ENOMEM;
                our_env[n_env++] = x;
        }

        if (journal_stream_dev != 0 && journal_stream_ino != 0) {
                if (asprintf(&x, "JOURNAL_STREAM=" DEV_FMT ":" INO_FMT, journal_stream_dev, journal_stream_ino) < 0)
                        return -ENOMEM;

                our_env[n_env++] = x;
        }

        if (c->log_namespace) {
                x = strjoin("LOG_NAMESPACE=", c->log_namespace);
                if (!x)
                        return -ENOMEM;

                our_env[n_env++] = x;
        }

        for (ExecDirectoryType t = 0; t < _EXEC_DIRECTORY_TYPE_MAX; t++) {
                _cleanup_free_ char *joined = NULL;
                const char *n;

                if (!p->prefix[t])
                        continue;

                if (c->directories[t].n_items == 0)
                        continue;

                n = exec_directory_env_name_to_string(t);
                if (!n)
                        continue;

                for (size_t i = 0; i < c->directories[t].n_items; i++) {
                        _cleanup_free_ char *prefixed = NULL;

                        prefixed = path_join(p->prefix[t], c->directories[t].items[i].path);
                        if (!prefixed)
                                return -ENOMEM;

                        if (!strextend_with_separator(&joined, ":", prefixed))
                                return -ENOMEM;
                }

                x = strjoin(n, "=", joined);
                if (!x)
                        return -ENOMEM;

                our_env[n_env++] = x;
        }

        if (exec_context_has_credentials(c) && p->prefix[EXEC_DIRECTORY_RUNTIME]) {
                x = strjoin("CREDENTIALS_DIRECTORY=", p->prefix[EXEC_DIRECTORY_RUNTIME], "/credentials/", u->id);
                if (!x)
                        return -ENOMEM;

                our_env[n_env++] = x;
        }

        if (asprintf(&x, "SYSTEMD_EXEC_PID=" PID_FMT, getpid_cached()) < 0)
                return -ENOMEM;

        our_env[n_env++] = x;

        our_env[n_env++] = NULL;
        assert(n_env <= N_ENV_VARS + _EXEC_DIRECTORY_TYPE_MAX);
#undef N_ENV_VARS

        *ret = TAKE_PTR(our_env);

        return 0;
}

static int build_pass_environment(const ExecContext *c, char ***ret) {
        _cleanup_strv_free_ char **pass_env = NULL;
        size_t n_env = 0;

        STRV_FOREACH(i, c->pass_environment) {
                _cleanup_free_ char *x = NULL;
                char *v;

                v = getenv(*i);
                if (!v)
                        continue;
                x = strjoin(*i, "=", v);
                if (!x)
                        return -ENOMEM;

                if (!GREEDY_REALLOC(pass_env, n_env + 2))
                        return -ENOMEM;

                pass_env[n_env++] = TAKE_PTR(x);
                pass_env[n_env] = NULL;
        }

        *ret = TAKE_PTR(pass_env);

        return 0;
}

bool exec_needs_mount_namespace(
                const ExecContext *context,
                const ExecParameters *params,
                const ExecRuntime *runtime) {

        assert(context);

        if (context->root_image)
                return true;

        if (!strv_isempty(context->read_write_paths) ||
            !strv_isempty(context->read_only_paths) ||
            !strv_isempty(context->inaccessible_paths) ||
            !strv_isempty(context->exec_paths) ||
            !strv_isempty(context->no_exec_paths))
                return true;

        if (context->n_bind_mounts > 0)
                return true;

        if (context->n_temporary_filesystems > 0)
                return true;

        if (context->n_mount_images > 0)
                return true;

        if (context->n_extension_images > 0)
                return true;

        if (!strv_isempty(context->extension_directories))
                return true;

        if (!IN_SET(context->mount_flags, 0, MS_SHARED))
                return true;

        if (context->private_tmp && runtime && (runtime->tmp_dir || runtime->var_tmp_dir))
                return true;

        if (context->private_devices ||
            context->private_mounts ||
            context->protect_system != PROTECT_SYSTEM_NO ||
            context->protect_home != PROTECT_HOME_NO ||
            context->protect_kernel_tunables ||
            context->protect_kernel_modules ||
            context->protect_kernel_logs ||
            context->protect_control_groups ||
            context->protect_proc != PROTECT_PROC_DEFAULT ||
            context->proc_subset != PROC_SUBSET_ALL ||
            context->private_ipc ||
            context->ipc_namespace_path)
                return true;

        if (context->root_directory) {
                if (exec_context_get_effective_mount_apivfs(context))
                        return true;

                for (ExecDirectoryType t = 0; t < _EXEC_DIRECTORY_TYPE_MAX; t++) {
                        if (params && !params->prefix[t])
                                continue;

                        if (context->directories[t].n_items > 0)
                                return true;
                }
        }

        if (context->dynamic_user &&
            (context->directories[EXEC_DIRECTORY_STATE].n_items > 0 ||
             context->directories[EXEC_DIRECTORY_CACHE].n_items > 0 ||
             context->directories[EXEC_DIRECTORY_LOGS].n_items > 0))
                return true;

        if (context->log_namespace)
                return true;

        return false;
}

static int setup_private_users(uid_t ouid, gid_t ogid, uid_t uid, gid_t gid) {
        _cleanup_free_ char *uid_map = NULL, *gid_map = NULL;
        _cleanup_close_pair_ int errno_pipe[2] = { -1, -1 };
        _cleanup_close_ int unshare_ready_fd = -1;
        _cleanup_(sigkill_waitp) pid_t pid = 0;
        uint64_t c = 1;
        ssize_t n;
        int r;

        /* Set up a user namespace and map the original UID/GID (IDs from before any user or group changes, i.e.
         * the IDs from the user or system manager(s)) to itself, the selected UID/GID to itself, and everything else to
         * nobody. In order to be able to write this mapping we need CAP_SETUID in the original user namespace, which
         * we however lack after opening the user namespace. To work around this we fork() a temporary child process,
         * which waits for the parent to create the new user namespace while staying in the original namespace. The
         * child then writes the UID mapping, under full privileges. The parent waits for the child to finish and
         * continues execution normally.
         * For unprivileged users (i.e. without capabilities), the root to root mapping is excluded. As such, it
         * does not need CAP_SETUID to write the single line mapping to itself. */

        /* Can only set up multiple mappings with CAP_SETUID. */
        if (have_effective_cap(CAP_SETUID) && uid != ouid && uid_is_valid(uid))
                r = asprintf(&uid_map,
                             UID_FMT " " UID_FMT " 1\n"     /* Map $OUID → $OUID */
                             UID_FMT " " UID_FMT " 1\n",    /* Map $UID → $UID */
                             ouid, ouid, uid, uid);
        else
                r = asprintf(&uid_map,
                             UID_FMT " " UID_FMT " 1\n",    /* Map $OUID → $OUID */
                             ouid, ouid);

        if (r < 0)
                return -ENOMEM;

        /* Can only set up multiple mappings with CAP_SETGID. */
        if (have_effective_cap(CAP_SETGID) && gid != ogid && gid_is_valid(gid))
                r = asprintf(&gid_map,
                             GID_FMT " " GID_FMT " 1\n"     /* Map $OGID → $OGID */
                             GID_FMT " " GID_FMT " 1\n",    /* Map $GID → $GID */
                             ogid, ogid, gid, gid);
        else
                r = asprintf(&gid_map,
                             GID_FMT " " GID_FMT " 1\n",    /* Map $OGID -> $OGID */
                             ogid, ogid);

        if (r < 0)
                return -ENOMEM;

        /* Create a communication channel so that the parent can tell the child when it finished creating the user
         * namespace. */
        unshare_ready_fd = eventfd(0, EFD_CLOEXEC);
        if (unshare_ready_fd < 0)
                return -errno;

        /* Create a communication channel so that the child can tell the parent a proper error code in case it
         * failed. */
        if (pipe2(errno_pipe, O_CLOEXEC) < 0)
                return -errno;

        r = safe_fork("(sd-userns)", FORK_RESET_SIGNALS|FORK_DEATHSIG, &pid);
        if (r < 0)
                return r;
        if (r == 0) {
                _cleanup_close_ int fd = -1;
                const char *a;
                pid_t ppid;

                /* Child process, running in the original user namespace. Let's update the parent's UID/GID map from
                 * here, after the parent opened its own user namespace. */

                ppid = getppid();
                errno_pipe[0] = safe_close(errno_pipe[0]);

                /* Wait until the parent unshared the user namespace */
                if (read(unshare_ready_fd, &c, sizeof(c)) < 0) {
                        r = -errno;
                        goto child_fail;
                }

                /* Disable the setgroups() system call in the child user namespace, for good. */
                a = procfs_file_alloca(ppid, "setgroups");
                fd = open(a, O_WRONLY|O_CLOEXEC);
                if (fd < 0) {
                        if (errno != ENOENT) {
                                r = -errno;
                                goto child_fail;
                        }

                        /* If the file is missing the kernel is too old, let's continue anyway. */
                } else {
                        if (write(fd, "deny\n", 5) < 0) {
                                r = -errno;
                                goto child_fail;
                        }

                        fd = safe_close(fd);
                }

                /* First write the GID map */
                a = procfs_file_alloca(ppid, "gid_map");
                fd = open(a, O_WRONLY|O_CLOEXEC);
                if (fd < 0) {
                        r = -errno;
                        goto child_fail;
                }
                if (write(fd, gid_map, strlen(gid_map)) < 0) {
                        r = -errno;
                        goto child_fail;
                }
                fd = safe_close(fd);

                /* The write the UID map */
                a = procfs_file_alloca(ppid, "uid_map");
                fd = open(a, O_WRONLY|O_CLOEXEC);
                if (fd < 0) {
                        r = -errno;
                        goto child_fail;
                }
                if (write(fd, uid_map, strlen(uid_map)) < 0) {
                        r = -errno;
                        goto child_fail;
                }

                _exit(EXIT_SUCCESS);

        child_fail:
                (void) write(errno_pipe[1], &r, sizeof(r));
                _exit(EXIT_FAILURE);
        }

        errno_pipe[1] = safe_close(errno_pipe[1]);

        if (unshare(CLONE_NEWUSER) < 0)
                return -errno;

        /* Let the child know that the namespace is ready now */
        if (write(unshare_ready_fd, &c, sizeof(c)) < 0)
                return -errno;

        /* Try to read an error code from the child */
        n = read(errno_pipe[0], &r, sizeof(r));
        if (n < 0)
                return -errno;
        if (n == sizeof(r)) { /* an error code was sent to us */
                if (r < 0)
                        return r;
                return -EIO;
        }
        if (n != 0) /* on success we should have read 0 bytes */
                return -EIO;

        r = wait_for_terminate_and_check("(sd-userns)", TAKE_PID(pid), 0);
        if (r < 0)
                return r;
        if (r != EXIT_SUCCESS) /* If something strange happened with the child, let's consider this fatal, too */
                return -EIO;

        return 0;
}

static bool exec_directory_is_private(const ExecContext *context, ExecDirectoryType type) {
        if (!context->dynamic_user)
                return false;

        if (type == EXEC_DIRECTORY_CONFIGURATION)
                return false;

        if (type == EXEC_DIRECTORY_RUNTIME && context->runtime_directory_preserve_mode == EXEC_PRESERVE_NO)
                return false;

        return true;
}

static int create_many_symlinks(const char *root, const char *source, char **symlinks) {
        _cleanup_free_ char *src_abs = NULL;
        int r;

        assert(source);

        src_abs = path_join(root, source);
        if (!src_abs)
                return -ENOMEM;

        STRV_FOREACH(dst, symlinks) {
                _cleanup_free_ char *dst_abs = NULL;

                dst_abs = path_join(root, *dst);
                if (!dst_abs)
                        return -ENOMEM;

                r = mkdir_parents_label(dst_abs, 0755);
                if (r < 0)
                        return r;

                r = symlink_idempotent(src_abs, dst_abs, true);
                if (r < 0)
                        return r;
        }

        return 0;
}

static int setup_exec_directory(
                const ExecContext *context,
                const ExecParameters *params,
                uid_t uid,
                gid_t gid,
                ExecDirectoryType type,
                bool needs_mount_namespace,
                int *exit_status) {

        static const int exit_status_table[_EXEC_DIRECTORY_TYPE_MAX] = {
                [EXEC_DIRECTORY_RUNTIME] = EXIT_RUNTIME_DIRECTORY,
                [EXEC_DIRECTORY_STATE] = EXIT_STATE_DIRECTORY,
                [EXEC_DIRECTORY_CACHE] = EXIT_CACHE_DIRECTORY,
                [EXEC_DIRECTORY_LOGS] = EXIT_LOGS_DIRECTORY,
                [EXEC_DIRECTORY_CONFIGURATION] = EXIT_CONFIGURATION_DIRECTORY,
        };
        int r;

        assert(context);
        assert(params);
        assert(type >= 0 && type < _EXEC_DIRECTORY_TYPE_MAX);
        assert(exit_status);

        if (!params->prefix[type])
                return 0;

        if (params->flags & EXEC_CHOWN_DIRECTORIES) {
                if (!uid_is_valid(uid))
                        uid = 0;
                if (!gid_is_valid(gid))
                        gid = 0;
        }

        for (size_t i = 0; i < context->directories[type].n_items; i++) {
                _cleanup_free_ char *p = NULL, *pp = NULL;

                p = path_join(params->prefix[type], context->directories[type].items[i].path);
                if (!p) {
                        r = -ENOMEM;
                        goto fail;
                }

                r = mkdir_parents_label(p, 0755);
                if (r < 0)
                        goto fail;

                if (exec_directory_is_private(context, type)) {
                        /* So, here's one extra complication when dealing with DynamicUser=1 units. In that
                         * case we want to avoid leaving a directory around fully accessible that is owned by
                         * a dynamic user whose UID is later on reused. To lock this down we use the same
                         * trick used by container managers to prohibit host users to get access to files of
                         * the same UID in containers: we place everything inside a directory that has an
                         * access mode of 0700 and is owned root:root, so that it acts as security boundary
                         * for unprivileged host code. We then use fs namespacing to make this directory
                         * permeable for the service itself.
                         *
                         * Specifically: for a service which wants a special directory "foo/" we first create
                         * a directory "private/" with access mode 0700 owned by root:root. Then we place
                         * "foo" inside of that directory (i.e. "private/foo/"), and make "foo" a symlink to
                         * "private/foo". This way, privileged host users can access "foo/" as usual, but
                         * unprivileged host users can't look into it. Inside of the namespace of the unit
                         * "private/" is replaced by a more liberally accessible tmpfs, into which the host's
                         * "private/foo/" is mounted under the same name, thus disabling the access boundary
                         * for the service and making sure it only gets access to the dirs it needs but no
                         * others. Tricky? Yes, absolutely, but it works!
                         *
                         * Note that we don't do this for EXEC_DIRECTORY_CONFIGURATION as that's assumed not
                         * to be owned by the service itself.
                         *
                         * Also, note that we don't do this for EXEC_DIRECTORY_RUNTIME as that's often used
                         * for sharing files or sockets with other services. */

                        pp = path_join(params->prefix[type], "private");
                        if (!pp) {
                                r = -ENOMEM;
                                goto fail;
                        }

                        /* First set up private root if it doesn't exist yet, with access mode 0700 and owned by root:root */
                        r = mkdir_safe_label(pp, 0700, 0, 0, MKDIR_WARN_MODE);
                        if (r < 0)
                                goto fail;

                        if (!path_extend(&pp, context->directories[type].items[i].path)) {
                                r = -ENOMEM;
                                goto fail;
                        }

                        /* Create all directories between the configured directory and this private root, and mark them 0755 */
                        r = mkdir_parents_label(pp, 0755);
                        if (r < 0)
                                goto fail;

                        if (is_dir(p, false) > 0 &&
                            (laccess(pp, F_OK) < 0 && errno == ENOENT)) {

                                /* Hmm, the private directory doesn't exist yet, but the normal one exists? If so, move
                                 * it over. Most likely the service has been upgraded from one that didn't use
                                 * DynamicUser=1, to one that does. */

                                log_info("Found pre-existing public %s= directory %s, migrating to %s.\n"
                                         "Apparently, service previously had DynamicUser= turned off, and has now turned it on.",
                                         exec_directory_type_to_string(type), p, pp);

                                if (rename(p, pp) < 0) {
                                        r = -errno;
                                        goto fail;
                                }
                        } else {
                                /* Otherwise, create the actual directory for the service */

                                r = mkdir_label(pp, context->directories[type].mode);
                                if (r < 0 && r != -EEXIST)
                                        goto fail;
                        }

                        if (!context->directories[type].items[i].only_create) {
                                /* And link it up from the original place.
                                 * Notes
                                 * 1) If a mount namespace is going to be used, then this symlink remains on
                                 *    the host, and a new one for the child namespace will be created later.
                                 * 2) It is not necessary to create this symlink when one of its parent
                                 *    directories is specified and already created. E.g.
                                 *        StateDirectory=foo foo/bar
                                 *    In that case, the inode points to pp and p for "foo/bar" are the same:
                                 *        pp = "/var/lib/private/foo/bar"
                                 *        p = "/var/lib/foo/bar"
                                 *    and, /var/lib/foo is a symlink to /var/lib/private/foo. So, not only
                                 *    we do not need to create the symlink, but we cannot create the symlink.
                                 *    See issue #24783. */
                                r = symlink_idempotent(pp, p, true);
                                if (r < 0)
                                        goto fail;
                        }

                } else {
                        _cleanup_free_ char *target = NULL;

                        if (type != EXEC_DIRECTORY_CONFIGURATION &&
                            readlink_and_make_absolute(p, &target) >= 0) {
                                _cleanup_free_ char *q = NULL, *q_resolved = NULL, *target_resolved = NULL;

                                /* This already exists and is a symlink? Interesting. Maybe it's one created
                                 * by DynamicUser=1 (see above)?
                                 *
                                 * We do this for all directory types except for ConfigurationDirectory=,
                                 * since they all support the private/ symlink logic at least in some
                                 * configurations, see above. */

                                r = chase_symlinks(target, NULL, 0, &target_resolved, NULL);
                                if (r < 0)
                                        goto fail;

                                q = path_join(params->prefix[type], "private", context->directories[type].items[i].path);
                                if (!q) {
                                        r = -ENOMEM;
                                        goto fail;
                                }

                                /* /var/lib or friends may be symlinks. So, let's chase them also. */
                                r = chase_symlinks(q, NULL, CHASE_NONEXISTENT, &q_resolved, NULL);
                                if (r < 0)
                                        goto fail;

                                if (path_equal(q_resolved, target_resolved)) {

                                        /* Hmm, apparently DynamicUser= was once turned on for this service,
                                         * but is no longer. Let's move the directory back up. */

                                        log_info("Found pre-existing private %s= directory %s, migrating to %s.\n"
                                                 "Apparently, service previously had DynamicUser= turned on, and has now turned it off.",
                                                 exec_directory_type_to_string(type), q, p);

                                        if (unlink(p) < 0) {
                                                r = -errno;
                                                goto fail;
                                        }

                                        if (rename(q, p) < 0) {
                                                r = -errno;
                                                goto fail;
                                        }
                                }
                        }

                        r = mkdir_label(p, context->directories[type].mode);
                        if (r < 0) {
                                if (r != -EEXIST)
                                        goto fail;

                                if (type == EXEC_DIRECTORY_CONFIGURATION) {
                                        struct stat st;

                                        /* Don't change the owner/access mode of the configuration directory,
                                         * as in the common case it is not written to by a service, and shall
                                         * not be writable. */

                                        if (stat(p, &st) < 0) {
                                                r = -errno;
                                                goto fail;
                                        }

                                        /* Still complain if the access mode doesn't match */
                                        if (((st.st_mode ^ context->directories[type].mode) & 07777) != 0)
                                                log_warning("%s \'%s\' already exists but the mode is different. "
                                                            "(File system: %o %sMode: %o)",
                                                            exec_directory_type_to_string(type), context->directories[type].items[i].path,
                                                            st.st_mode & 07777, exec_directory_type_to_string(type), context->directories[type].mode & 07777);

                                        continue;
                                }
                        }
                }

                /* Lock down the access mode (we use chmod_and_chown() to make this idempotent. We don't
                 * specify UID/GID here, so that path_chown_recursive() can optimize things depending on the
                 * current UID/GID ownership.) */
                r = chmod_and_chown(pp ?: p, context->directories[type].mode, UID_INVALID, GID_INVALID);
                if (r < 0)
                        goto fail;

                /* Then, change the ownership of the whole tree, if necessary. When dynamic users are used we
                 * drop the suid/sgid bits, since we really don't want SUID/SGID files for dynamic UID/GID
                 * assignments to exist. */
                r = path_chown_recursive(pp ?: p, uid, gid, context->dynamic_user ? 01777 : 07777);
                if (r < 0)
                        goto fail;
        }

        /* If we are not going to run in a namespace, set up the symlinks - otherwise
         * they are set up later, to allow configuring empty var/run/etc. */
        if (!needs_mount_namespace)
                for (size_t i = 0; i < context->directories[type].n_items; i++) {
                        r = create_many_symlinks(params->prefix[type],
                                                 context->directories[type].items[i].path,
                                                 context->directories[type].items[i].symlinks);
                        if (r < 0)
                                goto fail;
                }

        return 0;

fail:
        *exit_status = exit_status_table[type];
        return r;
}

static int write_credential(
                int dfd,
                const char *id,
                const void *data,
                size_t size,
                uid_t uid,
                bool ownership_ok) {

        _cleanup_(unlink_and_freep) char *tmp = NULL;
        _cleanup_close_ int fd = -1;
        int r;

        r = tempfn_random_child("", "cred", &tmp);
        if (r < 0)
                return r;

        fd = openat(dfd, tmp, O_CREAT|O_RDWR|O_CLOEXEC|O_EXCL|O_NOFOLLOW|O_NOCTTY, 0600);
        if (fd < 0) {
                tmp = mfree(tmp);
                return -errno;
        }

        r = loop_write(fd, data, size, /* do_poll = */ false);
        if (r < 0)
                return r;

        if (fchmod(fd, 0400) < 0) /* Take away "w" bit */
                return -errno;

        if (uid_is_valid(uid) && uid != getuid()) {
                r = fd_add_uid_acl_permission(fd, uid, ACL_READ);
                if (r < 0) {
                        if (!ERRNO_IS_NOT_SUPPORTED(r) && !ERRNO_IS_PRIVILEGE(r))
                                return r;

                        if (!ownership_ok) /* Ideally we use ACLs, since we can neatly express what we want
                                            * to express: that the user gets read access and nothing
                                            * else. But if the backing fs can't support that (e.g. ramfs)
                                            * then we can use file ownership instead. But that's only safe if
                                            * we can then re-mount the whole thing read-only, so that the
                                            * user can no longer chmod() the file to gain write access. */
                                return r;

                        if (fchown(fd, uid, GID_INVALID) < 0)
                                return -errno;
                }
        }

        if (renameat(dfd, tmp, dfd, id) < 0)
                return -errno;

        tmp = mfree(tmp);
        return 0;
}

static char **credential_search_path(
                const ExecParameters *params,
                bool encrypted) {

        _cleanup_strv_free_ char **l = NULL;

        assert(params);

        /* Assemble a search path to find credentials in. We'll look in /etc/credstore/ (and similar
         * directories in /usr/lib/ + /run/) for all types of credentials. If we are looking for encrypted
         * credentials, also look in /etc/credstore.encrypted/ (and similar dirs). */

        if (encrypted) {
                if (strv_extend(&l, params->received_encrypted_credentials_directory) < 0)
                        return NULL;

                if (strv_extend_strv(&l, CONF_PATHS_STRV("credstore.encrypted"), /* filter_duplicates= */ true) < 0)
                        return NULL;
        }

        if (params->received_credentials_directory)
                if (strv_extend(&l, params->received_credentials_directory) < 0)
                        return NULL;

        if (strv_extend_strv(&l, CONF_PATHS_STRV("credstore"), /* filter_duplicates= */ true) < 0)
                return NULL;

        if (DEBUG_LOGGING) {
                _cleanup_free_ char *t = strv_join(l, ":");

                log_debug("Credential search path is: %s", strempty(t));
        }

        return TAKE_PTR(l);
}

static int load_credential(
                const ExecContext *context,
                const ExecParameters *params,
                const char *id,
                const char *path,
                bool encrypted,
                const char *unit,
                int read_dfd,
                int write_dfd,
                uid_t uid,
                bool ownership_ok,
                uint64_t *left) {

        ReadFullFileFlags flags = READ_FULL_FILE_SECURE|READ_FULL_FILE_FAIL_WHEN_LARGER;
        _cleanup_strv_free_ char **search_path = NULL;
        _cleanup_(erase_and_freep) char *data = NULL;
        _cleanup_free_ char *bindname = NULL;
        const char *source = NULL;
        bool missing_ok = true;
        size_t size, add, maxsz;
        int r;

        assert(context);
        assert(params);
        assert(id);
        assert(path);
        assert(unit);
        assert(read_dfd >= 0 || read_dfd == AT_FDCWD);
        assert(write_dfd >= 0);
        assert(left);

        if (read_dfd >= 0) {
                /* If a directory fd is specified, then read the file directly from that dir. In this case we
                 * won't do AF_UNIX stuff (we simply don't want to recursively iterate down a tree of AF_UNIX
                 * IPC sockets). It's OK if a file vanishes here in the time we enumerate it and intend to
                 * open it. */

                if (!filename_is_valid(path)) /* safety check */
                        return -EINVAL;

                missing_ok = true;
                source = path;

        } else if (path_is_absolute(path)) {
                /* If this is an absolute path, read the data directly from it, and support AF_UNIX
                 * sockets */

                if (!path_is_valid(path)) /* safety check */
                        return -EINVAL;

                flags |= READ_FULL_FILE_CONNECT_SOCKET;

                /* Pass some minimal info about the unit and the credential name we are looking to acquire
                 * via the source socket address in case we read off an AF_UNIX socket. */
                if (asprintf(&bindname, "@%" PRIx64"/unit/%s/%s", random_u64(), unit, id) < 0)
                        return -ENOMEM;

                missing_ok = false;
                source = path;

        } else if (credential_name_valid(path)) {
                /* If this is a relative path, take it as credential name relative to the credentials
                 * directory we received ourselves. We don't support the AF_UNIX stuff in this mode, since we
                 * are operating on a credential store, i.e. this is guaranteed to be regular files. */

                search_path = credential_search_path(params, encrypted);
                if (!search_path)
                        return -ENOMEM;

                missing_ok = true;
        } else
                source = NULL;

        if (encrypted)
                flags |= READ_FULL_FILE_UNBASE64;

        maxsz = encrypted ? CREDENTIAL_ENCRYPTED_SIZE_MAX : CREDENTIAL_SIZE_MAX;

        if (search_path) {
                STRV_FOREACH(d, search_path) {
                        _cleanup_free_ char *j = NULL;

                        j = path_join(*d, path);
                        if (!j)
                                return -ENOMEM;

                        r = read_full_file_full(
                                        AT_FDCWD, j, /* path is absolute, hence pass AT_FDCWD as nop dir fd here */
                                        UINT64_MAX,
                                        maxsz,
                                        flags,
                                        NULL,
                                        &data, &size);
                        if (r != -ENOENT)
                                break;
                }
        } else if (source)
                r = read_full_file_full(
                                read_dfd, source,
                                UINT64_MAX,
                                maxsz,
                                flags,
                                bindname,
                                &data, &size);
        else
                r = -ENOENT;

        if (r == -ENOENT && (missing_ok || hashmap_contains(context->set_credentials, id))) {
                /* Make a missing inherited credential non-fatal, let's just continue. After all apps
                 * will get clear errors if we don't pass such a missing credential on as they
                 * themselves will get ENOENT when trying to read them, which should not be much
                 * worse than when we handle the error here and make it fatal.
                 *
                 * Also, if the source file doesn't exist, but a fallback is set via SetCredentials=
                 * we are fine, too. */
                log_debug_errno(r, "Couldn't read inherited credential '%s', skipping: %m", path);
                return 0;
        }
        if (r < 0)
                return log_debug_errno(r, "Failed to read credential '%s': %m", path);

        if (encrypted) {
                _cleanup_free_ void *plaintext = NULL;
                size_t plaintext_size = 0;

                r = decrypt_credential_and_warn(id, now(CLOCK_REALTIME), NULL, NULL, data, size, &plaintext, &plaintext_size);
                if (r < 0)
                        return r;

                free_and_replace(data, plaintext);
                size = plaintext_size;
        }

        add = strlen(id) + size;
        if (add > *left)
                return -E2BIG;

        r = write_credential(write_dfd, id, data, size, uid, ownership_ok);
        if (r < 0)
                return log_debug_errno(r, "Failed to write credential '%s': %m", id);

        *left -= add;
        return 0;
}

struct load_cred_args {
        const ExecContext *context;
        const ExecParameters *params;
        bool encrypted;
        const char *unit;
        int dfd;
        uid_t uid;
        bool ownership_ok;
        uint64_t *left;
};

static int load_cred_recurse_dir_cb(
                RecurseDirEvent event,
                const char *path,
                int dir_fd,
                int inode_fd,
                const struct dirent *de,
                const struct statx *sx,
                void *userdata) {

        struct load_cred_args *args = ASSERT_PTR(userdata);
        _cleanup_free_ char *sub_id = NULL;
        int r;

        if (event != RECURSE_DIR_ENTRY)
                return RECURSE_DIR_CONTINUE;

        if (!IN_SET(de->d_type, DT_REG, DT_SOCK))
                return RECURSE_DIR_CONTINUE;

        sub_id = strreplace(path, "/", "_");
        if (!sub_id)
                return -ENOMEM;

        if (!credential_name_valid(sub_id))
                return log_debug_errno(SYNTHETIC_ERRNO(EINVAL), "Credential would get ID %s, which is not valid, refusing", sub_id);

        if (faccessat(args->dfd, sub_id, F_OK, AT_SYMLINK_NOFOLLOW) >= 0) {
                log_debug("Skipping credential with duplicated ID %s at %s", sub_id, path);
                return RECURSE_DIR_CONTINUE;
        }
        if (errno != ENOENT)
                return log_debug_errno(errno, "Failed to test if credential %s exists: %m", sub_id);

        r = load_credential(
                        args->context,
                        args->params,
                        sub_id,
                        de->d_name,
                        args->encrypted,
                        args->unit,
                        dir_fd,
                        args->dfd,
                        args->uid,
                        args->ownership_ok,
                        args->left);
        if (r < 0)
                return r;

        return RECURSE_DIR_CONTINUE;
}

static int acquire_credentials(
                const ExecContext *context,
                const ExecParameters *params,
                const char *unit,
                const char *p,
                uid_t uid,
                bool ownership_ok) {

        uint64_t left = CREDENTIALS_TOTAL_SIZE_MAX;
        _cleanup_close_ int dfd = -1;
        ExecLoadCredential *lc;
        ExecSetCredential *sc;
        int r;

        assert(context);
        assert(p);

        dfd = open(p, O_DIRECTORY|O_CLOEXEC);
        if (dfd < 0)
                return -errno;

        r = fd_acl_make_writable(dfd); /* Add the "w" bit, if we are reusing an already set up credentials dir where it was unset */
        if (r < 0)
                return r;

        /* First, load credentials off disk (or acquire via AF_UNIX socket) */
        HASHMAP_FOREACH(lc, context->load_credentials) {
                _cleanup_close_ int sub_fd = -1;

                /* If this is an absolute path, then try to open it as a directory. If that works, then we'll
                 * recurse into it. If it is an absolute path but it isn't a directory, then we'll open it as
                 * a regular file. Finally, if it's a relative path we will use it as a credential name to
                 * propagate a credential passed to us from further up. */

                if (path_is_absolute(lc->path)) {
                        sub_fd = open(lc->path, O_DIRECTORY|O_CLOEXEC|O_RDONLY);
                        if (sub_fd < 0 && !IN_SET(errno,
                                                  ENOTDIR,  /* Not a directory */
                                                  ENOENT))  /* Doesn't exist? */
                                return log_debug_errno(errno, "Failed to open '%s': %m", lc->path);
                }

                if (sub_fd < 0)
                        /* Regular file (incl. a credential passed in from higher up) */
                        r = load_credential(
                                        context,
                                        params,
                                        lc->id,
                                        lc->path,
                                        lc->encrypted,
                                        unit,
                                        AT_FDCWD,
                                        dfd,
                                        uid,
                                        ownership_ok,
                                        &left);
                else
                        /* Directory */
                        r = recurse_dir(
                                        sub_fd,
                                        /* path= */ lc->id, /* recurse_dir() will suffix the subdir paths from here to the top-level id */
                                        /* statx_mask= */ 0,
                                        /* n_depth_max= */ UINT_MAX,
                                        RECURSE_DIR_SORT|RECURSE_DIR_IGNORE_DOT|RECURSE_DIR_ENSURE_TYPE,
                                        load_cred_recurse_dir_cb,
                                        &(struct load_cred_args) {
                                                .context = context,
                                                .params = params,
                                                .encrypted = lc->encrypted,
                                                .unit = unit,
                                                .dfd = dfd,
                                                .uid = uid,
                                                .ownership_ok = ownership_ok,
                                                .left = &left,
                                        });
                if (r < 0)
                        return r;
        }

        /* Second, we add in literally specified credentials. If the credentials already exist, we'll not add
         * them, so that they can act as a "default" if the same credential is specified multiple times. */
        HASHMAP_FOREACH(sc, context->set_credentials) {
                _cleanup_(erase_and_freep) void *plaintext = NULL;
                const char *data;
                size_t size, add;

                /* Note that we check ahead of time here instead of relying on O_EXCL|O_CREAT later to return
                 * EEXIST if the credential already exists. That's because the TPM2-based decryption is kinda
                 * slow and involved, hence it's nice to be able to skip that if the credential already
                 * exists anyway. */
                if (faccessat(dfd, sc->id, F_OK, AT_SYMLINK_NOFOLLOW) >= 0)
                        continue;
                if (errno != ENOENT)
                        return log_debug_errno(errno, "Failed to test if credential %s exists: %m", sc->id);

                if (sc->encrypted) {
                        r = decrypt_credential_and_warn(sc->id, now(CLOCK_REALTIME), NULL, NULL, sc->data, sc->size, &plaintext, &size);
                        if (r < 0)
                                return r;

                        data = plaintext;
                } else {
                        data = sc->data;
                        size = sc->size;
                }

                add = strlen(sc->id) + size;
                if (add > left)
                        return -E2BIG;

                r = write_credential(dfd, sc->id, data, size, uid, ownership_ok);
                if (r < 0)
                        return r;

                left -= add;
        }

        r = fd_acl_make_read_only(dfd); /* Now take away the "w" bit */
        if (r < 0)
                return r;

        /* After we created all keys with the right perms, also make sure the credential store as a whole is
         * accessible */

        if (uid_is_valid(uid) && uid != getuid()) {
                r = fd_add_uid_acl_permission(dfd, uid, ACL_READ | ACL_EXECUTE);
                if (r < 0) {
                        if (!ERRNO_IS_NOT_SUPPORTED(r) && !ERRNO_IS_PRIVILEGE(r))
                                return r;

                        if (!ownership_ok)
                                return r;

                        if (fchown(dfd, uid, GID_INVALID) < 0)
                                return -errno;
                }
        }

        return 0;
}

static int setup_credentials_internal(
                const ExecContext *context,
                const ExecParameters *params,
                const char *unit,
                const char *final,        /* This is where the credential store shall eventually end up at */
                const char *workspace,    /* This is where we can prepare it before moving it to the final place */
                bool reuse_workspace,     /* Whether to reuse any existing workspace mount if it already is a mount */
                bool must_mount,          /* Whether to require that we mount something, it's not OK to use the plain directory fall back */
                uid_t uid) {

        int r, workspace_mounted; /* negative if we don't know yet whether we have/can mount something; true
                                   * if we mounted something; false if we definitely can't mount anything */
        bool final_mounted;
        const char *where;

        assert(context);
        assert(final);
        assert(workspace);

        if (reuse_workspace) {
                r = path_is_mount_point(workspace, NULL, 0);
                if (r < 0)
                        return r;
                if (r > 0)
                        workspace_mounted = true; /* If this is already a mount, and we are supposed to reuse it, let's keep this in mind */
                else
                        workspace_mounted = -1; /* We need to figure out if we can mount something to the workspace */
        } else
                workspace_mounted = -1; /* ditto */

        r = path_is_mount_point(final, NULL, 0);
        if (r < 0)
                return r;
        if (r > 0) {
                /* If the final place already has something mounted, we use that. If the workspace also has
                 * something mounted we assume it's actually the same mount (but with MS_RDONLY
                 * different). */
                final_mounted = true;

                if (workspace_mounted < 0) {
                        /* If the final place is mounted, but the workspace we isn't, then let's bind mount
                         * the final version to the workspace, and make it writable, so that we can make
                         * changes */

                        r = mount_nofollow_verbose(LOG_DEBUG, final, workspace, NULL, MS_BIND|MS_REC, NULL);
                        if (r < 0)
                                return r;

                        r = mount_nofollow_verbose(LOG_DEBUG, NULL, workspace, NULL, MS_BIND|MS_REMOUNT|MS_NODEV|MS_NOEXEC|MS_NOSUID, NULL);
                        if (r < 0)
                                return r;

                        workspace_mounted = true;
                }
        } else
                final_mounted = false;

        if (workspace_mounted < 0) {
                /* Nothing is mounted on the workspace yet, let's try to mount something now */
                for (int try = 0;; try++) {

                        if (try == 0) {
                                /* Try "ramfs" first, since it's not swap backed */
                                r = mount_nofollow_verbose(LOG_DEBUG, "ramfs", workspace, "ramfs", MS_NODEV|MS_NOEXEC|MS_NOSUID, "mode=0700");
                                if (r >= 0) {
                                        workspace_mounted = true;
                                        break;
                                }

                        } else if (try == 1) {
                                _cleanup_free_ char *opts = NULL;

                                if (asprintf(&opts, "mode=0700,nr_inodes=1024,size=%zu", (size_t) CREDENTIALS_TOTAL_SIZE_MAX) < 0)
                                        return -ENOMEM;

                                /* Fall back to "tmpfs" otherwise */
                                r = mount_nofollow_verbose(LOG_DEBUG, "tmpfs", workspace, "tmpfs", MS_NODEV|MS_NOEXEC|MS_NOSUID, opts);
                                if (r >= 0) {
                                        workspace_mounted = true;
                                        break;
                                }

                        } else {
                                /* If that didn't work, try to make a bind mount from the final to the workspace, so that we can make it writable there. */
                                r = mount_nofollow_verbose(LOG_DEBUG, final, workspace, NULL, MS_BIND|MS_REC, NULL);
                                if (r < 0) {
                                        if (!ERRNO_IS_PRIVILEGE(r)) /* Propagate anything that isn't a permission problem */
                                                return r;

                                        if (must_mount) /* If we it's not OK to use the plain directory
                                                         * fallback, propagate all errors too */
                                                return r;

                                        /* If we lack privileges to bind mount stuff, then let's gracefully
                                         * proceed for compat with container envs, and just use the final dir
                                         * as is. */

                                        workspace_mounted = false;
                                        break;
                                }

                                /* Make the new bind mount writable (i.e. drop MS_RDONLY) */
                                r = mount_nofollow_verbose(LOG_DEBUG, NULL, workspace, NULL, MS_BIND|MS_REMOUNT|MS_NODEV|MS_NOEXEC|MS_NOSUID, NULL);
                                if (r < 0)
                                        return r;

                                workspace_mounted = true;
                                break;
                        }
                }
        }

        assert(!must_mount || workspace_mounted > 0);
        where = workspace_mounted ? workspace : final;

        (void) label_fix_full(AT_FDCWD, where, final, 0);

        r = acquire_credentials(context, params, unit, where, uid, workspace_mounted);
        if (r < 0)
                return r;

        if (workspace_mounted) {
                /* Make workspace read-only now, so that any bind mount we make from it defaults to read-only too */
                r = mount_nofollow_verbose(LOG_DEBUG, NULL, workspace, NULL, MS_BIND|MS_REMOUNT|MS_RDONLY|MS_NODEV|MS_NOEXEC|MS_NOSUID, NULL);
                if (r < 0)
                        return r;

                /* And mount it to the final place, read-only */
                if (final_mounted)
                        r = umount_verbose(LOG_DEBUG, workspace, MNT_DETACH|UMOUNT_NOFOLLOW);
                else
                        r = mount_nofollow_verbose(LOG_DEBUG, workspace, final, NULL, MS_MOVE, NULL);
                if (r < 0)
                        return r;
        } else {
                _cleanup_free_ char *parent = NULL;

                /* If we do not have our own mount put used the plain directory fallback, then we need to
                 * open access to the top-level credential directory and the per-service directory now */

                r = path_extract_directory(final, &parent);
                if (r < 0)
                        return r;
                if (chmod(parent, 0755) < 0)
                        return -errno;
        }

        return 0;
}

static int setup_credentials(
                const ExecContext *context,
                const ExecParameters *params,
                const char *unit,
                uid_t uid) {

        _cleanup_free_ char *p = NULL, *q = NULL;
        int r;

        assert(context);
        assert(params);

        if (!exec_context_has_credentials(context))
                return 0;

        if (!params->prefix[EXEC_DIRECTORY_RUNTIME])
                return -EINVAL;

        /* This where we'll place stuff when we are done; this main credentials directory is world-readable,
         * and the subdir we mount over with a read-only file system readable by the service's user */
        q = path_join(params->prefix[EXEC_DIRECTORY_RUNTIME], "credentials");
        if (!q)
                return -ENOMEM;

        r = mkdir_label(q, 0755); /* top-level dir: world readable/searchable */
        if (r < 0 && r != -EEXIST)
                return r;

        p = path_join(q, unit);
        if (!p)
                return -ENOMEM;

        r = mkdir_label(p, 0700); /* per-unit dir: private to user */
        if (r < 0 && r != -EEXIST)
                return r;

        r = safe_fork("(sd-mkdcreds)", FORK_DEATHSIG|FORK_WAIT|FORK_NEW_MOUNTNS, NULL);
        if (r < 0) {
                _cleanup_free_ char *t = NULL, *u = NULL;

                /* If this is not a privilege or support issue then propagate the error */
                if (!ERRNO_IS_NOT_SUPPORTED(r) && !ERRNO_IS_PRIVILEGE(r))
                        return r;

                /* Temporary workspace, that remains inaccessible all the time. We prepare stuff there before moving
                 * it into place, so that users can't access half-initialized credential stores. */
                t = path_join(params->prefix[EXEC_DIRECTORY_RUNTIME], "systemd/temporary-credentials");
                if (!t)
                        return -ENOMEM;

                /* We can't set up a mount namespace. In that case operate on a fixed, inaccessible per-unit
                 * directory outside of /run/credentials/ first, and then move it over to /run/credentials/
                 * after it is fully set up */
                u = path_join(t, unit);
                if (!u)
                        return -ENOMEM;

                FOREACH_STRING(i, t, u) {
                        r = mkdir_label(i, 0700);
                        if (r < 0 && r != -EEXIST)
                                return r;
                }

                r = setup_credentials_internal(
                                context,
                                params,
                                unit,
                                p,       /* final mount point */
                                u,       /* temporary workspace to overmount */
                                true,    /* reuse the workspace if it is already a mount */
                                false,   /* it's OK to fall back to a plain directory if we can't mount anything */
                                uid);

                (void) rmdir(u); /* remove the workspace again if we can. */

                if (r < 0)
                        return r;

        } else if (r == 0) {

                /* We managed to set up a mount namespace, and are now in a child. That's great. In this case
                 * we can use the same directory for all cases, after turning off propagation. Question
                 * though is: where do we turn off propagation exactly, and where do we place the workspace
                 * directory? We need some place that is guaranteed to be a mount point in the host, and
                 * which is guaranteed to have a subdir we can mount over. /run/ is not suitable for this,
                 * since we ultimately want to move the resulting file system there, i.e. we need propagation
                 * for /run/ eventually. We could use our own /run/systemd/bind mount on itself, but that
                 * would be visible in the host mount table all the time, which we want to avoid. Hence, what
                 * we do here instead we use /dev/ and /dev/shm/ for our purposes. We know for sure that
                 * /dev/ is a mount point and we now for sure that /dev/shm/ exists. Hence we can turn off
                 * propagation on the former, and then overmount the latter.
                 *
                 * Yes it's nasty playing games with /dev/ and /dev/shm/ like this, since it does not exist
                 * for this purpose, but there are few other candidates that work equally well for us, and
                 * given that the we do this in a privately namespaced short-lived single-threaded process
                 * that no one else sees this should be OK to do. */

                r = mount_nofollow_verbose(LOG_DEBUG, NULL, "/dev", NULL, MS_SLAVE|MS_REC, NULL); /* Turn off propagation from our namespace to host */
                if (r < 0)
                        goto child_fail;

                r = setup_credentials_internal(
                                context,
                                params,
                                unit,
                                p,           /* final mount point */
                                "/dev/shm",  /* temporary workspace to overmount */
                                false,       /* do not reuse /dev/shm if it is already a mount, under no circumstances */
                                true,        /* insist that something is mounted, do not allow fallback to plain directory */
                                uid);
                if (r < 0)
                        goto child_fail;

                _exit(EXIT_SUCCESS);

        child_fail:
                _exit(EXIT_FAILURE);
        }

        return 0;
}

#if ENABLE_SMACK
static int setup_smack(
                const Manager *manager,
                const ExecContext *context,
                int executable_fd) {
        int r;

        assert(context);
        assert(executable_fd >= 0);

        if (context->smack_process_label) {
                r = mac_smack_apply_pid(0, context->smack_process_label);
                if (r < 0)
                        return r;
        } else if (manager->default_smack_process_label) {
                _cleanup_free_ char *exec_label = NULL;

                r = mac_smack_read_fd(executable_fd, SMACK_ATTR_EXEC, &exec_label);
                if (r < 0 && !ERRNO_IS_XATTR_ABSENT(r))
                        return r;

                r = mac_smack_apply_pid(0, exec_label ? : manager->default_smack_process_label);
                if (r < 0)
                        return r;
        }

        return 0;
}
#endif

static int compile_bind_mounts(
                const ExecContext *context,
                const ExecParameters *params,
                BindMount **ret_bind_mounts,
                size_t *ret_n_bind_mounts,
                char ***ret_empty_directories) {

        _cleanup_strv_free_ char **empty_directories = NULL;
        BindMount *bind_mounts;
        size_t n, h = 0;
        int r;

        assert(context);
        assert(params);
        assert(ret_bind_mounts);
        assert(ret_n_bind_mounts);
        assert(ret_empty_directories);

        n = context->n_bind_mounts;
        for (ExecDirectoryType t = 0; t < _EXEC_DIRECTORY_TYPE_MAX; t++) {
                if (!params->prefix[t])
                        continue;

                for (size_t i = 0; i < context->directories[t].n_items; i++)
                        n += !context->directories[t].items[i].only_create;
        }

        if (n <= 0) {
                *ret_bind_mounts = NULL;
                *ret_n_bind_mounts = 0;
                *ret_empty_directories = NULL;
                return 0;
        }

        bind_mounts = new(BindMount, n);
        if (!bind_mounts)
                return -ENOMEM;

        for (size_t i = 0; i < context->n_bind_mounts; i++) {
                BindMount *item = context->bind_mounts + i;
                char *s, *d;

                s = strdup(item->source);
                if (!s) {
                        r = -ENOMEM;
                        goto finish;
                }

                d = strdup(item->destination);
                if (!d) {
                        free(s);
                        r = -ENOMEM;
                        goto finish;
                }

                bind_mounts[h++] = (BindMount) {
                        .source = s,
                        .destination = d,
                        .read_only = item->read_only,
                        .recursive = item->recursive,
                        .ignore_enoent = item->ignore_enoent,
                };
        }

        for (ExecDirectoryType t = 0; t < _EXEC_DIRECTORY_TYPE_MAX; t++) {
                if (!params->prefix[t])
                        continue;

                if (context->directories[t].n_items == 0)
                        continue;

                if (exec_directory_is_private(context, t) &&
                    !exec_context_with_rootfs(context)) {
                        char *private_root;

                        /* So this is for a dynamic user, and we need to make sure the process can access its own
                         * directory. For that we overmount the usually inaccessible "private" subdirectory with a
                         * tmpfs that makes it accessible and is empty except for the submounts we do this for. */

                        private_root = path_join(params->prefix[t], "private");
                        if (!private_root) {
                                r = -ENOMEM;
                                goto finish;
                        }

                        r = strv_consume(&empty_directories, private_root);
                        if (r < 0)
                                goto finish;
                }

                for (size_t i = 0; i < context->directories[t].n_items; i++) {
                        char *s, *d;

                        /* When one of the parent directories is in the list, we cannot create the symlink
                         * for the child directory. See also the comments in setup_exec_directory(). */
                        if (context->directories[t].items[i].only_create)
                                continue;

                        if (exec_directory_is_private(context, t))
                                s = path_join(params->prefix[t], "private", context->directories[t].items[i].path);
                        else
                                s = path_join(params->prefix[t], context->directories[t].items[i].path);
                        if (!s) {
                                r = -ENOMEM;
                                goto finish;
                        }

                        if (exec_directory_is_private(context, t) &&
                            exec_context_with_rootfs(context))
                                /* When RootDirectory= or RootImage= are set, then the symbolic link to the private
                                 * directory is not created on the root directory. So, let's bind-mount the directory
                                 * on the 'non-private' place. */
                                d = path_join(params->prefix[t], context->directories[t].items[i].path);
                        else
                                d = strdup(s);
                        if (!d) {
                                free(s);
                                r = -ENOMEM;
                                goto finish;
                        }

                        bind_mounts[h++] = (BindMount) {
                                .source = s,
                                .destination = d,
                                .read_only = false,
                                .nosuid = context->dynamic_user, /* don't allow suid/sgid when DynamicUser= is on */
                                .recursive = true,
                                .ignore_enoent = false,
                        };
                }
        }

        assert(h == n);

        *ret_bind_mounts = bind_mounts;
        *ret_n_bind_mounts = n;
        *ret_empty_directories = TAKE_PTR(empty_directories);

        return (int) n;

finish:
        bind_mount_free_many(bind_mounts, h);
        return r;
}

/* ret_symlinks will contain a list of pairs src:dest that describes
 * the symlinks to create later on. For example, the symlinks needed
 * to safely give private directories to DynamicUser=1 users. */
static int compile_symlinks(
                const ExecContext *context,
                const ExecParameters *params,
                char ***ret_symlinks) {

        _cleanup_strv_free_ char **symlinks = NULL;
        int r;

        assert(context);
        assert(params);
        assert(ret_symlinks);

        for (ExecDirectoryType dt = 0; dt < _EXEC_DIRECTORY_TYPE_MAX; dt++) {
                for (size_t i = 0; i < context->directories[dt].n_items; i++) {
                        _cleanup_free_ char *private_path = NULL, *path = NULL;

                        STRV_FOREACH(symlink, context->directories[dt].items[i].symlinks) {
                                _cleanup_free_ char *src_abs = NULL, *dst_abs = NULL;

                                src_abs = path_join(params->prefix[dt], context->directories[dt].items[i].path);
                                dst_abs = path_join(params->prefix[dt], *symlink);
                                if (!src_abs || !dst_abs)
                                        return -ENOMEM;

                                r = strv_consume_pair(&symlinks, TAKE_PTR(src_abs), TAKE_PTR(dst_abs));
                                if (r < 0)
                                        return r;
                        }

                        if (!exec_directory_is_private(context, dt) ||
                            exec_context_with_rootfs(context) ||
                            context->directories[dt].items[i].only_create)
                                continue;

                        private_path = path_join(params->prefix[dt], "private", context->directories[dt].items[i].path);
                        if (!private_path)
                                return -ENOMEM;

                        path = path_join(params->prefix[dt], context->directories[dt].items[i].path);
                        if (!path)
                                return -ENOMEM;

                        r = strv_consume_pair(&symlinks, TAKE_PTR(private_path), TAKE_PTR(path));
                        if (r < 0)
                                return r;
                }
        }

        *ret_symlinks = TAKE_PTR(symlinks);

        return 0;
}

static bool insist_on_sandboxing(
                const ExecContext *context,
                const char *root_dir,
                const char *root_image,
                const BindMount *bind_mounts,
                size_t n_bind_mounts) {

        assert(context);
        assert(n_bind_mounts == 0 || bind_mounts);

        /* Checks whether we need to insist on fs namespacing. i.e. whether we have settings configured that
         * would alter the view on the file system beyond making things read-only or invisible, i.e. would
         * rearrange stuff in a way we cannot ignore gracefully. */

        if (context->n_temporary_filesystems > 0)
                return true;

        if (root_dir || root_image)
                return true;

        if (context->n_mount_images > 0)
                return true;

        if (context->dynamic_user)
                return true;

        if (context->n_extension_images > 0 || !strv_isempty(context->extension_directories))
                return true;

        /* If there are any bind mounts set that don't map back onto themselves, fs namespacing becomes
         * essential. */
        for (size_t i = 0; i < n_bind_mounts; i++)
                if (!path_equal(bind_mounts[i].source, bind_mounts[i].destination))
                        return true;

        if (context->log_namespace)
                return true;

        return false;
}

static int apply_mount_namespace(
                const Unit *u,
                ExecCommandFlags command_flags,
                const ExecContext *context,
                const ExecParameters *params,
                const ExecRuntime *runtime,
                char **error_path) {

        _cleanup_strv_free_ char **empty_directories = NULL, **symlinks = NULL;
        const char *tmp_dir = NULL, *var_tmp_dir = NULL;
        const char *root_dir = NULL, *root_image = NULL;
        _cleanup_free_ char *creds_path = NULL, *incoming_dir = NULL, *propagate_dir = NULL,
                        *extension_dir = NULL;
        NamespaceInfo ns_info;
        bool needs_sandboxing;
        BindMount *bind_mounts = NULL;
        size_t n_bind_mounts = 0;
        int r;

        assert(context);

        if (params->flags & EXEC_APPLY_CHROOT) {
                root_image = context->root_image;

                if (!root_image)
                        root_dir = context->root_directory;
        }

        r = compile_bind_mounts(context, params, &bind_mounts, &n_bind_mounts, &empty_directories);
        if (r < 0)
                return r;

        /* Symlinks for exec dirs are set up after other mounts, before they are made read-only. */
        r = compile_symlinks(context, params, &symlinks);
        if (r < 0)
                goto finalize;

        needs_sandboxing = (params->flags & EXEC_APPLY_SANDBOXING) && !(command_flags & EXEC_COMMAND_FULLY_PRIVILEGED);
        if (needs_sandboxing) {
                /* The runtime struct only contains the parent of the private /tmp,
                 * which is non-accessible to world users. Inside of it there's a /tmp
                 * that is sticky, and that's the one we want to use here.
                 * This does not apply when we are using /run/systemd/empty as fallback. */

                if (context->private_tmp && runtime) {
                        if (streq_ptr(runtime->tmp_dir, RUN_SYSTEMD_EMPTY))
                                tmp_dir = runtime->tmp_dir;
                        else if (runtime->tmp_dir)
                                tmp_dir = strjoina(runtime->tmp_dir, "/tmp");

                        if (streq_ptr(runtime->var_tmp_dir, RUN_SYSTEMD_EMPTY))
                                var_tmp_dir = runtime->var_tmp_dir;
                        else if (runtime->var_tmp_dir)
                                var_tmp_dir = strjoina(runtime->var_tmp_dir, "/tmp");
                }

                ns_info = (NamespaceInfo) {
                        .ignore_protect_paths = false,
                        .private_dev = context->private_devices,
                        .protect_control_groups = context->protect_control_groups,
                        .protect_kernel_tunables = context->protect_kernel_tunables,
                        .protect_kernel_modules = context->protect_kernel_modules,
                        .protect_kernel_logs = context->protect_kernel_logs,
                        .protect_hostname = context->protect_hostname,
                        .mount_apivfs = exec_context_get_effective_mount_apivfs(context),
                        .private_mounts = context->private_mounts,
                        .protect_home = context->protect_home,
                        .protect_system = context->protect_system,
                        .protect_proc = context->protect_proc,
                        .proc_subset = context->proc_subset,
                        .private_ipc = context->private_ipc || context->ipc_namespace_path,
                        /* If NNP is on, we can turn on MS_NOSUID, since it won't have any effect anymore. */
                        .mount_nosuid = context->no_new_privileges && !mac_selinux_use(),
                };
        } else if (!context->dynamic_user && root_dir)
                /*
                 * If DynamicUser=no and RootDirectory= is set then lets pass a relaxed
                 * sandbox info, otherwise enforce it, don't ignore protected paths and
                 * fail if we are enable to apply the sandbox inside the mount namespace.
                 */
                ns_info = (NamespaceInfo) {
                        .ignore_protect_paths = true,
                };
        else
                ns_info = (NamespaceInfo) {};

        if (context->mount_flags == MS_SHARED)
                log_unit_debug(u, "shared mount propagation hidden by other fs namespacing unit settings: ignoring");

        if (exec_context_has_credentials(context) &&
            params->prefix[EXEC_DIRECTORY_RUNTIME] &&
            FLAGS_SET(params->flags, EXEC_WRITE_CREDENTIALS)) {
                creds_path = path_join(params->prefix[EXEC_DIRECTORY_RUNTIME], "credentials", u->id);
                if (!creds_path) {
                        r = -ENOMEM;
                        goto finalize;
                }
        }

        if (MANAGER_IS_SYSTEM(u->manager)) {
                propagate_dir = path_join("/run/systemd/propagate/", u->id);
                if (!propagate_dir) {
                        r = -ENOMEM;
                        goto finalize;
                }

                incoming_dir = strdup("/run/systemd/incoming");
                if (!incoming_dir) {
                        r = -ENOMEM;
                        goto finalize;
                }

                extension_dir = strdup("/run/systemd/unit-extensions");
                if (!extension_dir) {
                        r = -ENOMEM;
                        goto finalize;
                }
        } else
                if (asprintf(&extension_dir, "/run/user/" UID_FMT "/systemd/unit-extensions", geteuid()) < 0) {
                        r = -ENOMEM;
                        goto finalize;
                }

        r = setup_namespace(root_dir, root_image, context->root_image_options,
                            &ns_info, context->read_write_paths,
                            needs_sandboxing ? context->read_only_paths : NULL,
                            needs_sandboxing ? context->inaccessible_paths : NULL,
                            needs_sandboxing ? context->exec_paths : NULL,
                            needs_sandboxing ? context->no_exec_paths : NULL,
                            empty_directories,
                            symlinks,
                            bind_mounts,
                            n_bind_mounts,
                            context->temporary_filesystems,
                            context->n_temporary_filesystems,
                            context->mount_images,
                            context->n_mount_images,
                            tmp_dir,
                            var_tmp_dir,
                            creds_path,
                            context->log_namespace,
                            context->mount_flags,
                            context->root_hash, context->root_hash_size, context->root_hash_path,
                            context->root_hash_sig, context->root_hash_sig_size, context->root_hash_sig_path,
                            context->root_verity,
                            context->extension_images,
                            context->n_extension_images,
                            context->extension_directories,
                            propagate_dir,
                            incoming_dir,
                            extension_dir,
                            root_dir || root_image ? params->notify_socket : NULL,
                            error_path);

        /* If we couldn't set up the namespace this is probably due to a missing capability. setup_namespace() reports
         * that with a special, recognizable error ENOANO. In this case, silently proceed, but only if exclusively
         * sandboxing options were used, i.e. nothing such as RootDirectory= or BindMount= that would result in a
         * completely different execution environment. */
        if (r == -ENOANO) {
                if (insist_on_sandboxing(
                                    context,
                                    root_dir, root_image,
                                    bind_mounts,
                                    n_bind_mounts)) {
                        log_unit_debug(u, "Failed to set up namespace, and refusing to continue since the selected namespacing options alter mount environment non-trivially.\n"
                                       "Bind mounts: %zu, temporary filesystems: %zu, root directory: %s, root image: %s, dynamic user: %s",
                                       n_bind_mounts, context->n_temporary_filesystems, yes_no(root_dir), yes_no(root_image), yes_no(context->dynamic_user));

                        r = -EOPNOTSUPP;
                } else {
                        log_unit_debug(u, "Failed to set up namespace, assuming containerized execution and ignoring.");
                        r = 0;
                }
        }

finalize:
        bind_mount_free_many(bind_mounts, n_bind_mounts);
        return r;
}

static int apply_working_directory(
                const ExecContext *context,
                const ExecParameters *params,
                const char *home,
                int *exit_status) {

        const char *d, *wd;

        assert(context);
        assert(exit_status);

        if (context->working_directory_home) {

                if (!home) {
                        *exit_status = EXIT_CHDIR;
                        return -ENXIO;
                }

                wd = home;

        } else
                wd = empty_to_root(context->working_directory);

        if (params->flags & EXEC_APPLY_CHROOT)
                d = wd;
        else
                d = prefix_roota(context->root_directory, wd);

        if (chdir(d) < 0 && !context->working_directory_missing_ok) {
                *exit_status = EXIT_CHDIR;
                return -errno;
        }

        return 0;
}

static int apply_root_directory(
                const ExecContext *context,
                const ExecParameters *params,
                const bool needs_mount_ns,
                int *exit_status) {

        assert(context);
        assert(exit_status);

        if (params->flags & EXEC_APPLY_CHROOT)
                if (!needs_mount_ns && context->root_directory)
                        if (chroot(context->root_directory) < 0) {
                                *exit_status = EXIT_CHROOT;
                                return -errno;
                        }

        return 0;
}

static int setup_keyring(
                const Unit *u,
                const ExecContext *context,
                const ExecParameters *p,
                uid_t uid, gid_t gid) {

        key_serial_t keyring;
        int r = 0;
        uid_t saved_uid;
        gid_t saved_gid;

        assert(u);
        assert(context);
        assert(p);

        /* Let's set up a new per-service "session" kernel keyring for each system service. This has the benefit that
         * each service runs with its own keyring shared among all processes of the service, but with no hook-up beyond
         * that scope, and in particular no link to the per-UID keyring. If we don't do this the keyring will be
         * automatically created on-demand and then linked to the per-UID keyring, by the kernel. The kernel's built-in
         * on-demand behaviour is very appropriate for login users, but probably not so much for system services, where
         * UIDs are not necessarily specific to a service but reused (at least in the case of UID 0). */

        if (context->keyring_mode == EXEC_KEYRING_INHERIT)
                return 0;

        /* Acquiring a reference to the user keyring is nasty. We briefly change identity in order to get things set up
         * properly by the kernel. If we don't do that then we can't create it atomically, and that sucks for parallel
         * execution. This mimics what pam_keyinit does, too. Setting up session keyring, to be owned by the right user
         * & group is just as nasty as acquiring a reference to the user keyring. */

        saved_uid = getuid();
        saved_gid = getgid();

        if (gid_is_valid(gid) && gid != saved_gid) {
                if (setregid(gid, -1) < 0)
                        return log_unit_error_errno(u, errno, "Failed to change GID for user keyring: %m");
        }

        if (uid_is_valid(uid) && uid != saved_uid) {
                if (setreuid(uid, -1) < 0) {
                        r = log_unit_error_errno(u, errno, "Failed to change UID for user keyring: %m");
                        goto out;
                }
        }

        keyring = keyctl(KEYCTL_JOIN_SESSION_KEYRING, 0, 0, 0, 0);
        if (keyring == -1) {
                if (errno == ENOSYS)
                        log_unit_debug_errno(u, errno, "Kernel keyring not supported, ignoring.");
                else if (ERRNO_IS_PRIVILEGE(errno))
                        log_unit_debug_errno(u, errno, "Kernel keyring access prohibited, ignoring.");
                else if (errno == EDQUOT)
                        log_unit_debug_errno(u, errno, "Out of kernel keyrings to allocate, ignoring.");
                else
                        r = log_unit_error_errno(u, errno, "Setting up kernel keyring failed: %m");

                goto out;
        }

        /* When requested link the user keyring into the session keyring. */
        if (context->keyring_mode == EXEC_KEYRING_SHARED) {

                if (keyctl(KEYCTL_LINK,
                           KEY_SPEC_USER_KEYRING,
                           KEY_SPEC_SESSION_KEYRING, 0, 0) < 0) {
                        r = log_unit_error_errno(u, errno, "Failed to link user keyring into session keyring: %m");
                        goto out;
                }
        }

        /* Restore uid/gid back */
        if (uid_is_valid(uid) && uid != saved_uid) {
                if (setreuid(saved_uid, -1) < 0) {
                        r = log_unit_error_errno(u, errno, "Failed to change UID back for user keyring: %m");
                        goto out;
                }
        }

        if (gid_is_valid(gid) && gid != saved_gid) {
                if (setregid(saved_gid, -1) < 0)
                        return log_unit_error_errno(u, errno, "Failed to change GID back for user keyring: %m");
        }

        /* Populate they keyring with the invocation ID by default, as original saved_uid. */
        if (!sd_id128_is_null(u->invocation_id)) {
                key_serial_t key;

                key = add_key("user", "invocation_id", &u->invocation_id, sizeof(u->invocation_id), KEY_SPEC_SESSION_KEYRING);
                if (key == -1)
                        log_unit_debug_errno(u, errno, "Failed to add invocation ID to keyring, ignoring: %m");
                else {
                        if (keyctl(KEYCTL_SETPERM, key,
                                   KEY_POS_VIEW|KEY_POS_READ|KEY_POS_SEARCH|
                                   KEY_USR_VIEW|KEY_USR_READ|KEY_USR_SEARCH, 0, 0) < 0)
                                r = log_unit_error_errno(u, errno, "Failed to restrict invocation ID permission: %m");
                }
        }

out:
        /* Revert back uid & gid for the last time, and exit */
        /* no extra logging, as only the first already reported error matters */
        if (getuid() != saved_uid)
                (void) setreuid(saved_uid, -1);

        if (getgid() != saved_gid)
                (void) setregid(saved_gid, -1);

        return r;
}

static void append_socket_pair(int *array, size_t *n, const int pair[static 2]) {
        assert(array);
        assert(n);
        assert(pair);

        if (pair[0] >= 0)
                array[(*n)++] = pair[0];
        if (pair[1] >= 0)
                array[(*n)++] = pair[1];
}

static int close_remaining_fds(
                const ExecParameters *params,
                const ExecRuntime *runtime,
                const DynamicCreds *dcreds,
                int user_lookup_fd,
                int socket_fd,
                const int *fds, size_t n_fds) {

        size_t n_dont_close = 0;
        int dont_close[n_fds + 12];

        assert(params);

        if (params->stdin_fd >= 0)
                dont_close[n_dont_close++] = params->stdin_fd;
        if (params->stdout_fd >= 0)
                dont_close[n_dont_close++] = params->stdout_fd;
        if (params->stderr_fd >= 0)
                dont_close[n_dont_close++] = params->stderr_fd;

        if (socket_fd >= 0)
                dont_close[n_dont_close++] = socket_fd;
        if (n_fds > 0) {
                memcpy(dont_close + n_dont_close, fds, sizeof(int) * n_fds);
                n_dont_close += n_fds;
        }

        if (runtime) {
                append_socket_pair(dont_close, &n_dont_close, runtime->netns_storage_socket);
                append_socket_pair(dont_close, &n_dont_close, runtime->ipcns_storage_socket);
        }

        if (dcreds) {
                if (dcreds->user)
                        append_socket_pair(dont_close, &n_dont_close, dcreds->user->storage_socket);
                if (dcreds->group)
                        append_socket_pair(dont_close, &n_dont_close, dcreds->group->storage_socket);
        }

        if (user_lookup_fd >= 0)
                dont_close[n_dont_close++] = user_lookup_fd;

        return close_all_fds(dont_close, n_dont_close);
}

static int send_user_lookup(
                Unit *unit,
                int user_lookup_fd,
                uid_t uid,
                gid_t gid) {

        assert(unit);

        /* Send the resolved UID/GID to PID 1 after we learnt it. We send a single datagram, containing the UID/GID
         * data as well as the unit name. Note that we suppress sending this if no user/group to resolve was
         * specified. */

        if (user_lookup_fd < 0)
                return 0;

        if (!uid_is_valid(uid) && !gid_is_valid(gid))
                return 0;

        if (writev(user_lookup_fd,
               (struct iovec[]) {
                           IOVEC_INIT(&uid, sizeof(uid)),
                           IOVEC_INIT(&gid, sizeof(gid)),
                           IOVEC_INIT_STRING(unit->id) }, 3) < 0)
                return -errno;

        return 0;
}

static int acquire_home(const ExecContext *c, uid_t uid, const char** home, char **buf) {
        int r;

        assert(c);
        assert(home);
        assert(buf);

        /* If WorkingDirectory=~ is set, try to acquire a usable home directory. */

        if (*home)
                return 0;

        if (!c->working_directory_home)
                return 0;

        r = get_home_dir(buf);
        if (r < 0)
                return r;

        *home = *buf;
        return 1;
}

static int compile_suggested_paths(const ExecContext *c, const ExecParameters *p, char ***ret) {
        _cleanup_strv_free_ char ** list = NULL;
        int r;

        assert(c);
        assert(p);
        assert(ret);

        assert(c->dynamic_user);

        /* Compile a list of paths that it might make sense to read the owning UID from to use as initial candidate for
         * dynamic UID allocation, in order to save us from doing costly recursive chown()s of the special
         * directories. */

        for (ExecDirectoryType t = 0; t < _EXEC_DIRECTORY_TYPE_MAX; t++) {
                if (t == EXEC_DIRECTORY_CONFIGURATION)
                        continue;

                if (!p->prefix[t])
                        continue;

                for (size_t i = 0; i < c->directories[t].n_items; i++) {
                        char *e;

                        if (exec_directory_is_private(c, t))
                                e = path_join(p->prefix[t], "private", c->directories[t].items[i].path);
                        else
                                e = path_join(p->prefix[t], c->directories[t].items[i].path);
                        if (!e)
                                return -ENOMEM;

                        r = strv_consume(&list, e);
                        if (r < 0)
                                return r;
                }
        }

        *ret = TAKE_PTR(list);

        return 0;
}

static int exec_parameters_get_cgroup_path(const ExecParameters *params, char **ret) {
        bool using_subcgroup;
        char *p;

        assert(params);
        assert(ret);

        if (!params->cgroup_path)
                return -EINVAL;

        /* If we are called for a unit where cgroup delegation is on, and the payload created its own populated
         * subcgroup (which we expect it to do, after all it asked for delegation), then we cannot place the control
         * processes started after the main unit's process in the unit's main cgroup because it is now an inner one,
         * and inner cgroups may not contain processes. Hence, if delegation is on, and this is a control process,
         * let's use ".control" as subcgroup instead. Note that we do so only for ExecStartPost=, ExecReload=,
         * ExecStop=, ExecStopPost=, i.e. for the commands where the main process is already forked. For ExecStartPre=
         * this is not necessary, the cgroup is still empty. We distinguish these cases with the EXEC_CONTROL_CGROUP
         * flag, which is only passed for the former statements, not for the latter. */

        using_subcgroup = FLAGS_SET(params->flags, EXEC_CONTROL_CGROUP|EXEC_CGROUP_DELEGATE|EXEC_IS_CONTROL);
        if (using_subcgroup)
                p = path_join(params->cgroup_path, ".control");
        else
                p = strdup(params->cgroup_path);
        if (!p)
                return -ENOMEM;

        *ret = p;
        return using_subcgroup;
}

static int exec_context_cpu_affinity_from_numa(const ExecContext *c, CPUSet *ret) {
        _cleanup_(cpu_set_reset) CPUSet s = {};
        int r;

        assert(c);
        assert(ret);

        if (!c->numa_policy.nodes.set) {
                log_debug("Can't derive CPU affinity mask from NUMA mask because NUMA mask is not set, ignoring");
                return 0;
        }

        r = numa_to_cpu_set(&c->numa_policy, &s);
        if (r < 0)
                return r;

        cpu_set_reset(ret);

        return cpu_set_add_all(ret, &s);
}

bool exec_context_get_cpu_affinity_from_numa(const ExecContext *c) {
        assert(c);

        return c->cpu_affinity_from_numa;
}

static int add_shifted_fd(int *fds, size_t fds_size, size_t *n_fds, int fd, int *ret_fd) {
        int r;

        assert(fds);
        assert(n_fds);
        assert(*n_fds < fds_size);
        assert(ret_fd);

        if (fd < 0) {
                *ret_fd = -1;
                return 0;
        }

        if (fd < 3 + (int) *n_fds) {
                /* Let's move the fd up, so that it's outside of the fd range we will use to store
                 * the fds we pass to the process (or which are closed only during execve). */

                r = fcntl(fd, F_DUPFD_CLOEXEC, 3 + (int) *n_fds);
                if (r < 0)
                        return -errno;

                close_and_replace(fd, r);
        }

        *ret_fd = fds[*n_fds] = fd;
        (*n_fds) ++;
        return 1;
}

static int exec_child(
                Unit *unit,
                const ExecCommand *command,
                const ExecContext *context,
                const ExecParameters *params,
                ExecRuntime *runtime,
                DynamicCreds *dcreds,
                int socket_fd,
                const int named_iofds[static 3],
                int *fds,
                size_t n_socket_fds,
                size_t n_storage_fds,
                char **files_env,
                int user_lookup_fd,
                int *exit_status) {

        _cleanup_strv_free_ char **our_env = NULL, **pass_env = NULL, **joined_exec_search_path = NULL, **accum_env = NULL, **replaced_argv = NULL;
        int r, ngids = 0, exec_fd;
        _cleanup_free_ gid_t *supplementary_gids = NULL;
        const char *username = NULL, *groupname = NULL;
        _cleanup_free_ char *home_buffer = NULL;
        const char *home = NULL, *shell = NULL;
        char **final_argv = NULL;
        dev_t journal_stream_dev = 0;
        ino_t journal_stream_ino = 0;
        bool userns_set_up = false;
        bool needs_sandboxing,          /* Do we need to set up full sandboxing? (i.e. all namespacing, all MAC stuff, caps, yadda yadda */
                needs_setuid,           /* Do we need to do the actual setresuid()/setresgid() calls? */
                needs_mount_namespace,  /* Do we need to set up a mount namespace for this kernel? */
                needs_ambient_hack;     /* Do we need to apply the ambient capabilities hack? */
#if HAVE_SELINUX
        _cleanup_free_ char *mac_selinux_context_net = NULL;
        bool use_selinux = false;
#endif
#if ENABLE_SMACK
        bool use_smack = false;
#endif
#if HAVE_APPARMOR
        bool use_apparmor = false;
#endif
        uid_t saved_uid = getuid();
        gid_t saved_gid = getgid();
        uid_t uid = UID_INVALID;
        gid_t gid = GID_INVALID;
        size_t n_fds = n_socket_fds + n_storage_fds, /* fds to pass to the child */
               n_keep_fds; /* total number of fds not to close */
        int secure_bits;
        _cleanup_free_ gid_t *gids_after_pam = NULL;
        int ngids_after_pam = 0;

        assert(unit);
        assert(command);
        assert(context);
        assert(params);
        assert(exit_status);

        /* Explicitly test for CVE-2021-4034 inspired invocations */
        assert(command->path);
        assert(!strv_isempty(command->argv));

        rename_process_from_path(command->path);

        /* We reset exactly these signals, since they are the only ones we set to SIG_IGN in the main
         * daemon. All others we leave untouched because we set them to SIG_DFL or a valid handler initially,
         * both of which will be demoted to SIG_DFL. */
        (void) default_signals(SIGNALS_CRASH_HANDLER,
                               SIGNALS_IGNORE);

        if (context->ignore_sigpipe)
                (void) ignore_signals(SIGPIPE);

        r = reset_signal_mask();
        if (r < 0) {
                *exit_status = EXIT_SIGNAL_MASK;
                return log_unit_error_errno(unit, r, "Failed to set process signal mask: %m");
        }

        if (params->idle_pipe)
                do_idle_pipe_dance(params->idle_pipe);

        /* Close fds we don't need very early to make sure we don't block init reexecution because it cannot bind its
         * sockets. Among the fds we close are the logging fds, and we want to keep them closed, so that we don't have
         * any fds open we don't really want open during the transition. In order to make logging work, we switch the
         * log subsystem into open_when_needed mode, so that it reopens the logs on every single log call. */

        log_forget_fds();
        log_set_open_when_needed(true);
        log_settle_target();

        /* In case anything used libc syslog(), close this here, too */
        closelog();

        int keep_fds[n_fds + 3];
        memcpy_safe(keep_fds, fds, n_fds * sizeof(int));
        n_keep_fds = n_fds;

        r = add_shifted_fd(keep_fds, ELEMENTSOF(keep_fds), &n_keep_fds, params->exec_fd, &exec_fd);
        if (r < 0) {
                *exit_status = EXIT_FDS;
                return log_unit_error_errno(unit, r, "Failed to shift fd and set FD_CLOEXEC: %m");
        }

#if HAVE_LIBBPF
        if (unit->manager->restrict_fs) {
                int bpf_map_fd = lsm_bpf_map_restrict_fs_fd(unit);
                if (bpf_map_fd < 0) {
                        *exit_status = EXIT_FDS;
                        return log_unit_error_errno(unit, bpf_map_fd, "Failed to get restrict filesystems BPF map fd: %m");
                }

                r = add_shifted_fd(keep_fds, ELEMENTSOF(keep_fds), &n_keep_fds, bpf_map_fd, &bpf_map_fd);
                if (r < 0) {
                        *exit_status = EXIT_FDS;
                        return log_unit_error_errno(unit, r, "Failed to shift fd and set FD_CLOEXEC: %m");
                }
        }
#endif

        r = close_remaining_fds(params, runtime, dcreds, user_lookup_fd, socket_fd, keep_fds, n_keep_fds);
        if (r < 0) {
                *exit_status = EXIT_FDS;
                return log_unit_error_errno(unit, r, "Failed to close unwanted file descriptors: %m");
        }

        if (!context->same_pgrp &&
            setsid() < 0) {
                *exit_status = EXIT_SETSID;
                return log_unit_error_errno(unit, errno, "Failed to create new process session: %m");
        }

        exec_context_tty_reset(context, params);

        if (unit_shall_confirm_spawn(unit)) {
                _cleanup_free_ char *cmdline = NULL;

                cmdline = quote_command_line(command->argv, SHELL_ESCAPE_EMPTY);
                if (!cmdline) {
                        *exit_status = EXIT_MEMORY;
                        return log_oom();
                }

                r = ask_for_confirmation(context, params->confirm_spawn, unit, cmdline);
                if (r != CONFIRM_EXECUTE) {
                        if (r == CONFIRM_PRETEND_SUCCESS) {
                                *exit_status = EXIT_SUCCESS;
                                return 0;
                        }

                        *exit_status = EXIT_CONFIRM;
                        return log_unit_error_errno(unit, SYNTHETIC_ERRNO(ECANCELED),
                                                    "Execution cancelled by the user");
                }
        }

        /* We are about to invoke NSS and PAM modules. Let's tell them what we are doing here, maybe they care. This is
         * used by nss-resolve to disable itself when we are about to start systemd-resolved, to avoid deadlocks. Note
         * that these env vars do not survive the execve(), which means they really only apply to the PAM and NSS
         * invocations themselves. Also note that while we'll only invoke NSS modules involved in user management they
         * might internally call into other NSS modules that are involved in hostname resolution, we never know. */
        if (setenv("SYSTEMD_ACTIVATION_UNIT", unit->id, true) != 0 ||
            setenv("SYSTEMD_ACTIVATION_SCOPE", MANAGER_IS_SYSTEM(unit->manager) ? "system" : "user", true) != 0) {
                *exit_status = EXIT_MEMORY;
                return log_unit_error_errno(unit, errno, "Failed to update environment: %m");
        }

        if (context->dynamic_user && dcreds) {
                _cleanup_strv_free_ char **suggested_paths = NULL;

                /* On top of that, make sure we bypass our own NSS module nss-systemd comprehensively for any NSS
                 * checks, if DynamicUser=1 is used, as we shouldn't create a feedback loop with ourselves here. */
                if (putenv((char*) "SYSTEMD_NSS_DYNAMIC_BYPASS=1") != 0) {
                        *exit_status = EXIT_USER;
                        return log_unit_error_errno(unit, errno, "Failed to update environment: %m");
                }

                r = compile_suggested_paths(context, params, &suggested_paths);
                if (r < 0) {
                        *exit_status = EXIT_MEMORY;
                        return log_oom();
                }

                r = dynamic_creds_realize(dcreds, suggested_paths, &uid, &gid);
                if (r < 0) {
                        *exit_status = EXIT_USER;
                        if (r == -EILSEQ)
                                return log_unit_error_errno(unit, SYNTHETIC_ERRNO(EOPNOTSUPP),
                                                            "Failed to update dynamic user credentials: User or group with specified name already exists.");
                        return log_unit_error_errno(unit, r, "Failed to update dynamic user credentials: %m");
                }

                if (!uid_is_valid(uid)) {
                        *exit_status = EXIT_USER;
                        return log_unit_error_errno(unit, SYNTHETIC_ERRNO(ESRCH), "UID validation failed for \""UID_FMT"\"", uid);
                }

                if (!gid_is_valid(gid)) {
                        *exit_status = EXIT_USER;
                        return log_unit_error_errno(unit, SYNTHETIC_ERRNO(ESRCH), "GID validation failed for \""GID_FMT"\"", gid);
                }

                if (dcreds->user)
                        username = dcreds->user->name;

        } else {
                r = get_fixed_user(context, &username, &uid, &gid, &home, &shell);
                if (r < 0) {
                        *exit_status = EXIT_USER;
                        return log_unit_error_errno(unit, r, "Failed to determine user credentials: %m");
                }

                r = get_fixed_group(context, &groupname, &gid);
                if (r < 0) {
                        *exit_status = EXIT_GROUP;
                        return log_unit_error_errno(unit, r, "Failed to determine group credentials: %m");
                }
        }

        /* Initialize user supplementary groups and get SupplementaryGroups= ones */
        r = get_supplementary_groups(context, username, groupname, gid,
                                     &supplementary_gids, &ngids);
        if (r < 0) {
                *exit_status = EXIT_GROUP;
                return log_unit_error_errno(unit, r, "Failed to determine supplementary groups: %m");
        }

        r = send_user_lookup(unit, user_lookup_fd, uid, gid);
        if (r < 0) {
                *exit_status = EXIT_USER;
                return log_unit_error_errno(unit, r, "Failed to send user credentials to PID1: %m");
        }

        user_lookup_fd = safe_close(user_lookup_fd);

        r = acquire_home(context, uid, &home, &home_buffer);
        if (r < 0) {
                *exit_status = EXIT_CHDIR;
                return log_unit_error_errno(unit, r, "Failed to determine $HOME for user: %m");
        }

        /* If a socket is connected to STDIN/STDOUT/STDERR, we
         * must sure to drop O_NONBLOCK */
        if (socket_fd >= 0)
                (void) fd_nonblock(socket_fd, false);

        /* Journald will try to look-up our cgroup in order to populate _SYSTEMD_CGROUP and _SYSTEMD_UNIT fields.
         * Hence we need to migrate to the target cgroup from init.scope before connecting to journald */
        if (params->cgroup_path) {
                _cleanup_free_ char *p = NULL;

                r = exec_parameters_get_cgroup_path(params, &p);
                if (r < 0) {
                        *exit_status = EXIT_CGROUP;
                        return log_unit_error_errno(unit, r, "Failed to acquire cgroup path: %m");
                }

                r = cg_attach_everywhere(params->cgroup_supported, p, 0, NULL, NULL);
                if (r == -EUCLEAN) {
                        *exit_status = EXIT_CGROUP;
                        return log_unit_error_errno(unit, r, "Failed to attach process to cgroup %s "
                                                    "because the cgroup or one of its parents or "
                                                    "siblings is in the threaded mode: %m", p);
                }
                if (r < 0) {
                        *exit_status = EXIT_CGROUP;
                        return log_unit_error_errno(unit, r, "Failed to attach to cgroup %s: %m", p);
                }
        }

        if (context->network_namespace_path && runtime && runtime->netns_storage_socket[0] >= 0) {
                r = open_shareable_ns_path(runtime->netns_storage_socket, context->network_namespace_path, CLONE_NEWNET);
                if (r < 0) {
                        *exit_status = EXIT_NETWORK;
                        return log_unit_error_errno(unit, r, "Failed to open network namespace path %s: %m", context->network_namespace_path);
                }
        }

        if (context->ipc_namespace_path && runtime && runtime->ipcns_storage_socket[0] >= 0) {
                r = open_shareable_ns_path(runtime->ipcns_storage_socket, context->ipc_namespace_path, CLONE_NEWIPC);
                if (r < 0) {
                        *exit_status = EXIT_NAMESPACE;
                        return log_unit_error_errno(unit, r, "Failed to open IPC namespace path %s: %m", context->ipc_namespace_path);
                }
        }

        r = setup_input(context, params, socket_fd, named_iofds);
        if (r < 0) {
                *exit_status = EXIT_STDIN;
                return log_unit_error_errno(unit, r, "Failed to set up standard input: %m");
        }

        r = setup_output(unit, context, params, STDOUT_FILENO, socket_fd, named_iofds, basename(command->path), uid, gid, &journal_stream_dev, &journal_stream_ino);
        if (r < 0) {
                *exit_status = EXIT_STDOUT;
                return log_unit_error_errno(unit, r, "Failed to set up standard output: %m");
        }

        r = setup_output(unit, context, params, STDERR_FILENO, socket_fd, named_iofds, basename(command->path), uid, gid, &journal_stream_dev, &journal_stream_ino);
        if (r < 0) {
                *exit_status = EXIT_STDERR;
                return log_unit_error_errno(unit, r, "Failed to set up standard error output: %m");
        }

        if (context->oom_score_adjust_set) {
                /* When we can't make this change due to EPERM, then let's silently skip over it. User namespaces
                 * prohibit write access to this file, and we shouldn't trip up over that. */
                r = set_oom_score_adjust(context->oom_score_adjust);
                if (ERRNO_IS_PRIVILEGE(r))
                        log_unit_debug_errno(unit, r, "Failed to adjust OOM setting, assuming containerized execution, ignoring: %m");
                else if (r < 0) {
                        *exit_status = EXIT_OOM_ADJUST;
                        return log_unit_error_errno(unit, r, "Failed to adjust OOM setting: %m");
                }
        }

        if (context->coredump_filter_set) {
                r = set_coredump_filter(context->coredump_filter);
                if (ERRNO_IS_PRIVILEGE(r))
                        log_unit_debug_errno(unit, r, "Failed to adjust coredump_filter, ignoring: %m");
                else if (r < 0) {
                        *exit_status = EXIT_LIMITS;
                        return log_unit_error_errno(unit, r, "Failed to adjust coredump_filter: %m");
                }
        }

        if (context->nice_set) {
                r = setpriority_closest(context->nice);
                if (r < 0) {
                        *exit_status = EXIT_NICE;
                        return log_unit_error_errno(unit, r, "Failed to set up process scheduling priority (nice level): %m");
                }
        }

        if (context->cpu_sched_set) {
                struct sched_param param = {
                        .sched_priority = context->cpu_sched_priority,
                };

                r = sched_setscheduler(0,
                                       context->cpu_sched_policy |
                                       (context->cpu_sched_reset_on_fork ?
                                        SCHED_RESET_ON_FORK : 0),
                                       &param);
                if (r < 0) {
                        *exit_status = EXIT_SETSCHEDULER;
                        return log_unit_error_errno(unit, errno, "Failed to set up CPU scheduling: %m");
                }
        }

        if (context->cpu_affinity_from_numa || context->cpu_set.set) {
                _cleanup_(cpu_set_reset) CPUSet converted_cpu_set = {};
                const CPUSet *cpu_set;

                if (context->cpu_affinity_from_numa) {
                        r = exec_context_cpu_affinity_from_numa(context, &converted_cpu_set);
                        if (r < 0) {
                                *exit_status = EXIT_CPUAFFINITY;
                                return log_unit_error_errno(unit, r, "Failed to derive CPU affinity mask from NUMA mask: %m");
                        }

                        cpu_set = &converted_cpu_set;
                } else
                        cpu_set = &context->cpu_set;

                if (sched_setaffinity(0, cpu_set->allocated, cpu_set->set) < 0) {
                        *exit_status = EXIT_CPUAFFINITY;
                        return log_unit_error_errno(unit, errno, "Failed to set up CPU affinity: %m");
                }
        }

        if (mpol_is_valid(numa_policy_get_type(&context->numa_policy))) {
                r = apply_numa_policy(&context->numa_policy);
                if (r == -EOPNOTSUPP)
                        log_unit_debug_errno(unit, r, "NUMA support not available, ignoring.");
                else if (r < 0) {
                        *exit_status = EXIT_NUMA_POLICY;
                        return log_unit_error_errno(unit, r, "Failed to set NUMA memory policy: %m");
                }
        }

        if (context->ioprio_set)
                if (ioprio_set(IOPRIO_WHO_PROCESS, 0, context->ioprio) < 0) {
                        *exit_status = EXIT_IOPRIO;
                        return log_unit_error_errno(unit, errno, "Failed to set up IO scheduling priority: %m");
                }

        if (context->timer_slack_nsec != NSEC_INFINITY)
                if (prctl(PR_SET_TIMERSLACK, context->timer_slack_nsec) < 0) {
                        *exit_status = EXIT_TIMERSLACK;
                        return log_unit_error_errno(unit, errno, "Failed to set up timer slack: %m");
                }

        if (context->personality != PERSONALITY_INVALID) {
                r = safe_personality(context->personality);
                if (r < 0) {
                        *exit_status = EXIT_PERSONALITY;
                        return log_unit_error_errno(unit, r, "Failed to set up execution domain (personality): %m");
                }
        }

        if (context->utmp_id) {
                const char *line = context->tty_path ?
                        (path_startswith(context->tty_path, "/dev/") ?: context->tty_path) :
                        NULL;
                utmp_put_init_process(context->utmp_id, getpid_cached(), getsid(0),
                                      line,
                                      context->utmp_mode == EXEC_UTMP_INIT  ? INIT_PROCESS :
                                      context->utmp_mode == EXEC_UTMP_LOGIN ? LOGIN_PROCESS :
                                      USER_PROCESS,
                                      username);
        }

        if (uid_is_valid(uid)) {
                r = chown_terminal(STDIN_FILENO, uid);
                if (r < 0) {
                        *exit_status = EXIT_STDIN;
                        return log_unit_error_errno(unit, r, "Failed to change ownership of terminal: %m");
                }
        }

        /* If delegation is enabled we'll pass ownership of the cgroup to the user of the new process. On cgroup v1
         * this is only about systemd's own hierarchy, i.e. not the controller hierarchies, simply because that's not
         * safe. On cgroup v2 there's only one hierarchy anyway, and delegation is safe there, hence in that case only
         * touch a single hierarchy too. */
        if (params->cgroup_path && context->user && (params->flags & EXEC_CGROUP_DELEGATE)) {
                r = cg_set_access(SYSTEMD_CGROUP_CONTROLLER, params->cgroup_path, uid, gid);
                if (r < 0) {
                        *exit_status = EXIT_CGROUP;
                        return log_unit_error_errno(unit, r, "Failed to adjust control group access: %m");
                }
        }

        needs_mount_namespace = exec_needs_mount_namespace(context, params, runtime);

        for (ExecDirectoryType dt = 0; dt < _EXEC_DIRECTORY_TYPE_MAX; dt++) {
                r = setup_exec_directory(context, params, uid, gid, dt, needs_mount_namespace, exit_status);
                if (r < 0)
                        return log_unit_error_errno(unit, r, "Failed to set up special execution directory in %s: %m", params->prefix[dt]);
        }

        if (FLAGS_SET(params->flags, EXEC_WRITE_CREDENTIALS)) {
                r = setup_credentials(context, params, unit->id, uid);
                if (r < 0) {
                        *exit_status = EXIT_CREDENTIALS;
                        return log_unit_error_errno(unit, r, "Failed to set up credentials: %m");
                }
        }

        r = build_environment(
                        unit,
                        context,
                        params,
                        n_fds,
                        home,
                        username,
                        shell,
                        journal_stream_dev,
                        journal_stream_ino,
                        &our_env);
        if (r < 0) {
                *exit_status = EXIT_MEMORY;
                return log_oom();
        }

        r = build_pass_environment(context, &pass_env);
        if (r < 0) {
                *exit_status = EXIT_MEMORY;
                return log_oom();
        }

        /* The $PATH variable is set to the default path in params->environment. However, this is overridden
         * if user-specified fields have $PATH set. The intention is to also override $PATH if the unit does
         * not specify PATH but the unit has ExecSearchPath. */
        if (!strv_isempty(context->exec_search_path)) {
                _cleanup_free_ char *joined = NULL;

                joined = strv_join(context->exec_search_path, ":");
                if (!joined) {
                        *exit_status = EXIT_MEMORY;
                        return log_oom();
                }

                r = strv_env_assign(&joined_exec_search_path, "PATH", joined);
                if (r < 0) {
                        *exit_status = EXIT_MEMORY;
                        return log_oom();
                }
        }

        accum_env = strv_env_merge(params->environment,
                                   our_env,
                                   joined_exec_search_path,
                                   pass_env,
                                   context->environment,
                                   files_env);
        if (!accum_env) {
                *exit_status = EXIT_MEMORY;
                return log_oom();
        }
        accum_env = strv_env_clean(accum_env);

        (void) umask(context->umask);

        r = setup_keyring(unit, context, params, uid, gid);
        if (r < 0) {
                *exit_status = EXIT_KEYRING;
                return log_unit_error_errno(unit, r, "Failed to set up kernel keyring: %m");
        }

        /* We need sandboxing if the caller asked us to apply it and the command isn't explicitly excepted
         * from it. */
        needs_sandboxing = (params->flags & EXEC_APPLY_SANDBOXING) && !(command->flags & EXEC_COMMAND_FULLY_PRIVILEGED);

        /* We need the ambient capability hack, if the caller asked us to apply it and the command is marked
         * for it, and the kernel doesn't actually support ambient caps. */
        needs_ambient_hack = (params->flags & EXEC_APPLY_SANDBOXING) && (command->flags & EXEC_COMMAND_AMBIENT_MAGIC) && !ambient_capabilities_supported();

        /* We need setresuid() if the caller asked us to apply sandboxing and the command isn't explicitly
         * excepted from either whole sandboxing or just setresuid() itself, and the ambient hack is not
         * desired. */
        if (needs_ambient_hack)
                needs_setuid = false;
        else
                needs_setuid = (params->flags & EXEC_APPLY_SANDBOXING) && !(command->flags & (EXEC_COMMAND_FULLY_PRIVILEGED|EXEC_COMMAND_NO_SETUID));

        if (needs_sandboxing) {
                /* MAC enablement checks need to be done before a new mount ns is created, as they rely on
                 * /sys being present. The actual MAC context application will happen later, as late as
                 * possible, to avoid impacting our own code paths. */

#if HAVE_SELINUX
                use_selinux = mac_selinux_use();
#endif
#if ENABLE_SMACK
                use_smack = mac_smack_use();
#endif
#if HAVE_APPARMOR
                use_apparmor = mac_apparmor_use();
#endif
        }

        if (needs_sandboxing) {
                int which_failed;

                /* Let's set the resource limits before we call into PAM, so that pam_limits wins over what
                 * is set here. (See below.) */

                r = setrlimit_closest_all((const struct rlimit* const *) context->rlimit, &which_failed);
                if (r < 0) {
                        *exit_status = EXIT_LIMITS;
                        return log_unit_error_errno(unit, r, "Failed to adjust resource limit RLIMIT_%s: %m", rlimit_to_string(which_failed));
                }
        }

        if (needs_setuid && context->pam_name && username) {
                /* Let's call into PAM after we set up our own idea of resource limits to that pam_limits
                 * wins here. (See above.) */

                /* All fds passed in the fds array will be closed in the pam child process. */
                r = setup_pam(context->pam_name, username, uid, gid, context->tty_path, &accum_env, fds, n_fds);
                if (r < 0) {
                        *exit_status = EXIT_PAM;
                        return log_unit_error_errno(unit, r, "Failed to set up PAM session: %m");
                }

                ngids_after_pam = getgroups_alloc(&gids_after_pam);
                if (ngids_after_pam < 0) {
                        *exit_status = EXIT_MEMORY;
                        return log_unit_error_errno(unit, ngids_after_pam, "Failed to obtain groups after setting up PAM: %m");
                }
        }

        if (needs_sandboxing && context->private_users && !have_effective_cap(CAP_SYS_ADMIN)) {
                /* If we're unprivileged, set up the user namespace first to enable use of the other namespaces.
                 * Users with CAP_SYS_ADMIN can set up user namespaces last because they will be able to
                 * set up the all of the other namespaces (i.e. network, mount, UTS) without a user namespace. */

                userns_set_up = true;
                r = setup_private_users(saved_uid, saved_gid, uid, gid);
                if (r < 0) {
                        *exit_status = EXIT_USER;
                        return log_unit_error_errno(unit, r, "Failed to set up user namespacing for unprivileged user: %m");
                }
        }

        if ((context->private_network || context->network_namespace_path) && runtime && runtime->netns_storage_socket[0] >= 0) {

                if (ns_type_supported(NAMESPACE_NET)) {
                        r = setup_shareable_ns(runtime->netns_storage_socket, CLONE_NEWNET);
                        if (r == -EPERM)
                                log_unit_warning_errno(unit, r,
                                                       "PrivateNetwork=yes is configured, but network namespace setup failed, ignoring: %m");
                        else if (r < 0) {
                                *exit_status = EXIT_NETWORK;
                                return log_unit_error_errno(unit, r, "Failed to set up network namespacing: %m");
                        }
                } else if (context->network_namespace_path) {
                        *exit_status = EXIT_NETWORK;
                        return log_unit_error_errno(unit, SYNTHETIC_ERRNO(EOPNOTSUPP),
                                                    "NetworkNamespacePath= is not supported, refusing.");
                } else
                        log_unit_warning(unit, "PrivateNetwork=yes is configured, but the kernel does not support network namespaces, ignoring.");
        }

        if ((context->private_ipc || context->ipc_namespace_path) && runtime && runtime->ipcns_storage_socket[0] >= 0) {

                if (ns_type_supported(NAMESPACE_IPC)) {
                        r = setup_shareable_ns(runtime->ipcns_storage_socket, CLONE_NEWIPC);
                        if (r == -EPERM)
                                log_unit_warning_errno(unit, r,
                                                       "PrivateIPC=yes is configured, but IPC namespace setup failed, ignoring: %m");
                        else if (r < 0) {
                                *exit_status = EXIT_NAMESPACE;
                                return log_unit_error_errno(unit, r, "Failed to set up IPC namespacing: %m");
                        }
                } else if (context->ipc_namespace_path) {
                        *exit_status = EXIT_NAMESPACE;
                        return log_unit_error_errno(unit, SYNTHETIC_ERRNO(EOPNOTSUPP),
                                                    "IPCNamespacePath= is not supported, refusing.");
                } else
                        log_unit_warning(unit, "PrivateIPC=yes is configured, but the kernel does not support IPC namespaces, ignoring.");
        }

        if (needs_mount_namespace) {
                _cleanup_free_ char *error_path = NULL;

                r = apply_mount_namespace(unit, command->flags, context, params, runtime, &error_path);
                if (r < 0) {
                        *exit_status = EXIT_NAMESPACE;
                        return log_unit_error_errno(unit, r, "Failed to set up mount namespacing%s%s: %m",
                                                    error_path ? ": " : "", strempty(error_path));
                }
        }

        if (needs_sandboxing) {
                r = apply_protect_hostname(unit, context, exit_status);
                if (r < 0)
                        return r;
        }

        /* Drop groups as early as possible.
         * This needs to be done after PrivateDevices=y setup as device nodes should be owned by the host's root.
         * For non-root in a userns, devices will be owned by the user/group before the group change, and nobody. */
        if (needs_setuid) {
                _cleanup_free_ gid_t *gids_to_enforce = NULL;
                int ngids_to_enforce = 0;

                ngids_to_enforce = merge_gid_lists(supplementary_gids,
                                                   ngids,
                                                   gids_after_pam,
                                                   ngids_after_pam,
                                                   &gids_to_enforce);
                if (ngids_to_enforce < 0) {
                        *exit_status = EXIT_MEMORY;
                        return log_unit_error_errno(unit,
                                                    ngids_to_enforce,
                                                    "Failed to merge group lists. Group membership might be incorrect: %m");
                }

                r = enforce_groups(gid, gids_to_enforce, ngids_to_enforce);
                if (r < 0) {
                        *exit_status = EXIT_GROUP;
                        return log_unit_error_errno(unit, r, "Changing group credentials failed: %m");
                }
        }

        /* If the user namespace was not set up above, try to do it now.
         * It's preferred to set up the user namespace later (after all other namespaces) so as not to be
         * restricted by rules pertaining to combining user namspaces with other namespaces (e.g. in the
         * case of mount namespaces being less privileged when the mount point list is copied from a
         * different user namespace). */

        if (needs_sandboxing && context->private_users && !userns_set_up) {
                r = setup_private_users(saved_uid, saved_gid, uid, gid);
                if (r < 0) {
                        *exit_status = EXIT_USER;
                        return log_unit_error_errno(unit, r, "Failed to set up user namespacing: %m");
                }
        }

        /* Now that the mount namespace has been set up and privileges adjusted, let's look for the thing we
         * shall execute. */

        _cleanup_free_ char *executable = NULL;
        _cleanup_close_ int executable_fd = -1;
        r = find_executable_full(command->path, /* root= */ NULL, context->exec_search_path, false, &executable, &executable_fd);
        if (r < 0) {
                if (r != -ENOMEM && (command->flags & EXEC_COMMAND_IGNORE_FAILURE)) {
                        log_unit_struct_errno(unit, LOG_INFO, r,
                                              "MESSAGE_ID=" SD_MESSAGE_SPAWN_FAILED_STR,
                                              LOG_UNIT_INVOCATION_ID(unit),
                                              LOG_UNIT_MESSAGE(unit, "Executable %s missing, skipping: %m",
                                                               command->path),
                                              "EXECUTABLE=%s", command->path);
                        *exit_status = EXIT_SUCCESS;
                        return 0;
                }

                *exit_status = EXIT_EXEC;
                return log_unit_struct_errno(unit, LOG_INFO, r,
                                             "MESSAGE_ID=" SD_MESSAGE_SPAWN_FAILED_STR,
                                             LOG_UNIT_INVOCATION_ID(unit),
                                             LOG_UNIT_MESSAGE(unit, "Failed to locate executable %s: %m",
                                                              command->path),
                                             "EXECUTABLE=%s", command->path);
        }

        r = add_shifted_fd(keep_fds, ELEMENTSOF(keep_fds), &n_keep_fds, executable_fd, &executable_fd);
        if (r < 0) {
                *exit_status = EXIT_FDS;
                return log_unit_error_errno(unit, r, "Failed to shift fd and set FD_CLOEXEC: %m");
        }

#if HAVE_SELINUX
        if (needs_sandboxing && use_selinux && params->selinux_context_net) {
                int fd = -1;

                if (socket_fd >= 0)
                        fd = socket_fd;
                else if (params->n_socket_fds == 1)
                        /* If stdin is not connected to a socket but we are triggered by exactly one socket unit then we
                         * use context from that fd to compute the label. */
                        fd = params->fds[0];

                if (fd >= 0) {
                        r = mac_selinux_get_child_mls_label(fd, executable, context->selinux_context, &mac_selinux_context_net);
                        if (r < 0) {
                                if (!context->selinux_context_ignore) {
                                        *exit_status = EXIT_SELINUX_CONTEXT;
                                        return log_unit_error_errno(unit, r, "Failed to determine SELinux context: %m");
                                }
                                log_unit_debug_errno(unit, r, "Failed to determine SELinux context, ignoring: %m");
                        }
                }
        }
#endif

        /* We repeat the fd closing here, to make sure that nothing is leaked from the PAM modules. Note that we are
         * more aggressive this time since socket_fd and the netns and ipcns fds we don't need anymore. We do keep the exec_fd
         * however if we have it as we want to keep it open until the final execve(). */

        r = close_all_fds(keep_fds, n_keep_fds);
        if (r >= 0)
                r = shift_fds(fds, n_fds);
        if (r >= 0)
                r = flags_fds(fds, n_socket_fds, n_storage_fds, context->non_blocking);
        if (r < 0) {
                *exit_status = EXIT_FDS;
                return log_unit_error_errno(unit, r, "Failed to adjust passed file descriptors: %m");
        }

        /* At this point, the fds we want to pass to the program are all ready and set up, with O_CLOEXEC turned off
         * and at the right fd numbers. The are no other fds open, with one exception: the exec_fd if it is defined,
         * and it has O_CLOEXEC set, after all we want it to be closed by the execve(), so that our parent knows we
         * came this far. */

        secure_bits = context->secure_bits;

        if (needs_sandboxing) {
                uint64_t bset;

                /* Set the RTPRIO resource limit to 0, but only if nothing else was explicitly
                 * requested. (Note this is placed after the general resource limit initialization, see
                 * above, in order to take precedence.) */
                if (context->restrict_realtime && !context->rlimit[RLIMIT_RTPRIO]) {
                        if (setrlimit(RLIMIT_RTPRIO, &RLIMIT_MAKE_CONST(0)) < 0) {
                                *exit_status = EXIT_LIMITS;
                                return log_unit_error_errno(unit, errno, "Failed to adjust RLIMIT_RTPRIO resource limit: %m");
                        }
                }

#if ENABLE_SMACK
                /* LSM Smack needs the capability CAP_MAC_ADMIN to change the current execution security context of the
                 * process. This is the latest place before dropping capabilities. Other MAC context are set later. */
                if (use_smack) {
                        r = setup_smack(unit->manager, context, executable_fd);
                        if (r < 0 && !context->smack_process_label_ignore) {
                                *exit_status = EXIT_SMACK_PROCESS_LABEL;
                                return log_unit_error_errno(unit, r, "Failed to set SMACK process label: %m");
                        }
                }
#endif

                bset = context->capability_bounding_set;
                /* If the ambient caps hack is enabled (which means the kernel can't do them, and the user asked for
                 * our magic fallback), then let's add some extra caps, so that the service can drop privs of its own,
                 * instead of us doing that */
                if (needs_ambient_hack)
                        bset |= (UINT64_C(1) << CAP_SETPCAP) |
                                (UINT64_C(1) << CAP_SETUID) |
                                (UINT64_C(1) << CAP_SETGID);

                if (!cap_test_all(bset)) {
                        r = capability_bounding_set_drop(bset, false);
                        if (r < 0) {
                                *exit_status = EXIT_CAPABILITIES;
                                return log_unit_error_errno(unit, r, "Failed to drop capabilities: %m");
                        }
                }

                /* Ambient capabilities are cleared during setresuid() (in enforce_user()) even with
                 * keep-caps set.
                 * To be able to raise the ambient capabilities after setresuid() they have to be
                 * added to the inherited set and keep caps has to be set (done in enforce_user()).
                 * After setresuid() the ambient capabilities can be raised as they are present in
                 * the permitted and inhertiable set. However it is possible that someone wants to
                 * set ambient capabilities without changing the user, so we also set the ambient
                 * capabilities here.
                 * The requested ambient capabilities are raised in the inheritable set if the
                 * second argument is true. */
                if (!needs_ambient_hack) {
                        r = capability_ambient_set_apply(context->capability_ambient_set, true);
                        if (r < 0) {
                                *exit_status = EXIT_CAPABILITIES;
                                return log_unit_error_errno(unit, r, "Failed to apply ambient capabilities (before UID change): %m");
                        }
                }
        }

        /* chroot to root directory first, before we lose the ability to chroot */
        r = apply_root_directory(context, params, needs_mount_namespace, exit_status);
        if (r < 0)
                return log_unit_error_errno(unit, r, "Chrooting to the requested root directory failed: %m");

        if (needs_setuid) {
                if (uid_is_valid(uid)) {
                        r = enforce_user(context, uid);
                        if (r < 0) {
                                *exit_status = EXIT_USER;
                                return log_unit_error_errno(unit, r, "Failed to change UID to " UID_FMT ": %m", uid);
                        }

                        if (!needs_ambient_hack &&
                            context->capability_ambient_set != 0) {

                                /* Raise the ambient capabilities after user change. */
                                r = capability_ambient_set_apply(context->capability_ambient_set, false);
                                if (r < 0) {
                                        *exit_status = EXIT_CAPABILITIES;
                                        return log_unit_error_errno(unit, r, "Failed to apply ambient capabilities (after UID change): %m");
                                }
                        }
                }
        }

        /* Apply working directory here, because the working directory might be on NFS and only the user running
         * this service might have the correct privilege to change to the working directory */
        r = apply_working_directory(context, params, home, exit_status);
        if (r < 0)
                return log_unit_error_errno(unit, r, "Changing to the requested working directory failed: %m");

        if (needs_sandboxing) {
                /* Apply other MAC contexts late, but before seccomp syscall filtering, as those should really be last to
                 * influence our own codepaths as little as possible. Moreover, applying MAC contexts usually requires
                 * syscalls that are subject to seccomp filtering, hence should probably be applied before the syscalls
                 * are restricted. */

#if HAVE_SELINUX
                if (use_selinux) {
                        char *exec_context = mac_selinux_context_net ?: context->selinux_context;

                        if (exec_context) {
                                r = setexeccon(exec_context);
                                if (r < 0) {
                                        if (!context->selinux_context_ignore) {
                                                *exit_status = EXIT_SELINUX_CONTEXT;
                                                return log_unit_error_errno(unit, r, "Failed to change SELinux context to %s: %m", exec_context);
                                        }
                                        log_unit_debug_errno(unit, r, "Failed to change SELinux context to %s, ignoring: %m", exec_context);
                                }
                        }
                }
#endif

#if HAVE_APPARMOR
                if (use_apparmor && context->apparmor_profile) {
                        r = aa_change_onexec(context->apparmor_profile);
                        if (r < 0 && !context->apparmor_profile_ignore) {
                                *exit_status = EXIT_APPARMOR_PROFILE;
                                return log_unit_error_errno(unit, errno, "Failed to prepare AppArmor profile change to %s: %m", context->apparmor_profile);
                        }
                }
#endif

                /* PR_GET_SECUREBITS is not privileged, while PR_SET_SECUREBITS is. So to suppress potential EPERMs
                 * we'll try not to call PR_SET_SECUREBITS unless necessary. Setting securebits requires
                 * CAP_SETPCAP. */
                if (prctl(PR_GET_SECUREBITS) != secure_bits) {
                        /* CAP_SETPCAP is required to set securebits. This capability is raised into the
                         * effective set here.
                         * The effective set is overwritten during execve  with the following  values:
                         * - ambient set (for non-root processes)
                         * - (inheritable | bounding) set for root processes)
                         *
                         * Hence there is no security impact to raise it in the effective set before execve
                         */
                        r = capability_gain_cap_setpcap(NULL);
                        if (r < 0) {
                                *exit_status = EXIT_CAPABILITIES;
                                return log_unit_error_errno(unit, r, "Failed to gain CAP_SETPCAP for setting secure bits");
                        }
                        if (prctl(PR_SET_SECUREBITS, secure_bits) < 0) {
                                *exit_status = EXIT_SECUREBITS;
                                return log_unit_error_errno(unit, errno, "Failed to set process secure bits: %m");
                        }
                }

                if (context_has_no_new_privileges(context))
                        if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
                                *exit_status = EXIT_NO_NEW_PRIVILEGES;
                                return log_unit_error_errno(unit, errno, "Failed to disable new privileges: %m");
                        }

#if HAVE_SECCOMP
                r = apply_address_families(unit, context);
                if (r < 0) {
                        *exit_status = EXIT_ADDRESS_FAMILIES;
                        return log_unit_error_errno(unit, r, "Failed to restrict address families: %m");
                }

                r = apply_memory_deny_write_execute(unit, context);
                if (r < 0) {
                        *exit_status = EXIT_SECCOMP;
                        return log_unit_error_errno(unit, r, "Failed to disable writing to executable memory: %m");
                }

                r = apply_restrict_realtime(unit, context);
                if (r < 0) {
                        *exit_status = EXIT_SECCOMP;
                        return log_unit_error_errno(unit, r, "Failed to apply realtime restrictions: %m");
                }

                r = apply_restrict_suid_sgid(unit, context);
                if (r < 0) {
                        *exit_status = EXIT_SECCOMP;
                        return log_unit_error_errno(unit, r, "Failed to apply SUID/SGID restrictions: %m");
                }

                r = apply_restrict_namespaces(unit, context);
                if (r < 0) {
                        *exit_status = EXIT_SECCOMP;
                        return log_unit_error_errno(unit, r, "Failed to apply namespace restrictions: %m");
                }

                r = apply_protect_sysctl(unit, context);
                if (r < 0) {
                        *exit_status = EXIT_SECCOMP;
                        return log_unit_error_errno(unit, r, "Failed to apply sysctl restrictions: %m");
                }

                r = apply_protect_kernel_modules(unit, context);
                if (r < 0) {
                        *exit_status = EXIT_SECCOMP;
                        return log_unit_error_errno(unit, r, "Failed to apply module loading restrictions: %m");
                }

                r = apply_protect_kernel_logs(unit, context);
                if (r < 0) {
                        *exit_status = EXIT_SECCOMP;
                        return log_unit_error_errno(unit, r, "Failed to apply kernel log restrictions: %m");
                }

                r = apply_protect_clock(unit, context);
                if (r < 0) {
                        *exit_status = EXIT_SECCOMP;
                        return log_unit_error_errno(unit, r, "Failed to apply clock restrictions: %m");
                }

                r = apply_private_devices(unit, context);
                if (r < 0) {
                        *exit_status = EXIT_SECCOMP;
                        return log_unit_error_errno(unit, r, "Failed to set up private devices: %m");
                }

                r = apply_syscall_archs(unit, context);
                if (r < 0) {
                        *exit_status = EXIT_SECCOMP;
                        return log_unit_error_errno(unit, r, "Failed to apply syscall architecture restrictions: %m");
                }

                r = apply_lock_personality(unit, context);
                if (r < 0) {
                        *exit_status = EXIT_SECCOMP;
                        return log_unit_error_errno(unit, r, "Failed to lock personalities: %m");
                }

                r = apply_syscall_log(unit, context);
                if (r < 0) {
                        *exit_status = EXIT_SECCOMP;
                        return log_unit_error_errno(unit, r, "Failed to apply system call log filters: %m");
                }

                /* This really should remain the last step before the execve(), to make sure our own code is unaffected
                 * by the filter as little as possible. */
                r = apply_syscall_filter(unit, context, needs_ambient_hack);
                if (r < 0) {
                        *exit_status = EXIT_SECCOMP;
                        return log_unit_error_errno(unit, r, "Failed to apply system call filters: %m");
                }
#endif

#if HAVE_LIBBPF
                r = apply_restrict_filesystems(unit, context);
                if (r < 0) {
                        *exit_status = EXIT_BPF;
                        return log_unit_error_errno(unit, r, "Failed to restrict filesystems: %m");
                }
#endif

        }

        if (!strv_isempty(context->unset_environment)) {
                char **ee = NULL;

                ee = strv_env_delete(accum_env, 1, context->unset_environment);
                if (!ee) {
                        *exit_status = EXIT_MEMORY;
                        return log_oom();
                }

                strv_free_and_replace(accum_env, ee);
        }

        if (!FLAGS_SET(command->flags, EXEC_COMMAND_NO_ENV_EXPAND)) {
                replaced_argv = replace_env_argv(command->argv, accum_env);
                if (!replaced_argv) {
                        *exit_status = EXIT_MEMORY;
                        return log_oom();
                }
                final_argv = replaced_argv;
        } else
                final_argv = command->argv;

        if (DEBUG_LOGGING) {
                _cleanup_free_ char *line = NULL;

                line = quote_command_line(final_argv, SHELL_ESCAPE_EMPTY);
                if (!line) {
                        *exit_status = EXIT_MEMORY;
                        return log_oom();
                }

                log_unit_struct(unit, LOG_DEBUG,
                                "EXECUTABLE=%s", executable,
                                LOG_UNIT_MESSAGE(unit, "Executing: %s", line));
        }

        if (exec_fd >= 0) {
                uint8_t hot = 1;

                /* We have finished with all our initializations. Let's now let the manager know that. From this point
                 * on, if the manager sees POLLHUP on the exec_fd, then execve() was successful. */

                if (write(exec_fd, &hot, sizeof(hot)) < 0) {
                        *exit_status = EXIT_EXEC;
                        return log_unit_error_errno(unit, errno, "Failed to enable exec_fd: %m");
                }
        }

        r = fexecve_or_execve(executable_fd, executable, final_argv, accum_env);

        if (exec_fd >= 0) {
                uint8_t hot = 0;

                /* The execve() failed. This means the exec_fd is still open. Which means we need to tell the manager
                 * that POLLHUP on it no longer means execve() succeeded. */

                if (write(exec_fd, &hot, sizeof(hot)) < 0) {
                        *exit_status = EXIT_EXEC;
                        return log_unit_error_errno(unit, errno, "Failed to disable exec_fd: %m");
                }
        }

        *exit_status = EXIT_EXEC;
        return log_unit_error_errno(unit, r, "Failed to execute %s: %m", executable);
}

static int exec_context_load_environment(const Unit *unit, const ExecContext *c, char ***l);
static int exec_context_named_iofds(const ExecContext *c, const ExecParameters *p, int named_iofds[static 3]);

int exec_spawn(Unit *unit,
               ExecCommand *command,
               const ExecContext *context,
               const ExecParameters *params,
               ExecRuntime *runtime,
               DynamicCreds *dcreds,
               pid_t *ret) {

        int socket_fd, r, named_iofds[3] = { -1, -1, -1 }, *fds = NULL;
        _cleanup_free_ char *subcgroup_path = NULL;
        _cleanup_strv_free_ char **files_env = NULL;
        size_t n_storage_fds = 0, n_socket_fds = 0;
        _cleanup_free_ char *line = NULL;
        pid_t pid;

        assert(unit);
        assert(command);
        assert(context);
        assert(ret);
        assert(params);
        assert(params->fds || (params->n_socket_fds + params->n_storage_fds <= 0));

        if (context->std_input == EXEC_INPUT_SOCKET ||
            context->std_output == EXEC_OUTPUT_SOCKET ||
            context->std_error == EXEC_OUTPUT_SOCKET) {

                if (params->n_socket_fds > 1)
                        return log_unit_error_errno(unit, SYNTHETIC_ERRNO(EINVAL), "Got more than one socket.");

                if (params->n_socket_fds == 0)
                        return log_unit_error_errno(unit, SYNTHETIC_ERRNO(EINVAL), "Got no socket.");

                socket_fd = params->fds[0];
        } else {
                socket_fd = -1;
                fds = params->fds;
                n_socket_fds = params->n_socket_fds;
                n_storage_fds = params->n_storage_fds;
        }

        r = exec_context_named_iofds(context, params, named_iofds);
        if (r < 0)
                return log_unit_error_errno(unit, r, "Failed to load a named file descriptor: %m");

        r = exec_context_load_environment(unit, context, &files_env);
        if (r < 0)
                return log_unit_error_errno(unit, r, "Failed to load environment files: %m");

        line = quote_command_line(command->argv, SHELL_ESCAPE_EMPTY);
        if (!line)
                return log_oom();

        /* Fork with up-to-date SELinux label database, so the child inherits the up-to-date db
           and, until the next SELinux policy changes, we save further reloads in future children. */
        mac_selinux_maybe_reload();

        log_unit_struct(unit, LOG_DEBUG,
                        LOG_UNIT_MESSAGE(unit, "About to execute %s", line),
                        "EXECUTABLE=%s", command->path, /* We won't know the real executable path until we create
                                                           the mount namespace in the child, but we want to log
                                                           from the parent, so we need to use the (possibly
                                                           inaccurate) path here. */
                        LOG_UNIT_INVOCATION_ID(unit));

        if (params->cgroup_path) {
                r = exec_parameters_get_cgroup_path(params, &subcgroup_path);
                if (r < 0)
                        return log_unit_error_errno(unit, r, "Failed to acquire subcgroup path: %m");
                if (r > 0) { /* We are using a child cgroup */
                        r = cg_create(SYSTEMD_CGROUP_CONTROLLER, subcgroup_path);
                        if (r < 0)
                                return log_unit_error_errno(unit, r, "Failed to create control group '%s': %m", subcgroup_path);

                        /* Normally we would not propagate the oomd xattrs to children but since we created this
                         * sub-cgroup internally we should do it. */
                        cgroup_oomd_xattr_apply(unit, subcgroup_path);
                }
        }

        pid = fork();
        if (pid < 0)
                return log_unit_error_errno(unit, errno, "Failed to fork: %m");

        if (pid == 0) {
                int exit_status;

                r = exec_child(unit,
                               command,
                               context,
                               params,
                               runtime,
                               dcreds,
                               socket_fd,
                               named_iofds,
                               fds,
                               n_socket_fds,
                               n_storage_fds,
                               files_env,
                               unit->manager->user_lookup_fds[1],
                               &exit_status);

                if (r < 0) {
                        const char *status = ASSERT_PTR(
                                        exit_status_to_string(exit_status, EXIT_STATUS_LIBC | EXIT_STATUS_SYSTEMD));

                        log_unit_struct_errno(unit, LOG_ERR, r,
                                              "MESSAGE_ID=" SD_MESSAGE_SPAWN_FAILED_STR,
                                              LOG_UNIT_INVOCATION_ID(unit),
                                              LOG_UNIT_MESSAGE(unit, "Failed at step %s spawning %s: %m",
                                                               status, command->path),
                                              "EXECUTABLE=%s", command->path);
                } else
                        assert(exit_status == EXIT_SUCCESS);

                _exit(exit_status);
        }

        log_unit_debug(unit, "Forked %s as "PID_FMT, command->path, pid);

        /* We add the new process to the cgroup both in the child (so that we can be sure that no user code is ever
         * executed outside of the cgroup) and in the parent (so that we can be sure that when we kill the cgroup the
         * process will be killed too). */
        if (subcgroup_path)
                (void) cg_attach(SYSTEMD_CGROUP_CONTROLLER, subcgroup_path, pid);

        exec_status_start(&command->exec_status, pid);

        *ret = pid;
        return 0;
}

void exec_context_init(ExecContext *c) {
        assert(c);

        c->umask = 0022;
        c->ioprio = IOPRIO_DEFAULT_CLASS_AND_PRIO;
        c->cpu_sched_policy = SCHED_OTHER;
        c->syslog_priority = LOG_DAEMON|LOG_INFO;
        c->syslog_level_prefix = true;
        c->ignore_sigpipe = true;
        c->timer_slack_nsec = NSEC_INFINITY;
        c->personality = PERSONALITY_INVALID;
        for (ExecDirectoryType t = 0; t < _EXEC_DIRECTORY_TYPE_MAX; t++)
                c->directories[t].mode = 0755;
        c->timeout_clean_usec = USEC_INFINITY;
        c->capability_bounding_set = CAP_ALL;
        assert_cc(NAMESPACE_FLAGS_INITIAL != NAMESPACE_FLAGS_ALL);
        c->restrict_namespaces = NAMESPACE_FLAGS_INITIAL;
        c->log_level_max = -1;
#if HAVE_SECCOMP
        c->syscall_errno = SECCOMP_ERROR_NUMBER_KILL;
#endif
        c->tty_rows = UINT_MAX;
        c->tty_cols = UINT_MAX;
        numa_policy_reset(&c->numa_policy);
}

void exec_context_done(ExecContext *c) {
        assert(c);

        c->environment = strv_free(c->environment);
        c->environment_files = strv_free(c->environment_files);
        c->pass_environment = strv_free(c->pass_environment);
        c->unset_environment = strv_free(c->unset_environment);

        rlimit_free_all(c->rlimit);

        for (size_t l = 0; l < 3; l++) {
                c->stdio_fdname[l] = mfree(c->stdio_fdname[l]);
                c->stdio_file[l] = mfree(c->stdio_file[l]);
        }

        c->working_directory = mfree(c->working_directory);
        c->root_directory = mfree(c->root_directory);
        c->root_image = mfree(c->root_image);
        c->root_image_options = mount_options_free_all(c->root_image_options);
        c->root_hash = mfree(c->root_hash);
        c->root_hash_size = 0;
        c->root_hash_path = mfree(c->root_hash_path);
        c->root_hash_sig = mfree(c->root_hash_sig);
        c->root_hash_sig_size = 0;
        c->root_hash_sig_path = mfree(c->root_hash_sig_path);
        c->root_verity = mfree(c->root_verity);
        c->extension_images = mount_image_free_many(c->extension_images, &c->n_extension_images);
        c->extension_directories = strv_free(c->extension_directories);
        c->tty_path = mfree(c->tty_path);
        c->syslog_identifier = mfree(c->syslog_identifier);
        c->user = mfree(c->user);
        c->group = mfree(c->group);

        c->supplementary_groups = strv_free(c->supplementary_groups);

        c->pam_name = mfree(c->pam_name);

        c->read_only_paths = strv_free(c->read_only_paths);
        c->read_write_paths = strv_free(c->read_write_paths);
        c->inaccessible_paths = strv_free(c->inaccessible_paths);
        c->exec_paths = strv_free(c->exec_paths);
        c->no_exec_paths = strv_free(c->no_exec_paths);
        c->exec_search_path = strv_free(c->exec_search_path);

        bind_mount_free_many(c->bind_mounts, c->n_bind_mounts);
        c->bind_mounts = NULL;
        c->n_bind_mounts = 0;
        temporary_filesystem_free_many(c->temporary_filesystems, c->n_temporary_filesystems);
        c->temporary_filesystems = NULL;
        c->n_temporary_filesystems = 0;
        c->mount_images = mount_image_free_many(c->mount_images, &c->n_mount_images);

        cpu_set_reset(&c->cpu_set);
        numa_policy_reset(&c->numa_policy);

        c->utmp_id = mfree(c->utmp_id);
        c->selinux_context = mfree(c->selinux_context);
        c->apparmor_profile = mfree(c->apparmor_profile);
        c->smack_process_label = mfree(c->smack_process_label);

        c->restrict_filesystems = set_free_free(c->restrict_filesystems);

        c->syscall_filter = hashmap_free(c->syscall_filter);
        c->syscall_archs = set_free(c->syscall_archs);
        c->address_families = set_free(c->address_families);

        for (ExecDirectoryType t = 0; t < _EXEC_DIRECTORY_TYPE_MAX; t++)
                exec_directory_done(&c->directories[t]);

        c->log_level_max = -1;

        exec_context_free_log_extra_fields(c);

        c->log_ratelimit_interval_usec = 0;
        c->log_ratelimit_burst = 0;

        c->stdin_data = mfree(c->stdin_data);
        c->stdin_data_size = 0;

        c->network_namespace_path = mfree(c->network_namespace_path);
        c->ipc_namespace_path = mfree(c->ipc_namespace_path);

        c->log_namespace = mfree(c->log_namespace);

        c->load_credentials = hashmap_free(c->load_credentials);
        c->set_credentials = hashmap_free(c->set_credentials);
}

int exec_context_destroy_runtime_directory(const ExecContext *c, const char *runtime_prefix) {
        assert(c);

        if (!runtime_prefix)
                return 0;

        for (size_t i = 0; i < c->directories[EXEC_DIRECTORY_RUNTIME].n_items; i++) {
                _cleanup_free_ char *p = NULL;

                if (exec_directory_is_private(c, EXEC_DIRECTORY_RUNTIME))
                        p = path_join(runtime_prefix, "private", c->directories[EXEC_DIRECTORY_RUNTIME].items[i].path);
                else
                        p = path_join(runtime_prefix, c->directories[EXEC_DIRECTORY_RUNTIME].items[i].path);
                if (!p)
                        return -ENOMEM;

                /* We execute this synchronously, since we need to be sure this is gone when we start the
                 * service next. */
                (void) rm_rf(p, REMOVE_ROOT);

                STRV_FOREACH(symlink, c->directories[EXEC_DIRECTORY_RUNTIME].items[i].symlinks) {
                        _cleanup_free_ char *symlink_abs = NULL;

                        if (exec_directory_is_private(c, EXEC_DIRECTORY_RUNTIME))
                                symlink_abs = path_join(runtime_prefix, "private", *symlink);
                        else
                                symlink_abs = path_join(runtime_prefix, *symlink);
                        if (!symlink_abs)
                                return -ENOMEM;

                        (void) unlink(symlink_abs);
                }

        }

        return 0;
}

int exec_context_destroy_credentials(const ExecContext *c, const char *runtime_prefix, const char *unit) {
        _cleanup_free_ char *p = NULL;

        assert(c);

        if (!runtime_prefix || !unit)
                return 0;

        p = path_join(runtime_prefix, "credentials", unit);
        if (!p)
                return -ENOMEM;

        /* This is either a tmpfs/ramfs of its own, or a plain directory. Either way, let's first try to
         * unmount it, and afterwards remove the mount point */
        (void) umount2(p, MNT_DETACH|UMOUNT_NOFOLLOW);
        (void) rm_rf(p, REMOVE_ROOT|REMOVE_CHMOD);

        return 0;
}

int exec_context_destroy_mount_ns_dir(Unit *u) {
        _cleanup_free_ char *p = NULL;

        if (!u || !MANAGER_IS_SYSTEM(u->manager))
                return 0;

        p = path_join("/run/systemd/propagate/", u->id);
        if (!p)
                return -ENOMEM;

        /* This is only filled transiently (see mount_in_namespace()), should be empty or even non-existent*/
        if (rmdir(p) < 0 && errno != ENOENT)
                log_unit_debug_errno(u, errno, "Unable to remove propagation dir '%s', ignoring: %m", p);

        return 0;
}

static void exec_command_done(ExecCommand *c) {
        assert(c);

        c->path = mfree(c->path);
        c->argv = strv_free(c->argv);
}

void exec_command_done_array(ExecCommand *c, size_t n) {
        for (size_t i = 0; i < n; i++)
                exec_command_done(c+i);
}

ExecCommand* exec_command_free_list(ExecCommand *c) {
        ExecCommand *i;

        while ((i = c)) {
                LIST_REMOVE(command, c, i);
                exec_command_done(i);
                free(i);
        }

        return NULL;
}

void exec_command_free_array(ExecCommand **c, size_t n) {
        for (size_t i = 0; i < n; i++)
                c[i] = exec_command_free_list(c[i]);
}

void exec_command_reset_status_array(ExecCommand *c, size_t n) {
        for (size_t i = 0; i < n; i++)
                exec_status_reset(&c[i].exec_status);
}

void exec_command_reset_status_list_array(ExecCommand **c, size_t n) {
        for (size_t i = 0; i < n; i++)
                LIST_FOREACH(command, z, c[i])
                        exec_status_reset(&z->exec_status);
}

typedef struct InvalidEnvInfo {
        const Unit *unit;
        const char *path;
} InvalidEnvInfo;

static void invalid_env(const char *p, void *userdata) {
        InvalidEnvInfo *info = userdata;

        log_unit_error(info->unit, "Ignoring invalid environment assignment '%s': %s", p, info->path);
}

const char* exec_context_fdname(const ExecContext *c, int fd_index) {
        assert(c);

        switch (fd_index) {

        case STDIN_FILENO:
                if (c->std_input != EXEC_INPUT_NAMED_FD)
                        return NULL;

                return c->stdio_fdname[STDIN_FILENO] ?: "stdin";

        case STDOUT_FILENO:
                if (c->std_output != EXEC_OUTPUT_NAMED_FD)
                        return NULL;

                return c->stdio_fdname[STDOUT_FILENO] ?: "stdout";

        case STDERR_FILENO:
                if (c->std_error != EXEC_OUTPUT_NAMED_FD)
                        return NULL;

                return c->stdio_fdname[STDERR_FILENO] ?: "stderr";

        default:
                return NULL;
        }
}

static int exec_context_named_iofds(
                const ExecContext *c,
                const ExecParameters *p,
                int named_iofds[static 3]) {

        size_t targets;
        const char* stdio_fdname[3];
        size_t n_fds;

        assert(c);
        assert(p);
        assert(named_iofds);

        targets = (c->std_input == EXEC_INPUT_NAMED_FD) +
                  (c->std_output == EXEC_OUTPUT_NAMED_FD) +
                  (c->std_error == EXEC_OUTPUT_NAMED_FD);

        for (size_t i = 0; i < 3; i++)
                stdio_fdname[i] = exec_context_fdname(c, i);

        n_fds = p->n_storage_fds + p->n_socket_fds;

        for (size_t i = 0; i < n_fds  && targets > 0; i++)
                if (named_iofds[STDIN_FILENO] < 0 &&
                    c->std_input == EXEC_INPUT_NAMED_FD &&
                    stdio_fdname[STDIN_FILENO] &&
                    streq(p->fd_names[i], stdio_fdname[STDIN_FILENO])) {

                        named_iofds[STDIN_FILENO] = p->fds[i];
                        targets--;

                } else if (named_iofds[STDOUT_FILENO] < 0 &&
                           c->std_output == EXEC_OUTPUT_NAMED_FD &&
                           stdio_fdname[STDOUT_FILENO] &&
                           streq(p->fd_names[i], stdio_fdname[STDOUT_FILENO])) {

                        named_iofds[STDOUT_FILENO] = p->fds[i];
                        targets--;

                } else if (named_iofds[STDERR_FILENO] < 0 &&
                           c->std_error == EXEC_OUTPUT_NAMED_FD &&
                           stdio_fdname[STDERR_FILENO] &&
                           streq(p->fd_names[i], stdio_fdname[STDERR_FILENO])) {

                        named_iofds[STDERR_FILENO] = p->fds[i];
                        targets--;
                }

        return targets == 0 ? 0 : -ENOENT;
}

static int exec_context_load_environment(const Unit *unit, const ExecContext *c, char ***ret) {
        _cleanup_strv_free_ char **v = NULL;
        int r;

        assert(c);
        assert(ret);

        STRV_FOREACH(i, c->environment_files) {
                _cleanup_globfree_ glob_t pglob = {};
                bool ignore = false;
                char *fn = *i;

                if (fn[0] == '-') {
                        ignore = true;
                        fn++;
                }

                if (!path_is_absolute(fn)) {
                        if (ignore)
                                continue;
                        return -EINVAL;
                }

                /* Filename supports globbing, take all matching files */
                r = safe_glob(fn, 0, &pglob);
                if (r < 0) {
                        if (ignore)
                                continue;
                        return r;
                }

                /* When we don't match anything, -ENOENT should be returned */
                assert(pglob.gl_pathc > 0);

                for (unsigned n = 0; n < pglob.gl_pathc; n++) {
                        _cleanup_strv_free_ char **p = NULL;

                        r = load_env_file(NULL, pglob.gl_pathv[n], &p);
                        if (r < 0) {
                                if (ignore)
                                        continue;
                                return r;
                        }

                        /* Log invalid environment variables with filename */
                        if (p) {
                                InvalidEnvInfo info = {
                                        .unit = unit,
                                        .path = pglob.gl_pathv[n]
                                };

                                p = strv_env_clean_with_callback(p, invalid_env, &info);
                        }

                        if (!v)
                                v = TAKE_PTR(p);
                        else {
                                char **m = strv_env_merge(v, p);
                                if (!m)
                                        return -ENOMEM;

                                strv_free_and_replace(v, m);
                        }
                }
        }

        *ret = TAKE_PTR(v);

        return 0;
}

static bool tty_may_match_dev_console(const char *tty) {
        _cleanup_free_ char *resolved = NULL;

        if (!tty)
                return true;

        tty = skip_dev_prefix(tty);

        /* trivial identity? */
        if (streq(tty, "console"))
                return true;

        if (resolve_dev_console(&resolved) < 0)
                return true; /* if we could not resolve, assume it may */

        /* "tty0" means the active VC, so it may be the same sometimes */
        return path_equal(resolved, tty) || (streq(resolved, "tty0") && tty_is_vc(tty));
}

static bool exec_context_may_touch_tty(const ExecContext *ec) {
        assert(ec);

        return ec->tty_reset ||
                ec->tty_vhangup ||
                ec->tty_vt_disallocate ||
                is_terminal_input(ec->std_input) ||
                is_terminal_output(ec->std_output) ||
                is_terminal_output(ec->std_error);
}

bool exec_context_may_touch_console(const ExecContext *ec) {

        return exec_context_may_touch_tty(ec) &&
               tty_may_match_dev_console(exec_context_tty_path(ec));
}

static void strv_fprintf(FILE *f, char **l) {
        assert(f);

        STRV_FOREACH(g, l)
                fprintf(f, " %s", *g);
}

static void strv_dump(FILE* f, const char *prefix, const char *name, char **strv) {
        assert(f);
        assert(prefix);
        assert(name);

        if (!strv_isempty(strv)) {
                fprintf(f, "%s%s:", prefix, name);
                strv_fprintf(f, strv);
                fputs("\n", f);
        }
}

void exec_context_dump(const ExecContext *c, FILE* f, const char *prefix) {
        int r;

        assert(c);
        assert(f);

        prefix = strempty(prefix);

        fprintf(f,
                "%sUMask: %04o\n"
                "%sWorkingDirectory: %s\n"
                "%sRootDirectory: %s\n"
                "%sNonBlocking: %s\n"
                "%sPrivateTmp: %s\n"
                "%sPrivateDevices: %s\n"
                "%sProtectKernelTunables: %s\n"
                "%sProtectKernelModules: %s\n"
                "%sProtectKernelLogs: %s\n"
                "%sProtectClock: %s\n"
                "%sProtectControlGroups: %s\n"
                "%sPrivateNetwork: %s\n"
                "%sPrivateUsers: %s\n"
                "%sProtectHome: %s\n"
                "%sProtectSystem: %s\n"
                "%sMountAPIVFS: %s\n"
                "%sIgnoreSIGPIPE: %s\n"
                "%sMemoryDenyWriteExecute: %s\n"
                "%sRestrictRealtime: %s\n"
                "%sRestrictSUIDSGID: %s\n"
                "%sKeyringMode: %s\n"
                "%sProtectHostname: %s\n"
                "%sProtectProc: %s\n"
                "%sProcSubset: %s\n",
                prefix, c->umask,
                prefix, empty_to_root(c->working_directory),
                prefix, empty_to_root(c->root_directory),
                prefix, yes_no(c->non_blocking),
                prefix, yes_no(c->private_tmp),
                prefix, yes_no(c->private_devices),
                prefix, yes_no(c->protect_kernel_tunables),
                prefix, yes_no(c->protect_kernel_modules),
                prefix, yes_no(c->protect_kernel_logs),
                prefix, yes_no(c->protect_clock),
                prefix, yes_no(c->protect_control_groups),
                prefix, yes_no(c->private_network),
                prefix, yes_no(c->private_users),
                prefix, protect_home_to_string(c->protect_home),
                prefix, protect_system_to_string(c->protect_system),
                prefix, yes_no(exec_context_get_effective_mount_apivfs(c)),
                prefix, yes_no(c->ignore_sigpipe),
                prefix, yes_no(c->memory_deny_write_execute),
                prefix, yes_no(c->restrict_realtime),
                prefix, yes_no(c->restrict_suid_sgid),
                prefix, exec_keyring_mode_to_string(c->keyring_mode),
                prefix, yes_no(c->protect_hostname),
                prefix, protect_proc_to_string(c->protect_proc),
                prefix, proc_subset_to_string(c->proc_subset));

        if (c->root_image)
                fprintf(f, "%sRootImage: %s\n", prefix, c->root_image);

        if (c->root_image_options) {
                fprintf(f, "%sRootImageOptions:", prefix);
                LIST_FOREACH(mount_options, o, c->root_image_options)
                        if (!isempty(o->options))
                                fprintf(f, " %s:%s",
                                        partition_designator_to_string(o->partition_designator),
                                        o->options);
                fprintf(f, "\n");
        }

        if (c->root_hash) {
                _cleanup_free_ char *encoded = NULL;
                encoded = hexmem(c->root_hash, c->root_hash_size);
                if (encoded)
                        fprintf(f, "%sRootHash: %s\n", prefix, encoded);
        }

        if (c->root_hash_path)
                fprintf(f, "%sRootHash: %s\n", prefix, c->root_hash_path);

        if (c->root_hash_sig) {
                _cleanup_free_ char *encoded = NULL;
                ssize_t len;
                len = base64mem(c->root_hash_sig, c->root_hash_sig_size, &encoded);
                if (len)
                        fprintf(f, "%sRootHashSignature: base64:%s\n", prefix, encoded);
        }

        if (c->root_hash_sig_path)
                fprintf(f, "%sRootHashSignature: %s\n", prefix, c->root_hash_sig_path);

        if (c->root_verity)
                fprintf(f, "%sRootVerity: %s\n", prefix, c->root_verity);

        STRV_FOREACH(e, c->environment)
                fprintf(f, "%sEnvironment: %s\n", prefix, *e);

        STRV_FOREACH(e, c->environment_files)
                fprintf(f, "%sEnvironmentFile: %s\n", prefix, *e);

        STRV_FOREACH(e, c->pass_environment)
                fprintf(f, "%sPassEnvironment: %s\n", prefix, *e);

        STRV_FOREACH(e, c->unset_environment)
                fprintf(f, "%sUnsetEnvironment: %s\n", prefix, *e);

        fprintf(f, "%sRuntimeDirectoryPreserve: %s\n", prefix, exec_preserve_mode_to_string(c->runtime_directory_preserve_mode));

        for (ExecDirectoryType dt = 0; dt < _EXEC_DIRECTORY_TYPE_MAX; dt++) {
                fprintf(f, "%s%sMode: %04o\n", prefix, exec_directory_type_to_string(dt), c->directories[dt].mode);

                for (size_t i = 0; i < c->directories[dt].n_items; i++) {
                        fprintf(f, "%s%s: %s\n", prefix, exec_directory_type_to_string(dt), c->directories[dt].items[i].path);

                        STRV_FOREACH(d, c->directories[dt].items[i].symlinks)
                                fprintf(f, "%s%s: %s:%s\n", prefix, exec_directory_type_symlink_to_string(dt), c->directories[dt].items[i].path, *d);
                }
        }

        fprintf(f, "%sTimeoutCleanSec: %s\n", prefix, FORMAT_TIMESPAN(c->timeout_clean_usec, USEC_PER_SEC));

        if (c->nice_set)
                fprintf(f, "%sNice: %i\n", prefix, c->nice);

        if (c->oom_score_adjust_set)
                fprintf(f, "%sOOMScoreAdjust: %i\n", prefix, c->oom_score_adjust);

        if (c->coredump_filter_set)
                fprintf(f, "%sCoredumpFilter: 0x%"PRIx64"\n", prefix, c->coredump_filter);

        for (unsigned i = 0; i < RLIM_NLIMITS; i++)
                if (c->rlimit[i]) {
                        fprintf(f, "%sLimit%s: " RLIM_FMT "\n",
                                prefix, rlimit_to_string(i), c->rlimit[i]->rlim_max);
                        fprintf(f, "%sLimit%sSoft: " RLIM_FMT "\n",
                                prefix, rlimit_to_string(i), c->rlimit[i]->rlim_cur);
                }

        if (c->ioprio_set) {
                _cleanup_free_ char *class_str = NULL;

                r = ioprio_class_to_string_alloc(ioprio_prio_class(c->ioprio), &class_str);
                if (r >= 0)
                        fprintf(f, "%sIOSchedulingClass: %s\n", prefix, class_str);

                fprintf(f, "%sIOPriority: %d\n", prefix, ioprio_prio_data(c->ioprio));
        }

        if (c->cpu_sched_set) {
                _cleanup_free_ char *policy_str = NULL;

                r = sched_policy_to_string_alloc(c->cpu_sched_policy, &policy_str);
                if (r >= 0)
                        fprintf(f, "%sCPUSchedulingPolicy: %s\n", prefix, policy_str);

                fprintf(f,
                        "%sCPUSchedulingPriority: %i\n"
                        "%sCPUSchedulingResetOnFork: %s\n",
                        prefix, c->cpu_sched_priority,
                        prefix, yes_no(c->cpu_sched_reset_on_fork));
        }

        if (c->cpu_set.set) {
                _cleanup_free_ char *affinity = NULL;

                affinity = cpu_set_to_range_string(&c->cpu_set);
                fprintf(f, "%sCPUAffinity: %s\n", prefix, affinity);
        }

        if (mpol_is_valid(numa_policy_get_type(&c->numa_policy))) {
                _cleanup_free_ char *nodes = NULL;

                nodes = cpu_set_to_range_string(&c->numa_policy.nodes);
                fprintf(f, "%sNUMAPolicy: %s\n", prefix, mpol_to_string(numa_policy_get_type(&c->numa_policy)));
                fprintf(f, "%sNUMAMask: %s\n", prefix, strnull(nodes));
        }

        if (c->timer_slack_nsec != NSEC_INFINITY)
                fprintf(f, "%sTimerSlackNSec: "NSEC_FMT "\n", prefix, c->timer_slack_nsec);

        fprintf(f,
                "%sStandardInput: %s\n"
                "%sStandardOutput: %s\n"
                "%sStandardError: %s\n",
                prefix, exec_input_to_string(c->std_input),
                prefix, exec_output_to_string(c->std_output),
                prefix, exec_output_to_string(c->std_error));

        if (c->std_input == EXEC_INPUT_NAMED_FD)
                fprintf(f, "%sStandardInputFileDescriptorName: %s\n", prefix, c->stdio_fdname[STDIN_FILENO]);
        if (c->std_output == EXEC_OUTPUT_NAMED_FD)
                fprintf(f, "%sStandardOutputFileDescriptorName: %s\n", prefix, c->stdio_fdname[STDOUT_FILENO]);
        if (c->std_error == EXEC_OUTPUT_NAMED_FD)
                fprintf(f, "%sStandardErrorFileDescriptorName: %s\n", prefix, c->stdio_fdname[STDERR_FILENO]);

        if (c->std_input == EXEC_INPUT_FILE)
                fprintf(f, "%sStandardInputFile: %s\n", prefix, c->stdio_file[STDIN_FILENO]);
        if (c->std_output == EXEC_OUTPUT_FILE)
                fprintf(f, "%sStandardOutputFile: %s\n", prefix, c->stdio_file[STDOUT_FILENO]);
        if (c->std_output == EXEC_OUTPUT_FILE_APPEND)
                fprintf(f, "%sStandardOutputFileToAppend: %s\n", prefix, c->stdio_file[STDOUT_FILENO]);
        if (c->std_output == EXEC_OUTPUT_FILE_TRUNCATE)
                fprintf(f, "%sStandardOutputFileToTruncate: %s\n", prefix, c->stdio_file[STDOUT_FILENO]);
        if (c->std_error == EXEC_OUTPUT_FILE)
                fprintf(f, "%sStandardErrorFile: %s\n", prefix, c->stdio_file[STDERR_FILENO]);
        if (c->std_error == EXEC_OUTPUT_FILE_APPEND)
                fprintf(f, "%sStandardErrorFileToAppend: %s\n", prefix, c->stdio_file[STDERR_FILENO]);
        if (c->std_error == EXEC_OUTPUT_FILE_TRUNCATE)
                fprintf(f, "%sStandardErrorFileToTruncate: %s\n", prefix, c->stdio_file[STDERR_FILENO]);

        if (c->tty_path)
                fprintf(f,
                        "%sTTYPath: %s\n"
                        "%sTTYReset: %s\n"
                        "%sTTYVHangup: %s\n"
                        "%sTTYVTDisallocate: %s\n"
                        "%sTTYRows: %u\n"
                        "%sTTYColumns: %u\n",
                        prefix, c->tty_path,
                        prefix, yes_no(c->tty_reset),
                        prefix, yes_no(c->tty_vhangup),
                        prefix, yes_no(c->tty_vt_disallocate),
                        prefix, c->tty_rows,
                        prefix, c->tty_cols);

        if (IN_SET(c->std_output,
                   EXEC_OUTPUT_KMSG,
                   EXEC_OUTPUT_JOURNAL,
                   EXEC_OUTPUT_KMSG_AND_CONSOLE,
                   EXEC_OUTPUT_JOURNAL_AND_CONSOLE) ||
            IN_SET(c->std_error,
                   EXEC_OUTPUT_KMSG,
                   EXEC_OUTPUT_JOURNAL,
                   EXEC_OUTPUT_KMSG_AND_CONSOLE,
                   EXEC_OUTPUT_JOURNAL_AND_CONSOLE)) {

                _cleanup_free_ char *fac_str = NULL, *lvl_str = NULL;

                r = log_facility_unshifted_to_string_alloc(c->syslog_priority >> 3, &fac_str);
                if (r >= 0)
                        fprintf(f, "%sSyslogFacility: %s\n", prefix, fac_str);

                r = log_level_to_string_alloc(LOG_PRI(c->syslog_priority), &lvl_str);
                if (r >= 0)
                        fprintf(f, "%sSyslogLevel: %s\n", prefix, lvl_str);
        }

        if (c->log_level_max >= 0) {
                _cleanup_free_ char *t = NULL;

                (void) log_level_to_string_alloc(c->log_level_max, &t);

                fprintf(f, "%sLogLevelMax: %s\n", prefix, strna(t));
        }

        if (c->log_ratelimit_interval_usec > 0)
                fprintf(f,
                        "%sLogRateLimitIntervalSec: %s\n",
                        prefix, FORMAT_TIMESPAN(c->log_ratelimit_interval_usec, USEC_PER_SEC));

        if (c->log_ratelimit_burst > 0)
                fprintf(f, "%sLogRateLimitBurst: %u\n", prefix, c->log_ratelimit_burst);

        for (size_t j = 0; j < c->n_log_extra_fields; j++) {
                fprintf(f, "%sLogExtraFields: ", prefix);
                fwrite(c->log_extra_fields[j].iov_base,
                       1, c->log_extra_fields[j].iov_len,
                       f);
                fputc('\n', f);
        }

        if (c->log_namespace)
                fprintf(f, "%sLogNamespace: %s\n", prefix, c->log_namespace);

        if (c->secure_bits) {
                _cleanup_free_ char *str = NULL;

                r = secure_bits_to_string_alloc(c->secure_bits, &str);
                if (r >= 0)
                        fprintf(f, "%sSecure Bits: %s\n", prefix, str);
        }

        if (c->capability_bounding_set != CAP_ALL) {
                _cleanup_free_ char *str = NULL;

                r = capability_set_to_string_alloc(c->capability_bounding_set, &str);
                if (r >= 0)
                        fprintf(f, "%sCapabilityBoundingSet: %s\n", prefix, str);
        }

        if (c->capability_ambient_set != 0) {
                _cleanup_free_ char *str = NULL;

                r = capability_set_to_string_alloc(c->capability_ambient_set, &str);
                if (r >= 0)
                        fprintf(f, "%sAmbientCapabilities: %s\n", prefix, str);
        }

        if (c->user)
                fprintf(f, "%sUser: %s\n", prefix, c->user);
        if (c->group)
                fprintf(f, "%sGroup: %s\n", prefix, c->group);

        fprintf(f, "%sDynamicUser: %s\n", prefix, yes_no(c->dynamic_user));

        strv_dump(f, prefix, "SupplementaryGroups", c->supplementary_groups);

        if (c->pam_name)
                fprintf(f, "%sPAMName: %s\n", prefix, c->pam_name);

        strv_dump(f, prefix, "ReadWritePaths", c->read_write_paths);
        strv_dump(f, prefix, "ReadOnlyPaths", c->read_only_paths);
        strv_dump(f, prefix, "InaccessiblePaths", c->inaccessible_paths);
        strv_dump(f, prefix, "ExecPaths", c->exec_paths);
        strv_dump(f, prefix, "NoExecPaths", c->no_exec_paths);
        strv_dump(f, prefix, "ExecSearchPath", c->exec_search_path);

        for (size_t i = 0; i < c->n_bind_mounts; i++)
                fprintf(f, "%s%s: %s%s:%s:%s\n", prefix,
                        c->bind_mounts[i].read_only ? "BindReadOnlyPaths" : "BindPaths",
                        c->bind_mounts[i].ignore_enoent ? "-": "",
                        c->bind_mounts[i].source,
                        c->bind_mounts[i].destination,
                        c->bind_mounts[i].recursive ? "rbind" : "norbind");

        for (size_t i = 0; i < c->n_temporary_filesystems; i++) {
                const TemporaryFileSystem *t = c->temporary_filesystems + i;

                fprintf(f, "%sTemporaryFileSystem: %s%s%s\n", prefix,
                        t->path,
                        isempty(t->options) ? "" : ":",
                        strempty(t->options));
        }

        if (c->utmp_id)
                fprintf(f,
                        "%sUtmpIdentifier: %s\n",
                        prefix, c->utmp_id);

        if (c->selinux_context)
                fprintf(f,
                        "%sSELinuxContext: %s%s\n",
                        prefix, c->selinux_context_ignore ? "-" : "", c->selinux_context);

        if (c->apparmor_profile)
                fprintf(f,
                        "%sAppArmorProfile: %s%s\n",
                        prefix, c->apparmor_profile_ignore ? "-" : "", c->apparmor_profile);

        if (c->smack_process_label)
                fprintf(f,
                        "%sSmackProcessLabel: %s%s\n",
                        prefix, c->smack_process_label_ignore ? "-" : "", c->smack_process_label);

        if (c->personality != PERSONALITY_INVALID)
                fprintf(f,
                        "%sPersonality: %s\n",
                        prefix, strna(personality_to_string(c->personality)));

        fprintf(f,
                "%sLockPersonality: %s\n",
                prefix, yes_no(c->lock_personality));

        if (c->syscall_filter) {
                fprintf(f,
                        "%sSystemCallFilter: ",
                        prefix);

                if (!c->syscall_allow_list)
                        fputc('~', f);

#if HAVE_SECCOMP
                void *id, *val;
                bool first = true;
                HASHMAP_FOREACH_KEY(val, id, c->syscall_filter) {
                        _cleanup_free_ char *name = NULL;
                        const char *errno_name = NULL;
                        int num = PTR_TO_INT(val);

                        if (first)
                                first = false;
                        else
                                fputc(' ', f);

                        name = seccomp_syscall_resolve_num_arch(SCMP_ARCH_NATIVE, PTR_TO_INT(id) - 1);
                        fputs(strna(name), f);

                        if (num >= 0) {
                                errno_name = seccomp_errno_or_action_to_string(num);
                                if (errno_name)
                                        fprintf(f, ":%s", errno_name);
                                else
                                        fprintf(f, ":%d", num);
                        }
                }
#endif

                fputc('\n', f);
        }

        if (c->syscall_archs) {
                fprintf(f,
                        "%sSystemCallArchitectures:",
                        prefix);

#if HAVE_SECCOMP
                void *id;
                SET_FOREACH(id, c->syscall_archs)
                        fprintf(f, " %s", strna(seccomp_arch_to_string(PTR_TO_UINT32(id) - 1)));
#endif
                fputc('\n', f);
        }

        if (exec_context_restrict_namespaces_set(c)) {
                _cleanup_free_ char *s = NULL;

                r = namespace_flags_to_string(c->restrict_namespaces, &s);
                if (r >= 0)
                        fprintf(f, "%sRestrictNamespaces: %s\n",
                                prefix, strna(s));
        }

#if HAVE_LIBBPF
        if (exec_context_restrict_filesystems_set(c)) {
                char *fs;
                SET_FOREACH(fs, c->restrict_filesystems)
                        fprintf(f, "%sRestrictFileSystems: %s\n", prefix, fs);
        }
#endif

        if (c->network_namespace_path)
                fprintf(f,
                        "%sNetworkNamespacePath: %s\n",
                        prefix, c->network_namespace_path);

        if (c->syscall_errno > 0) {
                fprintf(f, "%sSystemCallErrorNumber: ", prefix);

#if HAVE_SECCOMP
                const char *errno_name = seccomp_errno_or_action_to_string(c->syscall_errno);
                if (errno_name)
                        fputs(errno_name, f);
                else
                        fprintf(f, "%d", c->syscall_errno);
#endif
                fputc('\n', f);
        }

        for (size_t i = 0; i < c->n_mount_images; i++) {
                fprintf(f, "%sMountImages: %s%s:%s", prefix,
                        c->mount_images[i].ignore_enoent ? "-": "",
                        c->mount_images[i].source,
                        c->mount_images[i].destination);
                LIST_FOREACH(mount_options, o, c->mount_images[i].mount_options)
                        fprintf(f, ":%s:%s",
                                partition_designator_to_string(o->partition_designator),
                                strempty(o->options));
                fprintf(f, "\n");
        }

        for (size_t i = 0; i < c->n_extension_images; i++) {
                fprintf(f, "%sExtensionImages: %s%s", prefix,
                        c->extension_images[i].ignore_enoent ? "-": "",
                        c->extension_images[i].source);
                LIST_FOREACH(mount_options, o, c->extension_images[i].mount_options)
                        fprintf(f, ":%s:%s",
                                partition_designator_to_string(o->partition_designator),
                                strempty(o->options));
                fprintf(f, "\n");
        }

        strv_dump(f, prefix, "ExtensionDirectories", c->extension_directories);
}

bool exec_context_maintains_privileges(const ExecContext *c) {
        assert(c);

        /* Returns true if the process forked off would run under
         * an unchanged UID or as root. */

        if (!c->user)
                return true;

        if (streq(c->user, "root") || streq(c->user, "0"))
                return true;

        return false;
}

int exec_context_get_effective_ioprio(const ExecContext *c) {
        int p;

        assert(c);

        if (c->ioprio_set)
                return c->ioprio;

        p = ioprio_get(IOPRIO_WHO_PROCESS, 0);
        if (p < 0)
                return IOPRIO_DEFAULT_CLASS_AND_PRIO;

        return ioprio_normalize(p);
}

bool exec_context_get_effective_mount_apivfs(const ExecContext *c) {
        assert(c);

        /* Explicit setting wins */
        if (c->mount_apivfs_set)
                return c->mount_apivfs;

        /* Default to "yes" if root directory or image are specified */
        if (exec_context_with_rootfs(c))
                return true;

        return false;
}

void exec_context_free_log_extra_fields(ExecContext *c) {
        assert(c);

        for (size_t l = 0; l < c->n_log_extra_fields; l++)
                free(c->log_extra_fields[l].iov_base);
        c->log_extra_fields = mfree(c->log_extra_fields);
        c->n_log_extra_fields = 0;
}

void exec_context_revert_tty(ExecContext *c) {
        _cleanup_close_ int fd = -1;
        const char *path;
        struct stat st;
        int r;

        assert(c);

        /* First, reset the TTY (possibly kicking everybody else from the TTY) */
        exec_context_tty_reset(c, NULL);

        /* And then undo what chown_terminal() did earlier. Note that we only do this if we have a path
         * configured. If the TTY was passed to us as file descriptor we assume the TTY is opened and managed
         * by whoever passed it to us and thus knows better when and how to chmod()/chown() it back. */
        if (!exec_context_may_touch_tty(c))
                return;

        path = exec_context_tty_path(c);
        if (!path)
                return;

        fd = open(path, O_PATH|O_CLOEXEC);
        if (fd < 0)
                return (void) log_full_errno(errno == ENOENT ? LOG_DEBUG : LOG_WARNING, errno,
                                             "Failed to open TTY inode of '%s' to adjust ownership/access mode, ignoring: %m",
                                             path);

        if (fstat(fd, &st) < 0)
                return (void) log_warning_errno(errno, "Failed to stat TTY '%s', ignoring: %m", path);

        /* Let's add a superficial check that we only do this for stuff that looks like a TTY. We only check
         * if things are a character device, since a proper check either means we'd have to open the TTY and
         * use isatty(), but we'd rather not do that since opening TTYs comes with all kinds of side-effects
         * and is slow. Or we'd have to hardcode dev_t major information, which we'd rather avoid. Why bother
         * with this at all? → https://github.com/systemd/systemd/issues/19213 */
        if (!S_ISCHR(st.st_mode))
                return log_warning("Configured TTY '%s' is not actually a character device, ignoring.", path);

        r = fchmod_and_chown(fd, TTY_MODE, 0, TTY_GID);
        if (r < 0)
                log_warning_errno(r, "Failed to reset TTY ownership/access mode of %s, ignoring: %m", path);
}

int exec_context_get_clean_directories(
                ExecContext *c,
                char **prefix,
                ExecCleanMask mask,
                char ***ret) {

        _cleanup_strv_free_ char **l = NULL;
        int r;

        assert(c);
        assert(prefix);
        assert(ret);

        for (ExecDirectoryType t = 0; t < _EXEC_DIRECTORY_TYPE_MAX; t++) {
                if (!FLAGS_SET(mask, 1U << t))
                        continue;

                if (!prefix[t])
                        continue;

                for (size_t i = 0; i < c->directories[t].n_items; i++) {
                        char *j;

                        j = path_join(prefix[t], c->directories[t].items[i].path);
                        if (!j)
                                return -ENOMEM;

                        r = strv_consume(&l, j);
                        if (r < 0)
                                return r;

                        /* Also remove private directories unconditionally. */
                        if (t != EXEC_DIRECTORY_CONFIGURATION) {
                                j = path_join(prefix[t], "private", c->directories[t].items[i].path);
                                if (!j)
                                        return -ENOMEM;

                                r = strv_consume(&l, j);
                                if (r < 0)
                                        return r;
                        }

                        STRV_FOREACH(symlink, c->directories[t].items[i].symlinks) {
                                j = path_join(prefix[t], *symlink);
                                if (!j)
                                        return -ENOMEM;

                                r = strv_consume(&l, j);
                                if (r < 0)
                                        return r;
                        }
                }
        }

        *ret = TAKE_PTR(l);
        return 0;
}

int exec_context_get_clean_mask(ExecContext *c, ExecCleanMask *ret) {
        ExecCleanMask mask = 0;

        assert(c);
        assert(ret);

        for (ExecDirectoryType t = 0; t < _EXEC_DIRECTORY_TYPE_MAX; t++)
                if (c->directories[t].n_items > 0)
                        mask |= 1U << t;

        *ret = mask;
        return 0;
}

void exec_status_start(ExecStatus *s, pid_t pid) {
        assert(s);

        *s = (ExecStatus) {
                .pid = pid,
        };

        dual_timestamp_get(&s->start_timestamp);
}

void exec_status_exit(ExecStatus *s, const ExecContext *context, pid_t pid, int code, int status) {
        assert(s);

        if (s->pid != pid)
                *s = (ExecStatus) {
                        .pid = pid,
                };

        dual_timestamp_get(&s->exit_timestamp);

        s->code = code;
        s->status = status;

        if (context && context->utmp_id)
                (void) utmp_put_dead_process(context->utmp_id, pid, code, status);
}

void exec_status_reset(ExecStatus *s) {
        assert(s);

        *s = (ExecStatus) {};
}

void exec_status_dump(const ExecStatus *s, FILE *f, const char *prefix) {
        assert(s);
        assert(f);

        if (s->pid <= 0)
                return;

        prefix = strempty(prefix);

        fprintf(f,
                "%sPID: "PID_FMT"\n",
                prefix, s->pid);

        if (dual_timestamp_is_set(&s->start_timestamp))
                fprintf(f,
                        "%sStart Timestamp: %s\n",
                        prefix, FORMAT_TIMESTAMP(s->start_timestamp.realtime));

        if (dual_timestamp_is_set(&s->exit_timestamp))
                fprintf(f,
                        "%sExit Timestamp: %s\n"
                        "%sExit Code: %s\n"
                        "%sExit Status: %i\n",
                        prefix, FORMAT_TIMESTAMP(s->exit_timestamp.realtime),
                        prefix, sigchld_code_to_string(s->code),
                        prefix, s->status);
}

static void exec_command_dump(ExecCommand *c, FILE *f, const char *prefix) {
        _cleanup_free_ char *cmd = NULL;
        const char *prefix2;

        assert(c);
        assert(f);

        prefix = strempty(prefix);
        prefix2 = strjoina(prefix, "\t");

        cmd = quote_command_line(c->argv, SHELL_ESCAPE_EMPTY);

        fprintf(f,
                "%sCommand Line: %s\n",
                prefix, strnull(cmd));

        exec_status_dump(&c->exec_status, f, prefix2);
}

void exec_command_dump_list(ExecCommand *c, FILE *f, const char *prefix) {
        assert(f);

        prefix = strempty(prefix);

        LIST_FOREACH(command, i, c)
                exec_command_dump(i, f, prefix);
}

void exec_command_append_list(ExecCommand **l, ExecCommand *e) {
        ExecCommand *end;

        assert(l);
        assert(e);

        if (*l) {
                /* It's kind of important, that we keep the order here */
                LIST_FIND_TAIL(command, *l, end);
                LIST_INSERT_AFTER(command, *l, end, e);
        } else
              *l = e;
}

int exec_command_set(ExecCommand *c, const char *path, ...) {
        va_list ap;
        char **l, *p;

        assert(c);
        assert(path);

        va_start(ap, path);
        l = strv_new_ap(path, ap);
        va_end(ap);

        if (!l)
                return -ENOMEM;

        p = strdup(path);
        if (!p) {
                strv_free(l);
                return -ENOMEM;
        }

        free_and_replace(c->path, p);

        return strv_free_and_replace(c->argv, l);
}

int exec_command_append(ExecCommand *c, const char *path, ...) {
        _cleanup_strv_free_ char **l = NULL;
        va_list ap;
        int r;

        assert(c);
        assert(path);

        va_start(ap, path);
        l = strv_new_ap(path, ap);
        va_end(ap);

        if (!l)
                return -ENOMEM;

        r = strv_extend_strv(&c->argv, l, false);
        if (r < 0)
                return r;

        return 0;
}

static void *remove_tmpdir_thread(void *p) {
        _cleanup_free_ char *path = p;

        (void) rm_rf(path, REMOVE_ROOT|REMOVE_PHYSICAL);
        return NULL;
}

static ExecRuntime* exec_runtime_free(ExecRuntime *rt, bool destroy) {
        int r;

        if (!rt)
                return NULL;

        if (rt->manager)
                (void) hashmap_remove(rt->manager->exec_runtime_by_id, rt->id);

        /* When destroy is true, then rm_rf tmp_dir and var_tmp_dir. */

        if (destroy && rt->tmp_dir && !streq(rt->tmp_dir, RUN_SYSTEMD_EMPTY)) {
                log_debug("Spawning thread to nuke %s", rt->tmp_dir);

                r = asynchronous_job(remove_tmpdir_thread, rt->tmp_dir);
                if (r < 0)
                        log_warning_errno(r, "Failed to nuke %s: %m", rt->tmp_dir);
                else
                        rt->tmp_dir = NULL;
        }

        if (destroy && rt->var_tmp_dir && !streq(rt->var_tmp_dir, RUN_SYSTEMD_EMPTY)) {
                log_debug("Spawning thread to nuke %s", rt->var_tmp_dir);

                r = asynchronous_job(remove_tmpdir_thread, rt->var_tmp_dir);
                if (r < 0)
                        log_warning_errno(r, "Failed to nuke %s: %m", rt->var_tmp_dir);
                else
                        rt->var_tmp_dir = NULL;
        }

        rt->id = mfree(rt->id);
        rt->tmp_dir = mfree(rt->tmp_dir);
        rt->var_tmp_dir = mfree(rt->var_tmp_dir);
        safe_close_pair(rt->netns_storage_socket);
        safe_close_pair(rt->ipcns_storage_socket);
        return mfree(rt);
}

static void exec_runtime_freep(ExecRuntime **rt) {
        (void) exec_runtime_free(*rt, false);
}

static int exec_runtime_allocate(ExecRuntime **ret, const char *id) {
        _cleanup_free_ char *id_copy = NULL;
        ExecRuntime *n;

        assert(ret);

        id_copy = strdup(id);
        if (!id_copy)
                return -ENOMEM;

        n = new(ExecRuntime, 1);
        if (!n)
                return -ENOMEM;

        *n = (ExecRuntime) {
                .id = TAKE_PTR(id_copy),
                .netns_storage_socket = { -1, -1 },
                .ipcns_storage_socket = { -1, -1 },
        };

        *ret = n;
        return 0;
}

static int exec_runtime_add(
                Manager *m,
                const char *id,
                char **tmp_dir,
                char **var_tmp_dir,
                int netns_storage_socket[2],
                int ipcns_storage_socket[2],
                ExecRuntime **ret) {

        _cleanup_(exec_runtime_freep) ExecRuntime *rt = NULL;
        int r;

        assert(m);
        assert(id);

        /* tmp_dir, var_tmp_dir, {net,ipc}ns_storage_socket fds are donated on success */

        r = exec_runtime_allocate(&rt, id);
        if (r < 0)
                return r;

        r = hashmap_ensure_put(&m->exec_runtime_by_id, &string_hash_ops, rt->id, rt);
        if (r < 0)
                return r;

        assert(!!rt->tmp_dir == !!rt->var_tmp_dir); /* We require both to be set together */
        rt->tmp_dir = TAKE_PTR(*tmp_dir);
        rt->var_tmp_dir = TAKE_PTR(*var_tmp_dir);

        if (netns_storage_socket) {
                rt->netns_storage_socket[0] = TAKE_FD(netns_storage_socket[0]);
                rt->netns_storage_socket[1] = TAKE_FD(netns_storage_socket[1]);
        }

        if (ipcns_storage_socket) {
                rt->ipcns_storage_socket[0] = TAKE_FD(ipcns_storage_socket[0]);
                rt->ipcns_storage_socket[1] = TAKE_FD(ipcns_storage_socket[1]);
        }

        rt->manager = m;

        if (ret)
                *ret = rt;
        /* do not remove created ExecRuntime object when the operation succeeds. */
        TAKE_PTR(rt);
        return 0;
}

static int exec_runtime_make(
                Manager *m,
                const ExecContext *c,
                const char *id,
                ExecRuntime **ret) {

        _cleanup_(namespace_cleanup_tmpdirp) char *tmp_dir = NULL, *var_tmp_dir = NULL;
        _cleanup_close_pair_ int netns_storage_socket[2] = { -1, -1 }, ipcns_storage_socket[2] = { -1, -1 };
        int r;

        assert(m);
        assert(c);
        assert(id);

        /* It is not necessary to create ExecRuntime object. */
        if (!c->private_network && !c->private_ipc && !c->private_tmp && !c->network_namespace_path) {
                *ret = NULL;
                return 0;
        }

        if (c->private_tmp &&
            !(prefixed_path_strv_contains(c->inaccessible_paths, "/tmp") &&
              (prefixed_path_strv_contains(c->inaccessible_paths, "/var/tmp") ||
               prefixed_path_strv_contains(c->inaccessible_paths, "/var")))) {
                r = setup_tmp_dirs(id, &tmp_dir, &var_tmp_dir);
                if (r < 0)
                        return r;
        }

        if (c->private_network || c->network_namespace_path) {
                if (socketpair(AF_UNIX, SOCK_DGRAM|SOCK_CLOEXEC, 0, netns_storage_socket) < 0)
                        return -errno;
        }

        if (c->private_ipc || c->ipc_namespace_path) {
                if (socketpair(AF_UNIX, SOCK_DGRAM|SOCK_CLOEXEC, 0, ipcns_storage_socket) < 0)
                        return -errno;
        }

        r = exec_runtime_add(m, id, &tmp_dir, &var_tmp_dir, netns_storage_socket, ipcns_storage_socket, ret);
        if (r < 0)
                return r;

        return 1;
}

int exec_runtime_acquire(Manager *m, const ExecContext *c, const char *id, bool create, ExecRuntime **ret) {
        ExecRuntime *rt;
        int r;

        assert(m);
        assert(id);
        assert(ret);

        rt = hashmap_get(m->exec_runtime_by_id, id);
        if (rt)
                /* We already have an ExecRuntime object, let's increase the ref count and reuse it */
                goto ref;

        if (!create) {
                *ret = NULL;
                return 0;
        }

        /* If not found, then create a new object. */
        r = exec_runtime_make(m, c, id, &rt);
        if (r < 0)
                return r;
        if (r == 0) {
                /* When r == 0, it is not necessary to create ExecRuntime object. */
                *ret = NULL;
                return 0;
        }

ref:
        /* increment reference counter. */
        rt->n_ref++;
        *ret = rt;
        return 1;
}

ExecRuntime *exec_runtime_unref(ExecRuntime *rt, bool destroy) {
        if (!rt)
                return NULL;

        assert(rt->n_ref > 0);

        rt->n_ref--;
        if (rt->n_ref > 0)
                return NULL;

        return exec_runtime_free(rt, destroy);
}

int exec_runtime_serialize(const Manager *m, FILE *f, FDSet *fds) {
        ExecRuntime *rt;

        assert(m);
        assert(f);
        assert(fds);

        HASHMAP_FOREACH(rt, m->exec_runtime_by_id) {
                fprintf(f, "exec-runtime=%s", rt->id);

                if (rt->tmp_dir)
                        fprintf(f, " tmp-dir=%s", rt->tmp_dir);

                if (rt->var_tmp_dir)
                        fprintf(f, " var-tmp-dir=%s", rt->var_tmp_dir);

                if (rt->netns_storage_socket[0] >= 0) {
                        int copy;

                        copy = fdset_put_dup(fds, rt->netns_storage_socket[0]);
                        if (copy < 0)
                                return copy;

                        fprintf(f, " netns-socket-0=%i", copy);
                }

                if (rt->netns_storage_socket[1] >= 0) {
                        int copy;

                        copy = fdset_put_dup(fds, rt->netns_storage_socket[1]);
                        if (copy < 0)
                                return copy;

                        fprintf(f, " netns-socket-1=%i", copy);
                }

                if (rt->ipcns_storage_socket[0] >= 0) {
                        int copy;

                        copy = fdset_put_dup(fds, rt->ipcns_storage_socket[0]);
                        if (copy < 0)
                                return copy;

                        fprintf(f, " ipcns-socket-0=%i", copy);
                }

                if (rt->ipcns_storage_socket[1] >= 0) {
                        int copy;

                        copy = fdset_put_dup(fds, rt->ipcns_storage_socket[1]);
                        if (copy < 0)
                                return copy;

                        fprintf(f, " ipcns-socket-1=%i", copy);
                }

                fputc('\n', f);
        }

        return 0;
}

int exec_runtime_deserialize_compat(Unit *u, const char *key, const char *value, FDSet *fds) {
        _cleanup_(exec_runtime_freep) ExecRuntime *rt_create = NULL;
        ExecRuntime *rt;
        int r;

        /* This is for the migration from old (v237 or earlier) deserialization text.
         * Due to the bug #7790, this may not work with the units that use JoinsNamespaceOf=.
         * Even if the ExecRuntime object originally created by the other unit, we cannot judge
         * so or not from the serialized text, then we always creates a new object owned by this. */

        assert(u);
        assert(key);
        assert(value);

        /* Manager manages ExecRuntime objects by the unit id.
         * So, we omit the serialized text when the unit does not have id (yet?)... */
        if (isempty(u->id)) {
                log_unit_debug(u, "Invocation ID not found. Dropping runtime parameter.");
                return 0;
        }

        if (hashmap_ensure_allocated(&u->manager->exec_runtime_by_id, &string_hash_ops) < 0)
                return log_oom();

        rt = hashmap_get(u->manager->exec_runtime_by_id, u->id);
        if (!rt) {
                if (exec_runtime_allocate(&rt_create, u->id) < 0)
                        return log_oom();

                rt = rt_create;
        }

        if (streq(key, "tmp-dir")) {
                if (free_and_strdup_warn(&rt->tmp_dir, value) < 0)
                        return -ENOMEM;

        } else if (streq(key, "var-tmp-dir")) {
                if (free_and_strdup_warn(&rt->var_tmp_dir, value) < 0)
                        return -ENOMEM;

        } else if (streq(key, "netns-socket-0")) {
                int fd;

                if (safe_atoi(value, &fd) < 0 || !fdset_contains(fds, fd)) {
                        log_unit_debug(u, "Failed to parse netns socket value: %s", value);
                        return 0;
                }

                safe_close(rt->netns_storage_socket[0]);
                rt->netns_storage_socket[0] = fdset_remove(fds, fd);

        } else if (streq(key, "netns-socket-1")) {
                int fd;

                if (safe_atoi(value, &fd) < 0 || !fdset_contains(fds, fd)) {
                        log_unit_debug(u, "Failed to parse netns socket value: %s", value);
                        return 0;
                }

                safe_close(rt->netns_storage_socket[1]);
                rt->netns_storage_socket[1] = fdset_remove(fds, fd);

        } else
                return 0;

        /* If the object is newly created, then put it to the hashmap which manages ExecRuntime objects. */
        if (rt_create) {
                r = hashmap_put(u->manager->exec_runtime_by_id, rt_create->id, rt_create);
                if (r < 0) {
                        log_unit_debug_errno(u, r, "Failed to put runtime parameter to manager's storage: %m");
                        return 0;
                }

                rt_create->manager = u->manager;

                /* Avoid cleanup */
                TAKE_PTR(rt_create);
        }

        return 1;
}

int exec_runtime_deserialize_one(Manager *m, const char *value, FDSet *fds) {
        _cleanup_free_ char *tmp_dir = NULL, *var_tmp_dir = NULL;
        char *id = NULL;
        int r, netns_fdpair[] = {-1, -1}, ipcns_fdpair[] = {-1, -1};
        const char *p, *v = ASSERT_PTR(value);
        size_t n;

        assert(m);
        assert(fds);

        n = strcspn(v, " ");
        id = strndupa_safe(v, n);
        if (v[n] != ' ')
                goto finalize;
        p = v + n + 1;

        v = startswith(p, "tmp-dir=");
        if (v) {
                n = strcspn(v, " ");
                tmp_dir = strndup(v, n);
                if (!tmp_dir)
                        return log_oom();
                if (v[n] != ' ')
                        goto finalize;
                p = v + n + 1;
        }

        v = startswith(p, "var-tmp-dir=");
        if (v) {
                n = strcspn(v, " ");
                var_tmp_dir = strndup(v, n);
                if (!var_tmp_dir)
                        return log_oom();
                if (v[n] != ' ')
                        goto finalize;
                p = v + n + 1;
        }

        v = startswith(p, "netns-socket-0=");
        if (v) {
                char *buf;

                n = strcspn(v, " ");
                buf = strndupa_safe(v, n);

                r = safe_atoi(buf, &netns_fdpair[0]);
                if (r < 0)
                        return log_debug_errno(r, "Unable to parse exec-runtime specification netns-socket-0=%s: %m", buf);
                if (!fdset_contains(fds, netns_fdpair[0]))
                        return log_debug_errno(SYNTHETIC_ERRNO(EBADF),
                                               "exec-runtime specification netns-socket-0= refers to unknown fd %d: %m", netns_fdpair[0]);
                netns_fdpair[0] = fdset_remove(fds, netns_fdpair[0]);
                if (v[n] != ' ')
                        goto finalize;
                p = v + n + 1;
        }

        v = startswith(p, "netns-socket-1=");
        if (v) {
                char *buf;

                n = strcspn(v, " ");
                buf = strndupa_safe(v, n);

                r = safe_atoi(buf, &netns_fdpair[1]);
                if (r < 0)
                        return log_debug_errno(r, "Unable to parse exec-runtime specification netns-socket-1=%s: %m", buf);
                if (!fdset_contains(fds, netns_fdpair[1]))
                        return log_debug_errno(SYNTHETIC_ERRNO(EBADF),
                                               "exec-runtime specification netns-socket-1= refers to unknown fd %d: %m", netns_fdpair[1]);
                netns_fdpair[1] = fdset_remove(fds, netns_fdpair[1]);
                if (v[n] != ' ')
                        goto finalize;
                p = v + n + 1;
        }

        v = startswith(p, "ipcns-socket-0=");
        if (v) {
                char *buf;

                n = strcspn(v, " ");
                buf = strndupa_safe(v, n);

                r = safe_atoi(buf, &ipcns_fdpair[0]);
                if (r < 0)
                        return log_debug_errno(r, "Unable to parse exec-runtime specification ipcns-socket-0=%s: %m", buf);
                if (!fdset_contains(fds, ipcns_fdpair[0]))
                        return log_debug_errno(SYNTHETIC_ERRNO(EBADF),
                                               "exec-runtime specification ipcns-socket-0= refers to unknown fd %d: %m", ipcns_fdpair[0]);
                ipcns_fdpair[0] = fdset_remove(fds, ipcns_fdpair[0]);
                if (v[n] != ' ')
                        goto finalize;
                p = v + n + 1;
        }

        v = startswith(p, "ipcns-socket-1=");
        if (v) {
                char *buf;

                n = strcspn(v, " ");
                buf = strndupa_safe(v, n);

                r = safe_atoi(buf, &ipcns_fdpair[1]);
                if (r < 0)
                        return log_debug_errno(r, "Unable to parse exec-runtime specification ipcns-socket-1=%s: %m", buf);
                if (!fdset_contains(fds, ipcns_fdpair[1]))
                        return log_debug_errno(SYNTHETIC_ERRNO(EBADF),
                                               "exec-runtime specification ipcns-socket-1= refers to unknown fd %d: %m", ipcns_fdpair[1]);
                ipcns_fdpair[1] = fdset_remove(fds, ipcns_fdpair[1]);
        }

finalize:
        r = exec_runtime_add(m, id, &tmp_dir, &var_tmp_dir, netns_fdpair, ipcns_fdpair, NULL);
        if (r < 0)
                return log_debug_errno(r, "Failed to add exec-runtime: %m");
        return 0;
}

void exec_runtime_vacuum(Manager *m) {
        ExecRuntime *rt;

        assert(m);

        /* Free unreferenced ExecRuntime objects. This is used after manager deserialization process. */

        HASHMAP_FOREACH(rt, m->exec_runtime_by_id) {
                if (rt->n_ref > 0)
                        continue;

                (void) exec_runtime_free(rt, false);
        }
}

void exec_params_clear(ExecParameters *p) {
        if (!p)
                return;

        p->environment = strv_free(p->environment);
        p->fd_names = strv_free(p->fd_names);
        p->fds = mfree(p->fds);
        p->exec_fd = safe_close(p->exec_fd);
}

ExecSetCredential *exec_set_credential_free(ExecSetCredential *sc) {
        if (!sc)
                return NULL;

        free(sc->id);
        free(sc->data);
        return mfree(sc);
}

ExecLoadCredential *exec_load_credential_free(ExecLoadCredential *lc) {
        if (!lc)
                return NULL;

        free(lc->id);
        free(lc->path);
        return mfree(lc);
}

void exec_directory_done(ExecDirectory *d) {
        if (!d)
                return;

        for (size_t i = 0; i < d->n_items; i++) {
                free(d->items[i].path);
                strv_free(d->items[i].symlinks);
        }

        d->items = mfree(d->items);
        d->n_items = 0;
        d->mode = 0755;
}

static ExecDirectoryItem *exec_directory_find(ExecDirectory *d, const char *path) {
        assert(d);
        assert(path);

        for (size_t i = 0; i < d->n_items; i++)
                if (path_equal(d->items[i].path, path))
                        return &d->items[i];

        return NULL;
}

int exec_directory_add(ExecDirectory *d, const char *path, const char *symlink) {
        _cleanup_strv_free_ char **s = NULL;
        _cleanup_free_ char *p = NULL;
        ExecDirectoryItem *existing;
        int r;

        assert(d);
        assert(path);

        existing = exec_directory_find(d, path);
        if (existing) {
                r = strv_extend(&existing->symlinks, symlink);
                if (r < 0)
                        return r;

                return 0; /* existing item is updated */
        }

        p = strdup(path);
        if (!p)
                return -ENOMEM;

        if (symlink) {
                s = strv_new(symlink);
                if (!s)
                        return -ENOMEM;
        }

        if (!GREEDY_REALLOC(d->items, d->n_items + 1))
                return -ENOMEM;

        d->items[d->n_items++] = (ExecDirectoryItem) {
                .path = TAKE_PTR(p),
                .symlinks = TAKE_PTR(s),
        };

        return 1; /* new item is added */
}

static int exec_directory_item_compare_func(const ExecDirectoryItem *a, const ExecDirectoryItem *b) {
        assert(a);
        assert(b);

        return path_compare(a->path, b->path);
}

void exec_directory_sort(ExecDirectory *d) {
        assert(d);

        /* Sort the exec directories to make always parent directories processed at first in
         * setup_exec_directory(), e.g., even if StateDirectory=foo/bar foo, we need to create foo at first,
         * then foo/bar. Also, set .only_create flag if one of the parent directories is contained in the
         * list. See also comments in setup_exec_directory() and issue #24783. */

        if (d->n_items <= 1)
                return;

        typesafe_qsort(d->items, d->n_items, exec_directory_item_compare_func);

        for (size_t i = 1; i < d->n_items; i++)
                for (size_t j = 0; j < i; j++)
                        if (path_startswith(d->items[i].path, d->items[j].path)) {
                                d->items[i].only_create = true;
                                break;
                        }
}

DEFINE_HASH_OPS_WITH_VALUE_DESTRUCTOR(exec_set_credential_hash_ops, char, string_hash_func, string_compare_func, ExecSetCredential, exec_set_credential_free);
DEFINE_HASH_OPS_WITH_VALUE_DESTRUCTOR(exec_load_credential_hash_ops, char, string_hash_func, string_compare_func, ExecLoadCredential, exec_load_credential_free);

static const char* const exec_input_table[_EXEC_INPUT_MAX] = {
        [EXEC_INPUT_NULL] = "null",
        [EXEC_INPUT_TTY] = "tty",
        [EXEC_INPUT_TTY_FORCE] = "tty-force",
        [EXEC_INPUT_TTY_FAIL] = "tty-fail",
        [EXEC_INPUT_SOCKET] = "socket",
        [EXEC_INPUT_NAMED_FD] = "fd",
        [EXEC_INPUT_DATA] = "data",
        [EXEC_INPUT_FILE] = "file",
};

DEFINE_STRING_TABLE_LOOKUP(exec_input, ExecInput);

static const char* const exec_output_table[_EXEC_OUTPUT_MAX] = {
        [EXEC_OUTPUT_INHERIT] = "inherit",
        [EXEC_OUTPUT_NULL] = "null",
        [EXEC_OUTPUT_TTY] = "tty",
        [EXEC_OUTPUT_KMSG] = "kmsg",
        [EXEC_OUTPUT_KMSG_AND_CONSOLE] = "kmsg+console",
        [EXEC_OUTPUT_JOURNAL] = "journal",
        [EXEC_OUTPUT_JOURNAL_AND_CONSOLE] = "journal+console",
        [EXEC_OUTPUT_SOCKET] = "socket",
        [EXEC_OUTPUT_NAMED_FD] = "fd",
        [EXEC_OUTPUT_FILE] = "file",
        [EXEC_OUTPUT_FILE_APPEND] = "append",
        [EXEC_OUTPUT_FILE_TRUNCATE] = "truncate",
};

DEFINE_STRING_TABLE_LOOKUP(exec_output, ExecOutput);

static const char* const exec_utmp_mode_table[_EXEC_UTMP_MODE_MAX] = {
        [EXEC_UTMP_INIT] = "init",
        [EXEC_UTMP_LOGIN] = "login",
        [EXEC_UTMP_USER] = "user",
};

DEFINE_STRING_TABLE_LOOKUP(exec_utmp_mode, ExecUtmpMode);

static const char* const exec_preserve_mode_table[_EXEC_PRESERVE_MODE_MAX] = {
        [EXEC_PRESERVE_NO] = "no",
        [EXEC_PRESERVE_YES] = "yes",
        [EXEC_PRESERVE_RESTART] = "restart",
};

DEFINE_STRING_TABLE_LOOKUP_WITH_BOOLEAN(exec_preserve_mode, ExecPreserveMode, EXEC_PRESERVE_YES);

/* This table maps ExecDirectoryType to the setting it is configured with in the unit */
static const char* const exec_directory_type_table[_EXEC_DIRECTORY_TYPE_MAX] = {
        [EXEC_DIRECTORY_RUNTIME] = "RuntimeDirectory",
        [EXEC_DIRECTORY_STATE] = "StateDirectory",
        [EXEC_DIRECTORY_CACHE] = "CacheDirectory",
        [EXEC_DIRECTORY_LOGS] = "LogsDirectory",
        [EXEC_DIRECTORY_CONFIGURATION] = "ConfigurationDirectory",
};

DEFINE_STRING_TABLE_LOOKUP(exec_directory_type, ExecDirectoryType);

/* This table maps ExecDirectoryType to the symlink setting it is configured with in the unit */
static const char* const exec_directory_type_symlink_table[_EXEC_DIRECTORY_TYPE_MAX] = {
        [EXEC_DIRECTORY_RUNTIME]       = "RuntimeDirectorySymlink",
        [EXEC_DIRECTORY_STATE]         = "StateDirectorySymlink",
        [EXEC_DIRECTORY_CACHE]         = "CacheDirectorySymlink",
        [EXEC_DIRECTORY_LOGS]          = "LogsDirectorySymlink",
        [EXEC_DIRECTORY_CONFIGURATION] = "ConfigurationDirectorySymlink",
};

DEFINE_STRING_TABLE_LOOKUP(exec_directory_type_symlink, ExecDirectoryType);

/* And this table maps ExecDirectoryType too, but to a generic term identifying the type of resource. This
 * one is supposed to be generic enough to be used for unit types that don't use ExecContext and per-unit
 * directories, specifically .timer units with their timestamp touch file. */
static const char* const exec_resource_type_table[_EXEC_DIRECTORY_TYPE_MAX] = {
        [EXEC_DIRECTORY_RUNTIME] = "runtime",
        [EXEC_DIRECTORY_STATE] = "state",
        [EXEC_DIRECTORY_CACHE] = "cache",
        [EXEC_DIRECTORY_LOGS] = "logs",
        [EXEC_DIRECTORY_CONFIGURATION] = "configuration",
};

DEFINE_STRING_TABLE_LOOKUP(exec_resource_type, ExecDirectoryType);

/* And this table also maps ExecDirectoryType, to the environment variable we pass the selected directory to
 * the service payload in. */
static const char* const exec_directory_env_name_table[_EXEC_DIRECTORY_TYPE_MAX] = {
        [EXEC_DIRECTORY_RUNTIME] = "RUNTIME_DIRECTORY",
        [EXEC_DIRECTORY_STATE] = "STATE_DIRECTORY",
        [EXEC_DIRECTORY_CACHE] = "CACHE_DIRECTORY",
        [EXEC_DIRECTORY_LOGS] = "LOGS_DIRECTORY",
        [EXEC_DIRECTORY_CONFIGURATION] = "CONFIGURATION_DIRECTORY",
};

DEFINE_PRIVATE_STRING_TABLE_LOOKUP_TO_STRING(exec_directory_env_name, ExecDirectoryType);

static const char* const exec_keyring_mode_table[_EXEC_KEYRING_MODE_MAX] = {
        [EXEC_KEYRING_INHERIT] = "inherit",
        [EXEC_KEYRING_PRIVATE] = "private",
        [EXEC_KEYRING_SHARED] = "shared",
};

DEFINE_STRING_TABLE_LOOKUP(exec_keyring_mode, ExecKeyringMode);
