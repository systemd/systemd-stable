/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <fcntl.h>
#include <getopt.h>
#include <linux/loop.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mount.h>

#include "sd-device.h"

#include "architecture.h"
#include "blockdev-util.h"
#include "build.h"
#include "chase-symlinks.h"
#include "copy.h"
#include "device-util.h"
#include "devnum-util.h"
#include "discover-image.h"
#include "dissect-image.h"
#include "env-util.h"
#include "escape.h"
#include "fd-util.h"
#include "fileio.h"
#include "format-table.h"
#include "format-util.h"
#include "fs-util.h"
#include "hexdecoct.h"
#include "log.h"
#include "loop-util.h"
#include "main-func.h"
#include "mkdir.h"
#include "mount-util.h"
#include "mountpoint-util.h"
#include "namespace-util.h"
#include "parse-argument.h"
#include "parse-util.h"
#include "path-util.h"
#include "pretty-print.h"
#include "process-util.h"
#include "recurse-dir.h"
#include "sha256.h"
#include "stat-util.h"
#include "string-util.h"
#include "strv.h"
#include "terminal-util.h"
#include "tmpfile-util.h"
#include "user-util.h"

static enum {
        ACTION_DISSECT,
        ACTION_MOUNT,
        ACTION_UMOUNT,
        ACTION_LIST,
        ACTION_MTREE,
        ACTION_WITH,
        ACTION_COPY_FROM,
        ACTION_COPY_TO,
        ACTION_DISCOVER,
} arg_action = ACTION_DISSECT;
static const char *arg_image = NULL;
static const char *arg_path = NULL;
static const char *arg_source = NULL;
static const char *arg_target = NULL;
static DissectImageFlags arg_flags =
        DISSECT_IMAGE_GENERIC_ROOT |
        DISSECT_IMAGE_DISCARD_ON_LOOP |
        DISSECT_IMAGE_RELAX_VAR_CHECK |
        DISSECT_IMAGE_FSCK |
        DISSECT_IMAGE_USR_NO_ROOT |
        DISSECT_IMAGE_GROWFS |
        DISSECT_IMAGE_PIN_PARTITION_DEVICES |
        DISSECT_IMAGE_ADD_PARTITION_DEVICES;
static VeritySettings arg_verity_settings = VERITY_SETTINGS_DEFAULT;
static JsonFormatFlags arg_json_format_flags = JSON_FORMAT_OFF;
static PagerFlags arg_pager_flags = 0;
static bool arg_legend = true;
static bool arg_rmdir = false;
static bool arg_in_memory = false;
static char **arg_argv = NULL;

STATIC_DESTRUCTOR_REGISTER(arg_verity_settings, verity_settings_done);
STATIC_DESTRUCTOR_REGISTER(arg_argv, strv_freep);

static int help(void) {
        _cleanup_free_ char *link = NULL;
        int r;

        r = terminal_urlify_man("systemd-dissect", "1", &link);
        if (r < 0)
                return log_oom();

        printf("%1$s [OPTIONS...] IMAGE\n"
               "%1$s [OPTIONS...] --mount IMAGE PATH\n"
               "%1$s [OPTIONS...] --umount PATH\n"
               "%1$s [OPTIONS...] --list IMAGE\n"
               "%1$s [OPTIONS...] --mtree IMAGE\n"
               "%1$s [OPTIONS...] --with IMAGE [COMMAND…]\n"
               "%1$s [OPTIONS...] --copy-from IMAGE PATH [TARGET]\n"
               "%1$s [OPTIONS...] --copy-to IMAGE [SOURCE] PATH\n\n"
               "%5$sDissect a Discoverable Disk Image (DDI).%6$s\n\n"
               "%3$sOptions:%4$s\n"
               "     --no-pager           Do not pipe output into a pager\n"
               "     --no-legend          Do not show the headers and footers\n"
               "  -r --read-only          Mount read-only\n"
               "     --fsck=BOOL          Run fsck before mounting\n"
               "     --growfs=BOOL        Grow file system to partition size, if marked\n"
               "     --mkdir              Make mount directory before mounting, if missing\n"
               "     --rmdir              Remove mount directory after unmounting\n"
               "     --discard=MODE       Choose 'discard' mode (disabled, loop, all, crypto)\n"
               "     --in-memory          Copy image into memory\n"
               "     --root-hash=HASH     Specify root hash for verity\n"
               "     --root-hash-sig=SIG  Specify pkcs7 signature of root hash for verity\n"
               "                          as a DER encoded PKCS7, either as a path to a file\n"
               "                          or as an ASCII base64 encoded string prefixed by\n"
               "                          'base64:'\n"
               "     --verity-data=PATH   Specify data file with hash tree for verity if it is\n"
               "                          not embedded in IMAGE\n"
               "     --json=pretty|short|off\n"
               "                          Generate JSON output\n"
               "\n%3$sCommands:%4$s\n"
               "  -h --help               Show this help\n"
               "     --version            Show package version\n"
               "  -m --mount              Mount the image to the specified directory\n"
               "  -M                      Shortcut for --mount --mkdir\n"
               "  -u --umount             Unmount the image from the specified directory\n"
               "  -U                      Shortcut for --umount --rmdir\n"
               "  -l --list               List all the files and directories of the specified\n"
               "                          OS image\n"
               "     --mtree              Show BSD mtree manifest of OS image\n"
               "     --with               Mount, run command, unmount\n"
               "  -x --copy-from          Copy files from image to host\n"
               "  -a --copy-to            Copy files from host to image\n"
               "     --discover           Discover DDIs in well known directories\n"
               "\nSee the %2$s for details.\n",
               program_invocation_short_name,
               link,
               ansi_underline(),
               ansi_normal(),
               ansi_highlight(),
               ansi_normal());

        return 0;
}

static int patch_argv(int *argc, char ***argv, char ***buf) {
        _cleanup_free_ char **l = NULL;
        char **e;

        assert(argc);
        assert(*argc >= 0);
        assert(argv);
        assert(*argv);
        assert(buf);

        /* Ugly hack: if --with is included in command line, also insert "--" immediately after it, to make
         * getopt_long() stop processing switches */

        for (e = *argv + 1; e < *argv + *argc; e++) {
                assert(*e);

                if (streq(*e, "--with"))
                        break;
        }

        if (e >= *argv + *argc || streq_ptr(e[1], "--")) {
                /* No --with used? Or already followed by "--"? Then don't do anything */
                *buf = NULL;
                return 0;
        }

        /* Insert the extra "--" right after the --with */
        l = new(char*, *argc + 2);
        if (!l)
                return log_oom();

        size_t idx = e - *argv + 1;
        memcpy(l, *argv, sizeof(char*) * idx);                          /* copy everything up to and including the --with */
        l[idx] = (char*) "--";                                          /* insert "--" */
        memcpy(l + idx + 1, e + 1, sizeof(char*) * (*argc - idx + 1));  /* copy the rest, including trailing NULL entry */

        (*argc)++;
        (*argv) = l;

        *buf = TAKE_PTR(l);
        return 1;
}

static int parse_argv(int argc, char *argv[]) {

        enum {
                ARG_VERSION = 0x100,
                ARG_NO_PAGER,
                ARG_NO_LEGEND,
                ARG_WITH,
                ARG_DISCARD,
                ARG_FSCK,
                ARG_GROWFS,
                ARG_ROOT_HASH,
                ARG_ROOT_HASH_SIG,
                ARG_VERITY_DATA,
                ARG_MKDIR,
                ARG_RMDIR,
                ARG_IN_MEMORY,
                ARG_JSON,
                ARG_MTREE,
                ARG_DISCOVER,
        };

        static const struct option options[] = {
                { "help",          no_argument,       NULL, 'h'               },
                { "version",       no_argument,       NULL, ARG_VERSION       },
                { "no-pager",      no_argument,       NULL, ARG_NO_PAGER      },
                { "no-legend",     no_argument,       NULL, ARG_NO_LEGEND     },
                { "mount",         no_argument,       NULL, 'm'               },
                { "umount",        no_argument,       NULL, 'u'               },
                { "with",          no_argument,       NULL, ARG_WITH          },
                { "read-only",     no_argument,       NULL, 'r'               },
                { "discard",       required_argument, NULL, ARG_DISCARD       },
                { "fsck",          required_argument, NULL, ARG_FSCK          },
                { "growfs",        required_argument, NULL, ARG_GROWFS        },
                { "root-hash",     required_argument, NULL, ARG_ROOT_HASH     },
                { "root-hash-sig", required_argument, NULL, ARG_ROOT_HASH_SIG },
                { "verity-data",   required_argument, NULL, ARG_VERITY_DATA   },
                { "mkdir",         no_argument,       NULL, ARG_MKDIR         },
                { "rmdir",         no_argument,       NULL, ARG_RMDIR         },
                { "in-memory",     no_argument,       NULL, ARG_IN_MEMORY     },
                { "list",          no_argument,       NULL, 'l'               },
                { "mtree",         no_argument,       NULL, ARG_MTREE         },
                { "copy-from",     no_argument,       NULL, 'x'               },
                { "copy-to",       no_argument,       NULL, 'a'               },
                { "json",          required_argument, NULL, ARG_JSON          },
                { "discover",      no_argument,       NULL, ARG_DISCOVER      },
                {}
        };

        _cleanup_free_ char **buf = NULL; /* we use free(), not strv_free() here, as we don't copy the strings here */
        int c, r;

        assert(argc >= 0);
        assert(argv);

        r = patch_argv(&argc, &argv, &buf);
        if (r < 0)
                return r;

        while ((c = getopt_long(argc, argv, "hmurMUlxa", options, NULL)) >= 0) {

                switch (c) {

                case 'h':
                        return help();

                case ARG_VERSION:
                        return version();

                case ARG_NO_PAGER:
                        arg_pager_flags |= PAGER_DISABLE;
                        break;

                case ARG_NO_LEGEND:
                        arg_legend = false;
                        break;

                case 'm':
                        arg_action = ACTION_MOUNT;
                        break;

                case ARG_MKDIR:
                        arg_flags |= DISSECT_IMAGE_MKDIR;
                        break;

                case 'M':
                        /* Shortcut combination of the above two */
                        arg_action = ACTION_MOUNT;
                        arg_flags |= DISSECT_IMAGE_MKDIR;
                        break;

                case 'u':
                        arg_action = ACTION_UMOUNT;
                        break;

                case ARG_RMDIR:
                        arg_rmdir = true;
                        break;

                case 'U':
                        /* Shortcut combination of the above two */
                        arg_action = ACTION_UMOUNT;
                        arg_rmdir = true;
                        break;

                case 'l':
                        arg_action = ACTION_LIST;
                        arg_flags |= DISSECT_IMAGE_READ_ONLY;
                        break;

                case ARG_MTREE:
                        arg_action = ACTION_MTREE;
                        arg_flags |= DISSECT_IMAGE_READ_ONLY;
                        break;

                case ARG_WITH:
                        arg_action = ACTION_WITH;
                        break;

                case 'x':
                        arg_action = ACTION_COPY_FROM;
                        arg_flags |= DISSECT_IMAGE_READ_ONLY;
                        break;

                case 'a':
                        arg_action = ACTION_COPY_TO;
                        break;

                case 'r':
                        arg_flags |= DISSECT_IMAGE_READ_ONLY;
                        break;

                case ARG_DISCARD: {
                        DissectImageFlags flags;

                        if (streq(optarg, "disabled"))
                                flags = 0;
                        else if (streq(optarg, "loop"))
                                flags = DISSECT_IMAGE_DISCARD_ON_LOOP;
                        else if (streq(optarg, "all"))
                                flags = DISSECT_IMAGE_DISCARD_ON_LOOP | DISSECT_IMAGE_DISCARD;
                        else if (streq(optarg, "crypt"))
                                flags = DISSECT_IMAGE_DISCARD_ANY;
                        else if (streq(optarg, "list")) {
                                puts("disabled\n"
                                     "all\n"
                                     "crypt\n"
                                     "loop");
                                return 0;
                        } else
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                                       "Unknown --discard= parameter: %s",
                                                       optarg);
                        arg_flags = (arg_flags & ~DISSECT_IMAGE_DISCARD_ANY) | flags;

                        break;
                }

                case ARG_IN_MEMORY:
                        arg_in_memory = true;
                        break;

                case ARG_ROOT_HASH: {
                        _cleanup_free_ void *p = NULL;
                        size_t l;

                        r = unhexmem(optarg, strlen(optarg), &p, &l);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse root hash '%s': %m", optarg);
                        if (l < sizeof(sd_id128_t))
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                                       "Root hash must be at least 128bit long: %s", optarg);

                        free_and_replace(arg_verity_settings.root_hash, p);
                        arg_verity_settings.root_hash_size = l;
                        break;
                }

                case ARG_ROOT_HASH_SIG: {
                        char *value;
                        size_t l;
                        void *p;

                        if ((value = startswith(optarg, "base64:"))) {
                                r = unbase64mem(value, strlen(value), &p, &l);
                                if (r < 0)
                                        return log_error_errno(r, "Failed to parse root hash signature '%s': %m", optarg);
                        } else {
                                r = read_full_file(optarg, (char**) &p, &l);
                                if (r < 0)
                                        return log_error_errno(r, "Failed to read root hash signature file '%s': %m", optarg);
                        }

                        free_and_replace(arg_verity_settings.root_hash_sig, p);
                        arg_verity_settings.root_hash_sig_size = l;
                        break;
                }

                case ARG_VERITY_DATA:
                        r = parse_path_argument(optarg, false, &arg_verity_settings.data_path);
                        if (r < 0)
                                return r;
                        break;

                case ARG_FSCK:
                        r = parse_boolean(optarg);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse --fsck= parameter: %s", optarg);

                        SET_FLAG(arg_flags, DISSECT_IMAGE_FSCK, r);
                        break;

                case ARG_GROWFS:
                        r = parse_boolean(optarg);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse --growfs= parameter: %s", optarg);

                        SET_FLAG(arg_flags, DISSECT_IMAGE_GROWFS, r);
                        break;

                case ARG_JSON:
                        r = parse_json_argument(optarg, &arg_json_format_flags);
                        if (r <= 0)
                                return r;

                        break;

                case ARG_DISCOVER:
                        arg_action = ACTION_DISCOVER;
                        break;

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached();
                }
        }

        switch (arg_action) {

        case ACTION_DISSECT:
                if (optind + 1 != argc)
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                               "Expected an image file path as only argument.");

                arg_image = argv[optind];
                arg_flags |= DISSECT_IMAGE_READ_ONLY;
                break;

        case ACTION_MOUNT:
                if (optind + 2 != argc)
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                               "Expected an image file path and mount point path as only arguments.");

                arg_image = argv[optind];
                arg_path = argv[optind + 1];
                arg_flags |= DISSECT_IMAGE_REQUIRE_ROOT;
                break;

        case ACTION_UMOUNT:
                if (optind + 1 != argc)
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                               "Expected a mount point path as only argument.");

                arg_path = argv[optind];
                break;

        case ACTION_LIST:
        case ACTION_MTREE:
                if (optind + 1 != argc)
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                               "Expected an image file path as only argument.");

                arg_image = argv[optind];
                arg_flags |= DISSECT_IMAGE_READ_ONLY | DISSECT_IMAGE_REQUIRE_ROOT;
                break;

        case ACTION_COPY_FROM:
                if (argc < optind + 2 || argc > optind + 3)
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                               "Expected an image file path, a source path and an optional destination path as only arguments.");

                arg_image = argv[optind];
                arg_source = argv[optind + 1];
                arg_target = argc > optind + 2 ? argv[optind + 2] : "-" /* this means stdout */ ;

                arg_flags |= DISSECT_IMAGE_READ_ONLY | DISSECT_IMAGE_REQUIRE_ROOT;
                break;

        case ACTION_COPY_TO:
                if (argc < optind + 2 || argc > optind + 3)
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                               "Expected an image file path, an optional source path and a destination path as only arguments.");

                arg_image = argv[optind];

                if (argc > optind + 2) {
                        arg_source = argv[optind + 1];
                        arg_target = argv[optind + 2];
                } else {
                        arg_source = "-"; /* this means stdin */
                        arg_target = argv[optind + 1];
                }

                arg_flags |= DISSECT_IMAGE_REQUIRE_ROOT;
                break;

        case ACTION_WITH:
                if (optind >= argc)
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                               "Expected an image file path and an optional command line.");

                arg_image = argv[optind];
                if (argc > optind + 1) {
                        arg_argv = strv_copy(argv + optind + 1);
                        if (!arg_argv)
                                return log_oom();
                }

                break;

        case ACTION_DISCOVER:
                if (optind != argc)
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                               "Expected no argument.");

                break;

        default:
                assert_not_reached();
        }

        return 1;
}

static int strv_pair_to_json(char **l, JsonVariant **ret) {
        _cleanup_strv_free_ char **jl = NULL;

        STRV_FOREACH_PAIR(a, b, l) {
                char *j;

                j = strjoin(*a, "=", *b);
                if (!j)
                        return log_oom();

                if (strv_consume(&jl, j) < 0)
                        return log_oom();
        }

        return json_variant_new_array_strv(ret, jl);
}

static void strv_pair_print(char **l, const char *prefix) {
        assert(prefix);

        STRV_FOREACH_PAIR(p, q, l)
                if (p == l)
                        printf("%s %s=%s\n", prefix, *p, *q);
                else
                        printf("%*s %s=%s\n", (int) strlen(prefix), "", *p, *q);
}

static int get_sysext_scopes(DissectedImage *m, char ***ret_scopes) {
        _cleanup_strv_free_ char **l = NULL;
        const char *e;

        assert(m);
        assert(ret_scopes);

        /* If there's no extension-release file its not a system extension. Otherwise the SYSEXT_SCOPE field
         * indicates which scope it is for — and it defaults to "system" + "portable" if unset. */

        if (!m->extension_release) {
                *ret_scopes = NULL;
                return 0;
        }

        e = strv_env_pairs_get(m->extension_release, "SYSEXT_SCOPE");
        if (e)
                l = strv_split(e, WHITESPACE);
        else
                l = strv_new("system", "portable");
        if (!l)
                return -ENOMEM;

        *ret_scopes = TAKE_PTR(l);
        return 1;
}

static int action_dissect(DissectedImage *m, LoopDevice *d) {
        _cleanup_(json_variant_unrefp) JsonVariant *v = NULL;
        _cleanup_(table_unrefp) Table *t = NULL;
        _cleanup_free_ char *bn = NULL;
        uint64_t size = UINT64_MAX;
        int r;

        assert(m);
        assert(d);

        r = path_extract_filename(arg_image, &bn);
        if (r < 0)
                return log_error_errno(r, "Failed to extract file name from image path '%s': %m", arg_image);

        if (arg_json_format_flags & (JSON_FORMAT_OFF|JSON_FORMAT_PRETTY|JSON_FORMAT_PRETTY_AUTO))
                pager_open(arg_pager_flags);

        if (arg_json_format_flags & JSON_FORMAT_OFF)
                printf("      Name: %s%s%s\n", ansi_highlight(), bn, ansi_normal());

        if (ioctl(d->fd, BLKGETSIZE64, &size) < 0)
                log_debug_errno(errno, "Failed to query size of loopback device: %m");
        else if (arg_json_format_flags & JSON_FORMAT_OFF)
                printf("      Size: %s\n", FORMAT_BYTES(size));

        printf(" Sec. Size: %" PRIu32 "\n", m->sector_size);

        if (arg_json_format_flags & JSON_FORMAT_OFF)
                putc('\n', stdout);

        fflush(stdout);

        r = dissected_image_acquire_metadata(m, 0);
        if (r == -ENXIO)
                return log_error_errno(r, "No root partition discovered.");
        if (r == -EUCLEAN)
                return log_error_errno(r, "File system check of image failed.");
        if (r == -EMEDIUMTYPE)
                log_warning_errno(r, "Not a valid OS image, no os-release file included. Proceeding anyway.");
        else if (r == -EUNATCH)
                log_warning_errno(r, "OS image is encrypted, proceeding without showing OS image metadata.");
        else if (r == -EBUSY)
                log_warning_errno(r, "OS image is currently in use, proceeding without showing OS image metadata.");
        else if (r < 0)
                return log_error_errno(r, "Failed to acquire image metadata: %m");
        else if (arg_json_format_flags & JSON_FORMAT_OFF) {
                _cleanup_strv_free_ char **sysext_scopes = NULL;

                if (!sd_id128_is_null(m->image_uuid))
                        printf("Image UUID: %s\n", SD_ID128_TO_UUID_STRING(m->image_uuid));

                if (m->hostname)
                        printf("  Hostname: %s\n", m->hostname);

                if (!sd_id128_is_null(m->machine_id))
                        printf("Machine ID: " SD_ID128_FORMAT_STR "\n", SD_ID128_FORMAT_VAL(m->machine_id));

                strv_pair_print(m->machine_info,
                               "Mach. Info:");
                strv_pair_print(m->os_release,
                               "OS Release:");
                strv_pair_print(m->initrd_release,
                                "initrd R.:");
                strv_pair_print(m->extension_release,
                               " Ext. Rel.:");

                if (m->hostname ||
                    !sd_id128_is_null(m->machine_id) ||
                    !strv_isempty(m->machine_info) ||
                    !strv_isempty(m->os_release) ||
                    !strv_isempty(m->initrd_release) ||
                    !strv_isempty(m->extension_release))
                        putc('\n', stdout);

                printf("    Use As: %s bootable system for UEFI\n", COLOR_MARK_BOOL(m->partitions[PARTITION_ESP].found));

                if (m->has_init_system >= 0)
                        printf("            %s bootable system for container\n", COLOR_MARK_BOOL(m->has_init_system));

                printf("            %s portable service\n",
                       COLOR_MARK_BOOL(strv_env_pairs_get(m->os_release, "PORTABLE_PREFIXES")));
                printf("            %s initrd\n",
                       COLOR_MARK_BOOL(!strv_isempty(m->initrd_release)));

                r = get_sysext_scopes(m, &sysext_scopes);
                if (r < 0)
                        return log_error_errno(r, "Failed to parse SYSEXT_SCOPE: %m");

                printf("            %s extension for system\n",
                       COLOR_MARK_BOOL(strv_contains(sysext_scopes, "system")));
                printf("            %s extension for initrd\n",
                       COLOR_MARK_BOOL(strv_contains(sysext_scopes, "initrd")));
                printf("            %s extension for portable service\n",
                       COLOR_MARK_BOOL(strv_contains(sysext_scopes, "portable")));

                putc('\n', stdout);
        } else {
                _cleanup_(json_variant_unrefp) JsonVariant *mi = NULL, *osr = NULL, *irdr = NULL, *exr = NULL;
                _cleanup_strv_free_ char **sysext_scopes = NULL;

                if (!strv_isempty(m->machine_info)) {
                        r = strv_pair_to_json(m->machine_info, &mi);
                        if (r < 0)
                                return log_oom();
                }

                if (!strv_isempty(m->os_release)) {
                        r = strv_pair_to_json(m->os_release, &osr);
                        if (r < 0)
                                return log_oom();
                }

                if (!strv_isempty(m->initrd_release)) {
                        r = strv_pair_to_json(m->initrd_release, &irdr);
                        if (r < 0)
                                return log_oom();
                }

                if (!strv_isempty(m->extension_release)) {
                        r = strv_pair_to_json(m->extension_release, &exr);
                        if (r < 0)
                                return log_oom();
                }

                r = get_sysext_scopes(m, &sysext_scopes);
                if (r < 0)
                        return log_error_errno(r, "Failed to parse SYSEXT_SCOPE: %m");

                r = json_build(&v, JSON_BUILD_OBJECT(
                                               JSON_BUILD_PAIR("name", JSON_BUILD_STRING(bn)),
                                               JSON_BUILD_PAIR_CONDITION(!sd_id128_is_null(m->image_uuid), "imageUuid", JSON_BUILD_UUID(m->image_uuid)),
                                               JSON_BUILD_PAIR("size", JSON_BUILD_INTEGER(size)),
                                               JSON_BUILD_PAIR("sectorSize", JSON_BUILD_INTEGER(m->sector_size)),
                                               JSON_BUILD_PAIR_CONDITION(m->hostname, "hostname", JSON_BUILD_STRING(m->hostname)),
                                               JSON_BUILD_PAIR_CONDITION(!sd_id128_is_null(m->machine_id), "machineId", JSON_BUILD_ID128(m->machine_id)),
                                               JSON_BUILD_PAIR_CONDITION(mi, "machineInfo", JSON_BUILD_VARIANT(mi)),
                                               JSON_BUILD_PAIR_CONDITION(osr, "osRelease", JSON_BUILD_VARIANT(osr)),
                                               JSON_BUILD_PAIR_CONDITION(osr, "initrdRelease", JSON_BUILD_VARIANT(irdr)),
                                               JSON_BUILD_PAIR_CONDITION(exr, "extensionRelease", JSON_BUILD_VARIANT(exr)),
                                               JSON_BUILD_PAIR("useBootableUefi", JSON_BUILD_BOOLEAN(m->partitions[PARTITION_ESP].found)),
                                               JSON_BUILD_PAIR_CONDITION(m->has_init_system >= 0, "useBootableContainer", JSON_BUILD_BOOLEAN(m->has_init_system)),
                                               JSON_BUILD_PAIR("useInitrd", JSON_BUILD_BOOLEAN(!strv_isempty(m->initrd_release))),
                                               JSON_BUILD_PAIR("usePortableService", JSON_BUILD_BOOLEAN(strv_env_pairs_get(m->os_release, "PORTABLE_MATCHES"))),
                                               JSON_BUILD_PAIR("useSystemExtension", JSON_BUILD_BOOLEAN(strv_contains(sysext_scopes, "system"))),
                                               JSON_BUILD_PAIR("useInitRDExtension", JSON_BUILD_BOOLEAN(strv_contains(sysext_scopes, "initrd"))),
                                               JSON_BUILD_PAIR("usePortableExtension", JSON_BUILD_BOOLEAN(strv_contains(sysext_scopes, "portable")))));
                if (r < 0)
                        return log_oom();
        }

        t = table_new("rw", "designator", "partition uuid", "partition label", "fstype", "architecture", "verity", "growfs", "node", "partno");
        if (!t)
                return log_oom();

        table_set_ersatz_string(t, TABLE_ERSATZ_DASH);
        (void) table_set_align_percent(t, table_get_cell(t, 0, 7), 100);

        for (PartitionDesignator i = 0; i < _PARTITION_DESIGNATOR_MAX; i++) {
                DissectedPartition *p = m->partitions + i;

                if (!p->found)
                        continue;

                r = table_add_many(
                                t,
                                TABLE_STRING, p->rw ? "rw" : "ro",
                                TABLE_STRING, partition_designator_to_string(i));
                if (r < 0)
                        return table_log_add_error(r);

                if (sd_id128_is_null(p->uuid))
                        r = table_add_cell(t, NULL, TABLE_EMPTY, NULL);
                else
                        r = table_add_cell(t, NULL, TABLE_UUID, &p->uuid);
                if (r < 0)
                        return table_log_add_error(r);

                r = table_add_many(
                                t,
                                TABLE_STRING, p->label,
                                TABLE_STRING, p->fstype,
                                TABLE_STRING, architecture_to_string(p->architecture));
                if (r < 0)
                        return table_log_add_error(r);

                if (arg_verity_settings.data_path)
                        r = table_add_cell(t, NULL, TABLE_STRING, "external");
                else if (dissected_image_verity_candidate(m, i))
                        r = table_add_cell(t, NULL, TABLE_STRING,
                                           dissected_image_verity_sig_ready(m, i) ? "signed" :
                                           yes_no(dissected_image_verity_ready(m, i)));
                else
                        r = table_add_cell(t, NULL, TABLE_EMPTY, NULL);
                if (r < 0)
                        return table_log_add_error(r);

                r = table_add_many(t, TABLE_BOOLEAN, (int) p->growfs);
                if (r < 0)
                        return table_log_add_error(r);

                if (p->partno < 0) /* no partition table, naked file system */ {
                        r = table_add_cell(t, NULL, TABLE_STRING, arg_image);
                        if (r < 0)
                                return table_log_add_error(r);

                        r = table_add_cell(t, NULL, TABLE_EMPTY, NULL);
                } else {
                        r = table_add_cell(t, NULL, TABLE_STRING, p->node);
                        if (r < 0)
                                return table_log_add_error(r);

                        r = table_add_cell(t, NULL, TABLE_INT, &p->partno);
                }
                if (r < 0)
                        return table_log_add_error(r);
        }

        if (arg_json_format_flags & JSON_FORMAT_OFF) {
                (void) table_set_header(t, arg_legend);

                r = table_print(t, NULL);
                if (r < 0)
                        return table_log_print_error(r);
        } else {
                _cleanup_(json_variant_unrefp) JsonVariant *jt = NULL;

                r = table_to_json(t, &jt);
                if (r < 0)
                        return log_error_errno(r, "Failed to convert table to JSON: %m");

                r = json_variant_set_field(&v, "mounts", jt);
                if (r < 0)
                        return log_oom();

                json_variant_dump(v, arg_json_format_flags, stdout, NULL);
        }

        return 0;
}

static int action_mount(DissectedImage *m, LoopDevice *d) {
        int r;

        assert(m);
        assert(d);

        r = dissected_image_decrypt_interactively(
                        m, NULL,
                        &arg_verity_settings,
                        arg_flags);
        if (r < 0)
                return r;

        r = dissected_image_mount_and_warn(m, arg_path, UID_INVALID, UID_INVALID, arg_flags);
        if (r < 0)
                return r;

        r = loop_device_flock(d, LOCK_UN);
        if (r < 0)
                return log_error_errno(r, "Failed to unlock loopback block device: %m");

        r = dissected_image_relinquish(m);
        if (r < 0)
                return log_error_errno(r, "Failed to relinquish DM and loopback block devices: %m");

        return 0;
}

static int list_print_item(
                RecurseDirEvent event,
                const char *path,
                int dir_fd,
                int inode_fd,
                const struct dirent *de,
                const struct statx *sx,
                void *userdata) {

        assert(path);

        if (event == RECURSE_DIR_ENTER)
                printf("%s%s/%s\n", path, ansi_grey(), ansi_normal());
        else if (event == RECURSE_DIR_ENTRY)
                printf("%s\n", path);

        return RECURSE_DIR_CONTINUE;
}

static int get_file_sha256(int inode_fd, uint8_t ret[static SHA256_DIGEST_SIZE]) {
        _cleanup_close_ int fd = -EBADF;
        struct sha256_ctx ctx;

        /* convert O_PATH fd into a regular one */
        fd = fd_reopen(inode_fd, O_RDONLY|O_CLOEXEC);
        if (fd < 0)
                return fd;

        /* Calculating the SHA sum might be slow, hence let's flush STDOUT first, to give user an idea where we are slow. */
        fflush(stdout);

        sha256_init_ctx(&ctx);

        for (;;) {
                uint8_t buffer[64 * 1024];
                ssize_t n;

                n = read(fd, buffer, sizeof(buffer));
                if (n < 0)
                        return -errno;
                if (n == 0)
                        break;

                sha256_process_bytes(buffer, n, &ctx);
        }

        sha256_finish_ctx(&ctx, ret);
        return 0;
}

static int mtree_print_item(
                RecurseDirEvent event,
                const char *path,
                int dir_fd,
                int inode_fd,
                const struct dirent *de,
                const struct statx *sx,
                void *userdata) {

        _cleanup_free_ char *escaped = NULL;
        int r;

        assert(path);

        if (!IN_SET(event, RECURSE_DIR_ENTER, RECURSE_DIR_ENTRY))
                return RECURSE_DIR_CONTINUE;

        assert(sx);

        if (isempty(path))
                path = ".";
        else {
                /* BSD mtree uses either C or octal escaping, and covers whitespace, comments and glob characters. We use C style escaping and follow suit */
                path = escaped = xescape(path, WHITESPACE COMMENTS GLOB_CHARS);
                if (!escaped)
                        return log_oom();
        }

        printf("%s", isempty(path) ? "." : path);

        if (FLAGS_SET(sx->stx_mask, STATX_TYPE)) {
                if (S_ISDIR(sx->stx_mode))
                        printf("%s/%s", ansi_grey(), ansi_normal());

                printf(" %stype=%s%s%s%s",
                       ansi_grey(),
                       ansi_normal(),
                       S_ISDIR(sx->stx_mode) ? ansi_highlight_blue() :
                       S_ISLNK(sx->stx_mode) ? ansi_highlight_cyan() :
                       (S_ISFIFO(sx->stx_mode) || S_ISCHR(sx->stx_mode) || S_ISBLK(sx->stx_mode)) ? ansi_highlight_yellow4() :
                       S_ISSOCK(sx->stx_mode) ? ansi_highlight_magenta() : "",
                       ASSERT_PTR(S_ISDIR(sx->stx_mode) ? "dir" :
                                  S_ISREG(sx->stx_mode) ? "file" :
                                  S_ISLNK(sx->stx_mode) ? "link" :
                                  S_ISFIFO(sx->stx_mode) ? "fifo" :
                                  S_ISBLK(sx->stx_mode) ? "block" :
                                  S_ISCHR(sx->stx_mode) ? "char" :
                                  S_ISSOCK(sx->stx_mode) ? "socket" : NULL),
                       ansi_normal());
        }

        if (FLAGS_SET(sx->stx_mask, STATX_MODE) && (!FLAGS_SET(sx->stx_mask, STATX_TYPE) || !S_ISLNK(sx->stx_mode)))
                printf(" %smode=%s%04o",
                       ansi_grey(),
                       ansi_normal(),
                       (unsigned) (sx->stx_mode & 0777));

        if (FLAGS_SET(sx->stx_mask, STATX_UID))
                printf(" %suid=%s" UID_FMT,
                       ansi_grey(),
                       ansi_normal(),
                       sx->stx_uid);

        if (FLAGS_SET(sx->stx_mask, STATX_GID))
                printf(" %sgid=%s" GID_FMT,
                       ansi_grey(),
                       ansi_normal(),
                       sx->stx_gid);

        if (FLAGS_SET(sx->stx_mask, STATX_TYPE|STATX_SIZE) && S_ISREG(sx->stx_mode)) {
                printf(" %ssize=%s%" PRIu64,
                       ansi_grey(),
                       ansi_normal(),
                       (uint64_t) sx->stx_size);

                if (inode_fd >= 0 && sx->stx_size > 0) {
                        uint8_t hash[SHA256_DIGEST_SIZE];

                        r = get_file_sha256(inode_fd, hash);
                        if (r < 0)
                                log_warning_errno(r, "Failed to calculate file SHA256 sum for '%s', ignoring: %m", path);
                        else {
                                _cleanup_free_ char *h = NULL;

                                h = hexmem(hash, sizeof(hash));
                                if (!h)
                                        return log_oom();

                                printf(" %ssha256sum=%s%s",
                                       ansi_grey(),
                                       ansi_normal(),
                                       h);
                        }
                }
        }

        if (FLAGS_SET(sx->stx_mask, STATX_TYPE) && S_ISLNK(sx->stx_mode) && inode_fd >= 0) {
                _cleanup_free_ char *target = NULL;

                r = readlinkat_malloc(inode_fd, "", &target);
                if (r < 0)
                        log_warning_errno(r, "Failed to read symlink '%s', ignoring: %m", path);
                else {
                        _cleanup_free_ char *target_escaped = NULL;

                        target_escaped = xescape(target, WHITESPACE COMMENTS GLOB_CHARS);
                        if (!target_escaped)
                                return log_oom();

                        printf(" %slink=%s%s",
                               ansi_grey(),
                               ansi_normal(),
                               target_escaped);
                }
        }

        if (FLAGS_SET(sx->stx_mask, STATX_TYPE) && (S_ISBLK(sx->stx_mode) || S_ISCHR(sx->stx_mode)))
                printf(" %sdevice=%slinux,%" PRIu64 ",%" PRIu64,
                       ansi_grey(),
                       ansi_normal(),
                       (uint64_t) sx->stx_rdev_major,
                       (uint64_t) sx->stx_rdev_minor);

        printf("\n");

        return RECURSE_DIR_CONTINUE;
}

static int action_list_or_mtree_or_copy(DissectedImage *m, LoopDevice *d) {
        _cleanup_(umount_and_rmdir_and_freep) char *mounted_dir = NULL;
        _cleanup_(rmdir_and_freep) char *created_dir = NULL;
        _cleanup_free_ char *temp = NULL;
        int r;

        assert(m);
        assert(d);

        r = dissected_image_decrypt_interactively(
                        m, NULL,
                        &arg_verity_settings,
                        arg_flags);
        if (r < 0)
                return r;

        r = detach_mount_namespace();
        if (r < 0)
                return log_error_errno(r, "Failed to detach mount namespace: %m");

        r = tempfn_random_child(NULL, program_invocation_short_name, &temp);
        if (r < 0)
                return log_error_errno(r, "Failed to generate temporary mount directory: %m");

        r = mkdir_p(temp, 0700);
        if (r < 0)
                return log_error_errno(r, "Failed to create mount point: %m");

        created_dir = TAKE_PTR(temp);

        r = dissected_image_mount_and_warn(m, created_dir, UID_INVALID, UID_INVALID, arg_flags);
        if (r < 0)
                return r;

        mounted_dir = TAKE_PTR(created_dir);

        r = loop_device_flock(d, LOCK_UN);
        if (r < 0)
                return log_error_errno(r, "Failed to unlock loopback block device: %m");

        r = dissected_image_relinquish(m);
        if (r < 0)
                return log_error_errno(r, "Failed to relinquish DM and loopback block devices: %m");

        if (arg_action == ACTION_COPY_FROM) {
                _cleanup_close_ int source_fd = -EBADF, target_fd = -EBADF;

                source_fd = chase_symlinks_and_open(arg_source, mounted_dir, CHASE_PREFIX_ROOT|CHASE_WARN, O_RDONLY|O_CLOEXEC|O_NOCTTY, NULL);
                if (source_fd < 0)
                        return log_error_errno(source_fd, "Failed to open source path '%s' in image '%s': %m", arg_source, arg_image);

                /* Copying to stdout? */
                if (streq(arg_target, "-")) {
                        r = copy_bytes(source_fd, STDOUT_FILENO, UINT64_MAX, COPY_REFLINK);
                        if (r < 0)
                                return log_error_errno(r, "Failed to copy bytes from %s in mage '%s' to stdout: %m", arg_source, arg_image);

                        /* When we copy to stdout we don't copy any attributes (i.e. no access mode, no ownership, no xattr, no times) */
                        return 0;
                }

                /* Try to copy as directory? */
                r = copy_directory_fd(source_fd, arg_target, COPY_REFLINK|COPY_MERGE_EMPTY|COPY_SIGINT|COPY_HARDLINKS);
                if (r >= 0)
                        return 0;
                if (r != -ENOTDIR)
                        return log_error_errno(r, "Failed to copy %s in image '%s' to '%s': %m", arg_source, arg_image, arg_target);

                r = fd_verify_regular(source_fd);
                if (r == -EISDIR)
                        return log_error_errno(r, "Target '%s' exists already and is not a directory.", arg_target);
                if (r < 0)
                        return log_error_errno(r, "Source path %s in image '%s' is neither regular file nor directory, refusing: %m", arg_source, arg_image);

                /* Nah, it's a plain file! */
                target_fd = open(arg_target, O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOCTTY|O_NOFOLLOW, 0600);
                if (target_fd < 0)
                        return log_error_errno(errno, "Failed to create regular file at target path '%s': %m", arg_target);

                r = copy_bytes(source_fd, target_fd, UINT64_MAX, COPY_REFLINK);
                if (r < 0)
                        return log_error_errno(r, "Failed to copy bytes from %s in mage '%s' to '%s': %m", arg_source, arg_image, arg_target);

                (void) copy_xattr(source_fd, NULL, target_fd, NULL, 0);
                (void) copy_access(source_fd, target_fd);
                (void) copy_times(source_fd, target_fd, 0);

                /* When this is a regular file we don't copy ownership! */

        } else if (arg_action == ACTION_COPY_TO) {
                _cleanup_close_ int source_fd = -EBADF, target_fd = -EBADF, dfd = -EBADF;
                _cleanup_free_ char *dn = NULL, *bn = NULL;
                bool is_dir;

                r = path_extract_directory(arg_target, &dn);
                if (r < 0)
                        return log_error_errno(r, "Failed to extract directory from target path '%s': %m", arg_target);
                r = path_extract_filename(arg_target, &bn);
                if (r < 0)
                        return log_error_errno(r, "Failed to extract filename from target path '%s': %m", arg_target);
                is_dir = r == O_DIRECTORY;

                r = chase_symlinks(dn, mounted_dir, CHASE_PREFIX_ROOT|CHASE_WARN, NULL, &dfd);
                if (r < 0)
                        return log_error_errno(r, "Failed to open '%s': %m", dn);

                /* Are we reading from stdin? */
                if (streq(arg_source, "-")) {
                        if (is_dir)
                                return log_error_errno(SYNTHETIC_ERRNO(EISDIR), "Cannot copy STDIN to a directory, refusing.");

                        target_fd = openat(dfd, bn, O_WRONLY|O_CREAT|O_CLOEXEC|O_NOCTTY|O_EXCL, 0644);
                        if (target_fd < 0)
                                return log_error_errno(errno, "Failed to open target file '%s': %m", arg_target);

                        r = copy_bytes(STDIN_FILENO, target_fd, UINT64_MAX, COPY_REFLINK);
                        if (r < 0)
                                return log_error_errno(r, "Failed to copy bytes from stdin to '%s' in image '%s': %m", arg_target, arg_image);

                        /* When we copy from stdin we don't copy any attributes (i.e. no access mode, no ownership, no xattr, no times) */
                        return 0;
                }

                source_fd = open(arg_source, O_RDONLY|O_CLOEXEC|O_NOCTTY);
                if (source_fd < 0)
                        return log_error_errno(source_fd, "Failed to open source path '%s': %m", arg_source);

                r = fd_verify_regular(source_fd);
                if (r < 0) {
                        if (r != -EISDIR)
                                return log_error_errno(r, "Source '%s' is neither regular file nor directory: %m", arg_source);

                        /* We are looking at a directory. */

                        target_fd = openat(dfd, bn, O_RDONLY|O_DIRECTORY|O_CLOEXEC);
                        if (target_fd < 0) {
                                if (errno != ENOENT)
                                        return log_error_errno(errno, "Failed to open destination '%s': %m", arg_target);

                                r = copy_tree_at(source_fd, ".", dfd, bn, UID_INVALID, GID_INVALID, COPY_REFLINK|COPY_REPLACE|COPY_SIGINT|COPY_HARDLINKS, NULL);
                        } else
                                r = copy_tree_at(source_fd, ".", target_fd, ".", UID_INVALID, GID_INVALID, COPY_REFLINK|COPY_REPLACE|COPY_SIGINT|COPY_HARDLINKS, NULL);
                        if (r < 0)
                                return log_error_errno(r, "Failed to copy '%s' to '%s' in image '%s': %m", arg_source, arg_target, arg_image);

                        return 0;
                }

                if (is_dir)
                        return log_error_errno(SYNTHETIC_ERRNO(EISDIR), "Source is a regular file, but target is not, refusing.");

                /* We area looking at a regular file */
                target_fd = openat(dfd, bn, O_WRONLY|O_CREAT|O_CLOEXEC|O_NOCTTY|O_EXCL, 0600);
                if (target_fd < 0)
                        return log_error_errno(errno, "Failed to open target file '%s': %m", arg_target);

                r = copy_bytes(source_fd, target_fd, UINT64_MAX, COPY_REFLINK);
                if (r < 0)
                        return log_error_errno(r, "Failed to copy bytes from '%s' to '%s' in image '%s': %m", arg_source, arg_target, arg_image);

                (void) copy_xattr(source_fd, NULL, target_fd, NULL, 0);
                (void) copy_access(source_fd, target_fd);
                (void) copy_times(source_fd, target_fd, 0);

                /* When this is a regular file we don't copy ownership! */

        } else {
                _cleanup_close_ int dfd = -EBADF;

                dfd = open(mounted_dir, O_DIRECTORY|O_CLOEXEC|O_RDONLY);
                if (dfd < 0)
                        return log_error_errno(errno, "Failed to open mount directory: %m");

                pager_open(arg_pager_flags);

                if (arg_action == ACTION_LIST)
                        r = recurse_dir(dfd, NULL, 0, UINT_MAX, RECURSE_DIR_SORT, list_print_item, NULL);
                else if (arg_action == ACTION_MTREE)
                        r = recurse_dir(dfd, ".", STATX_TYPE|STATX_MODE|STATX_UID|STATX_GID|STATX_SIZE, UINT_MAX, RECURSE_DIR_SORT|RECURSE_DIR_INODE_FD|RECURSE_DIR_TOPLEVEL, mtree_print_item, NULL);
                else
                        assert_not_reached();
                if (r < 0)
                        return log_error_errno(r, "Failed to list image: %m");
        }

        return 0;
}

static int action_umount(const char *path) {
        _cleanup_close_ int fd = -EBADF;
        _cleanup_free_ char *canonical = NULL;
        _cleanup_(loop_device_unrefp) LoopDevice *d = NULL;
        _cleanup_(sd_device_unrefp) sd_device *dev = NULL;
        int r;

        fd = chase_symlinks_and_open(path, NULL, 0, O_DIRECTORY, &canonical);
        if (fd == -ENOTDIR)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTDIR), "'%s' is not a directory", path);
        if (fd < 0)
                return log_error_errno(fd, "Failed to resolve path '%s': %m", path);

        r = fd_is_mount_point(fd, NULL, 0);
        if (r == 0)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "'%s' is not a mount point", canonical);
        if (r < 0)
                return log_error_errno(r, "Failed to determine whether '%s' is a mount point: %m", canonical);

        r = block_device_new_from_fd(fd, BLOCK_DEVICE_LOOKUP_WHOLE_DISK | BLOCK_DEVICE_LOOKUP_BACKING, &dev);
        if (r < 0) {
                _cleanup_close_ int usr_fd = -EBADF;

                /* The command `systemd-dissect --mount` expects that the image at least has the root or /usr
                 * partition. If it does not have the root partition, then we mount the /usr partition on a
                 * tmpfs. Hence, let's try to find the backing block device through the /usr partition. */

                usr_fd = openat(fd, "usr", O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
                if (usr_fd < 0)
                        return log_error_errno(errno, "Failed to open '%s/usr': %m", canonical);

                r = block_device_new_from_fd(usr_fd, BLOCK_DEVICE_LOOKUP_WHOLE_DISK | BLOCK_DEVICE_LOOKUP_BACKING, &dev);
        }
        if (r < 0)
                return log_error_errno(r, "Failed to find backing block device for '%s': %m", canonical);

        r = loop_device_open(dev, 0, LOCK_EX, &d);
        if (r < 0)
                return log_device_error_errno(dev, r, "Failed to open loopback block device: %m");

        /* We've locked the loop device, now we're ready to unmount. To allow the unmount to succeed, we have
         * to close the O_PATH fd we opened earlier. */
        fd = safe_close(fd);

        r = umount_recursive(canonical, 0);
        if (r < 0)
                return log_error_errno(r, "Failed to unmount '%s': %m", canonical);

        /* We managed to lock and unmount successfully? That means we can try to remove the loop device. */
        loop_device_unrelinquish(d);

        if (arg_rmdir) {
                r = RET_NERRNO(rmdir(canonical));
                if (r < 0)
                        return log_error_errno(r, "Failed to remove mount directory '%s': %m", canonical);
        }

        return 0;
}

static int action_with(DissectedImage *m, LoopDevice *d) {
        _cleanup_(umount_and_rmdir_and_freep) char *mounted_dir = NULL;
        _cleanup_(rmdir_and_freep) char *created_dir = NULL;
        _cleanup_free_ char *temp = NULL;
        int r, rcode;

        r = dissected_image_decrypt_interactively(
                        m, NULL,
                        &arg_verity_settings,
                        arg_flags);
        if (r < 0)
                return r;

        r = tempfn_random_child(NULL, program_invocation_short_name, &temp);
        if (r < 0)
                return log_error_errno(r, "Failed to generate temporary mount directory: %m");

        r = mkdir_p(temp, 0700);
        if (r < 0)
                return log_error_errno(r, "Failed to create mount point: %m");

        created_dir = TAKE_PTR(temp);

        r = dissected_image_mount_and_warn(m, created_dir, UID_INVALID, UID_INVALID, arg_flags);
        if (r < 0)
                return r;

        mounted_dir = TAKE_PTR(created_dir);

        r = dissected_image_relinquish(m);
        if (r < 0)
                return log_error_errno(r, "Failed to relinquish DM and loopback block devices: %m");

        r = loop_device_flock(d, LOCK_UN);
        if (r < 0)
                return log_error_errno(r, "Failed to unlock loopback block device: %m");

        rcode = safe_fork("(with)", FORK_CLOSE_ALL_FDS|FORK_LOG|FORK_WAIT, NULL);
        if (rcode == 0) {
                /* Child */

                if (chdir(mounted_dir) < 0) {
                        log_error_errno(errno, "Failed to change to '%s' directory: %m", mounted_dir);
                        _exit(EXIT_FAILURE);
                }

                if (setenv("SYSTEMD_DISSECT_ROOT", mounted_dir, /* overwrite= */ true) < 0) {
                        log_error_errno(errno, "Failed to set $SYSTEMD_DISSECT_ROOT: %m");
                        _exit(EXIT_FAILURE);
                }

                if (strv_isempty(arg_argv)) {
                        const char *sh;

                        sh = secure_getenv("SHELL");
                        if (sh) {
                                execvp(sh, STRV_MAKE(sh));
                                log_warning_errno(errno, "Failed to execute $SHELL, falling back to /bin/sh: %m");
                        }

                        execl("/bin/sh", "sh", NULL);
                        log_error_errno(errno, "Failed to invoke /bin/sh: %m");
                } else {
                        execvp(arg_argv[0], arg_argv);
                        log_error_errno(errno, "Failed to execute '%s': %m", arg_argv[0]);
                }

                _exit(EXIT_FAILURE);
        }

        /* Let's manually detach everything, to make things synchronous */
        r = loop_device_flock(d, LOCK_SH);
        if (r < 0)
                log_warning_errno(r, "Failed to lock loopback block device, ignoring: %m");

        r = umount_recursive(mounted_dir, 0);
        if (r < 0)
                log_warning_errno(r, "Failed to unmount '%s', ignoring: %m", mounted_dir);
        else
                loop_device_unrelinquish(d); /* Let's try to destroy the loopback device */

        created_dir = TAKE_PTR(mounted_dir);

        if (rmdir(created_dir) < 0)
                log_warning_errno(r, "Failed to remove directory '%s', ignoring: %m", created_dir);

        temp = TAKE_PTR(created_dir);

        return rcode;
}

static int action_discover(void) {
        _cleanup_(hashmap_freep) Hashmap *images = NULL;
        _cleanup_(table_unrefp) Table *t = NULL;
        Image *img;
        int r;

        images = hashmap_new(&image_hash_ops);
        if (!images)
                return log_oom();

        for (ImageClass cl = 0; cl < _IMAGE_CLASS_MAX; cl++) {
                r = image_discover(cl, NULL, images);
                if (r < 0)
                        return log_error_errno(r, "Failed to discover images: %m");
        }

        if ((arg_json_format_flags & JSON_FORMAT_OFF) && hashmap_isempty(images)) {
                log_info("No images found.");
                return 0;
        }

        t = table_new("name", "type", "class", "ro", "path", "time", "usage");
        if (!t)
                return log_oom();

        HASHMAP_FOREACH(img, images) {

                if (!IN_SET(img->type, IMAGE_RAW, IMAGE_BLOCK))
                        continue;

                r = table_add_many(
                                t,
                                TABLE_STRING, img->name,
                                TABLE_STRING, image_type_to_string(img->type),
                                TABLE_STRING, image_class_to_string(img->class),
                                TABLE_BOOLEAN, img->read_only,
                                TABLE_PATH, img->path,
                                TABLE_TIMESTAMP, img->mtime != 0 ? img->mtime : img->crtime,
                                TABLE_SIZE, img->usage);
                if (r < 0)
                        return table_log_add_error(r);
        }

        (void) table_set_sort(t, (size_t) 0);

        return table_print_with_pager(t, arg_json_format_flags, arg_pager_flags, arg_legend);
}

static int run(int argc, char *argv[]) {
        _cleanup_(dissected_image_unrefp) DissectedImage *m = NULL;
        _cleanup_(loop_device_unrefp) LoopDevice *d = NULL;
        uint32_t loop_flags;
        int open_flags, r;

        log_setup();

        r = parse_argv(argc, argv);
        if (r <= 0)
                return r;

        if (arg_action == ACTION_UMOUNT)
                return action_umount(arg_path);
        if (arg_action == ACTION_DISCOVER)
                return action_discover();

        r = verity_settings_load(
                        &arg_verity_settings,
                        arg_image, NULL, NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to read verity artifacts for %s: %m", arg_image);

        if (arg_verity_settings.data_path)
                arg_flags |= DISSECT_IMAGE_NO_PARTITION_TABLE; /* We only support Verity per file system,
                                                                * hence if there's external Verity data
                                                                * available we turn off partition table
                                                                * support */

        open_flags = FLAGS_SET(arg_flags, DISSECT_IMAGE_DEVICE_READ_ONLY) ? O_RDONLY : O_RDWR;
        loop_flags = FLAGS_SET(arg_flags, DISSECT_IMAGE_NO_PARTITION_TABLE) ? 0 : LO_FLAGS_PARTSCAN;

        if (arg_in_memory)
                r = loop_device_make_by_path_memory(arg_image, open_flags, /* sector_size= */ UINT32_MAX, loop_flags, LOCK_SH, &d);
        else
                r = loop_device_make_by_path(arg_image, open_flags, /* sector_size= */ UINT32_MAX, loop_flags, LOCK_SH, &d);
        if (r < 0)
                return log_error_errno(r, "Failed to set up loopback device for %s: %m", arg_image);

        r = dissect_loop_device_and_warn(
                        d,
                        &arg_verity_settings,
                        NULL,
                        arg_flags,
                        &m);
        if (r < 0)
                return r;

        r = dissected_image_load_verity_sig_partition(
                        m,
                        d->fd,
                        &arg_verity_settings);
        if (r < 0)
                return log_error_errno(r, "Failed to load verity signature partition: %m");

        switch (arg_action) {

        case ACTION_DISSECT:
                r = action_dissect(m, d);
                break;

        case ACTION_MOUNT:
                r = action_mount(m, d);
                break;

        case ACTION_LIST:
        case ACTION_MTREE:
        case ACTION_COPY_FROM:
        case ACTION_COPY_TO:
                r = action_list_or_mtree_or_copy(m, d);
                break;

        case ACTION_WITH:
                r = action_with(m, d);
                break;

        default:
                assert_not_reached();
        }

        return r;
}

DEFINE_MAIN_FUNCTION(run);
