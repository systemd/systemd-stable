/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <stdlib.h>
#include <unistd.h>

#include "sd-device.h"
#include "sd-id128.h"

#include "alloc-util.h"
#include "blkid-util.h"
#include "blockdev-util.h"
#include "btrfs-util.h"
#include "device-util.h"
#include "dirent-util.h"
#include "dissect-image.h"
#include "dropin.h"
#include "efi-loader.h"
#include "fd-util.h"
#include "fileio.h"
#include "fs-util.h"
#include "fstab-util.h"
#include "generator.h"
#include "gpt.h"
#include "mkdir.h"
#include "mountpoint-util.h"
#include "parse-util.h"
#include "path-util.h"
#include "proc-cmdline.h"
#include "special.h"
#include "specifier.h"
#include "stat-util.h"
#include "string-util.h"
#include "strv.h"
#include "unit-name.h"
#include "util.h"
#include "virt.h"

static const char *arg_dest = NULL;
static bool arg_enabled = true;
static bool arg_root_enabled = true;
static int arg_root_rw = -1;

static int open_parent_block_device(dev_t devnum, int *ret_fd) {
        _cleanup_(sd_device_unrefp) sd_device *d = NULL;
        const char *name, *devtype, *node;
        sd_device *parent;
        dev_t pn;
        int fd, r;

        assert(ret_fd);

        r = sd_device_new_from_devnum(&d, 'b', devnum);
        if (r < 0)
                return log_debug_errno(r, "Failed to open device: %m");

        if (sd_device_get_devname(d, &name) < 0) {
                r = sd_device_get_syspath(d, &name);
                if (r < 0) {
                        log_device_debug_errno(d, r, "Device %u:%u does not have a name, ignoring: %m",
                                               major(devnum), minor(devnum));
                        return 0;
                }
        }

        r = sd_device_get_parent(d, &parent);
        if (r < 0) {
                log_device_debug_errno(d, r, "Not a partitioned device, ignoring: %m");
                return 0;
        }

        /* Does it have a devtype? */
        r = sd_device_get_devtype(parent, &devtype);
        if (r < 0) {
                log_device_debug_errno(parent, r, "Parent doesn't have a device type, ignoring: %m");
                return 0;
        }

        /* Is this a disk or a partition? We only care for disks... */
        if (!streq(devtype, "disk")) {
                log_device_debug(parent, "Parent isn't a raw disk, ignoring.");
                return 0;
        }

        /* Does it have a device node? */
        r = sd_device_get_devname(parent, &node);
        if (r < 0) {
                log_device_debug_errno(parent, r, "Parent device does not have device node, ignoring: %m");
                return 0;
        }

        log_device_debug(d, "Root device %s.", node);

        r = sd_device_get_devnum(parent, &pn);
        if (r < 0) {
                log_device_debug_errno(parent, r, "Parent device is not a proper block device, ignoring: %m");
                return 0;
        }

        fd = open(node, O_RDONLY|O_CLOEXEC|O_NOCTTY);
        if (fd < 0)
                return log_error_errno(errno, "Failed to open %s: %m", node);

        *ret_fd = fd;
        return 1;
}

static int add_cryptsetup(const char *id, const char *what, bool rw, bool require, char **device) {
#if HAVE_LIBCRYPTSETUP
        _cleanup_free_ char *e = NULL, *n = NULL, *d = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        assert(id);
        assert(what);

        r = unit_name_from_path(what, ".device", &d);
        if (r < 0)
                return log_error_errno(r, "Failed to generate unit name: %m");

        e = unit_name_escape(id);
        if (!e)
                return log_oom();

        r = unit_name_build("systemd-cryptsetup", e, ".service", &n);
        if (r < 0)
                return log_error_errno(r, "Failed to generate unit name: %m");

        r = generator_open_unit_file(arg_dest, NULL, n, &f);
        if (r < 0)
                return r;

        r = generator_write_cryptsetup_unit_section(f, NULL);
        if (r < 0)
                return r;

        fprintf(f,
                "Before=umount.target cryptsetup.target\n"
                "Conflicts=umount.target\n"
                "BindsTo=%s\n"
                "After=%s\n",
                d, d);

        r = generator_write_cryptsetup_service_section(f, id, what, NULL, rw ? NULL : "read-only");
        if (r < 0)
                return r;

        r = fflush_and_check(f);
        if (r < 0)
                return log_error_errno(r, "Failed to write file %s: %m", n);

        r = generator_add_symlink(arg_dest, d, "wants", n);
        if (r < 0)
                return r;

        const char *dmname;
        dmname = strjoina("dev-mapper-", e, ".device");

        if (require) {
                r = generator_add_symlink(arg_dest, "cryptsetup.target", "requires", n);
                if (r < 0)
                        return r;

                r = generator_add_symlink(arg_dest, dmname, "requires", n);
                if (r < 0)
                        return r;
        }

        r = write_drop_in_format(arg_dest, dmname, 50, "job-timeout",
                                 "# Automatically generated by systemd-gpt-auto-generator\n\n"
                                 "[Unit]\n"
                                 "JobTimeoutSec=0"); /* the binary handles timeouts anyway */
        if (r < 0)
                log_warning_errno(r, "Failed to write device timeout drop-in, ignoring: %m");

        if (device) {
                char *ret;

                ret = path_join("/dev/mapper", id);
                if (!ret)
                        return log_oom();

                *device = ret;
        }

        return 0;
#else
        return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP), "Partition is encrypted, but the project was compiled without libcryptsetup support");
#endif
}

static int add_mount(
                const char *id,
                const char *what,
                const char *where,
                const char *fstype,
                bool rw,
                bool growfs,
                const char *options,
                const char *description,
                const char *post) {

        _cleanup_free_ char *unit = NULL, *crypto_what = NULL, *p = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        /* Note that we don't apply specifier escaping on the input strings here, since we know they are not configured
         * externally, but all originate from our own sources here, and hence we know they contain no % characters that
         * could potentially be understood as specifiers. */

        assert(id);
        assert(what);
        assert(where);
        assert(description);

        log_debug("Adding %s: %s fstype=%s", where, what, fstype ?: "(any)");

        if (streq_ptr(fstype, "crypto_LUKS")) {
                r = add_cryptsetup(id, what, rw, true, &crypto_what);
                if (r < 0)
                        return r;

                what = crypto_what;
                fstype = NULL;
        }

        r = unit_name_from_path(where, ".mount", &unit);
        if (r < 0)
                return log_error_errno(r, "Failed to generate unit name: %m");

        p = path_join(empty_to_root(arg_dest), unit);
        if (!p)
                return log_oom();

        f = fopen(p, "wxe");
        if (!f)
                return log_error_errno(errno, "Failed to create unit file %s: %m", unit);

        fprintf(f,
                "# Automatically generated by systemd-gpt-auto-generator\n\n"
                "[Unit]\n"
                "Description=%s\n"
                "Documentation=man:systemd-gpt-auto-generator(8)\n",
                description);

        if (post)
                fprintf(f, "Before=%s\n", post);

        r = generator_write_fsck_deps(f, arg_dest, what, where, fstype);
        if (r < 0)
                return r;

        r = generator_write_blockdev_dependency(f, what);
        if (r < 0)
                return r;

        fprintf(f,
                "\n"
                "[Mount]\n"
                "What=%s\n"
                "Where=%s\n",
                what, where);

        if (fstype)
                fprintf(f, "Type=%s\n", fstype);

        if (options)
                fprintf(f, "Options=%s,%s\n", options, rw ? "rw" : "ro");
        else
                fprintf(f, "Options=%s\n", rw ? "rw" : "ro");

        r = fflush_and_check(f);
        if (r < 0)
                return log_error_errno(r, "Failed to write unit file %s: %m", p);

        if (growfs) {
                r = generator_hook_up_growfs(arg_dest, where, post);
                if (r < 0)
                        return r;
        }

        if (post) {
                r = generator_add_symlink(arg_dest, post, "requires", unit);
                if (r < 0)
                        return r;
        }

        return 0;
}

static int path_is_busy(const char *where) {
        int r;

        /* already a mountpoint; generators run during reload */
        r = path_is_mount_point(where, NULL, AT_SYMLINK_FOLLOW);
        if (r > 0)
                return false;

        /* the directory might not exist on a stateless system */
        if (r == -ENOENT)
                return false;

        if (r < 0)
                return log_warning_errno(r, "Cannot check if \"%s\" is a mount point: %m", where);

        /* not a mountpoint but it contains files */
        r = dir_is_empty(where);
        if (r < 0)
                return log_warning_errno(r, "Cannot check if \"%s\" is empty: %m", where);
        if (r > 0)
                return false;

        log_debug("\"%s\" already populated, ignoring.", where);
        return true;
}

static int add_partition_mount(
                DissectedPartition *p,
                const char *id,
                const char *where,
                const char *description) {

        int r;
        assert(p);

        r = path_is_busy(where);
        if (r != 0)
                return r < 0 ? r : 0;

        return add_mount(
                        id,
                        p->node,
                        where,
                        p->fstype,
                        p->rw,
                        p->growfs,
                        NULL,
                        description,
                        SPECIAL_LOCAL_FS_TARGET);
}

static int add_swap(const char *path) {
        _cleanup_free_ char *name = NULL, *unit = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        assert(path);

        /* Disable the swap auto logic if at least one swap is defined in /etc/fstab, see #6192. */
        r = fstab_has_fstype("swap");
        if (r < 0)
                return log_error_errno(r, "Failed to parse fstab: %m");
        if (r > 0) {
                log_debug("swap specified in fstab, ignoring.");
                return 0;
        }

        log_debug("Adding swap: %s", path);

        r = unit_name_from_path(path, ".swap", &name);
        if (r < 0)
                return log_error_errno(r, "Failed to generate unit name: %m");

        unit = path_join(empty_to_root(arg_dest), name);
        if (!unit)
                return log_oom();

        f = fopen(unit, "wxe");
        if (!f)
                return log_error_errno(errno, "Failed to create unit file %s: %m", unit);

        fprintf(f,
                "# Automatically generated by systemd-gpt-auto-generator\n\n"
                "[Unit]\n"
                "Description=Swap Partition\n"
                "Documentation=man:systemd-gpt-auto-generator(8)\n");

        r = generator_write_blockdev_dependency(f, path);
        if (r < 0)
                return r;

        fprintf(f,
                "\n"
                "[Swap]\n"
                "What=%s\n",
                path);

        r = fflush_and_check(f);
        if (r < 0)
                return log_error_errno(r, "Failed to write unit file %s: %m", unit);

        return generator_add_symlink(arg_dest, SPECIAL_SWAP_TARGET, "wants", name);
}

static int add_automount(
                const char *id,
                const char *what,
                const char *where,
                const char *fstype,
                bool rw,
                bool growfs,
                const char *options,
                const char *description,
                usec_t timeout) {

        _cleanup_free_ char *unit = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        const char *opt = "noauto", *p;
        int r;

        assert(id);
        assert(where);
        assert(description);

        if (options)
                opt = strjoina(options, ",", opt);

        r = add_mount(id,
                      what,
                      where,
                      fstype,
                      rw,
                      growfs,
                      opt,
                      description,
                      NULL);
        if (r < 0)
                return r;

        r = unit_name_from_path(where, ".automount", &unit);
        if (r < 0)
                return log_error_errno(r, "Failed to generate unit name: %m");

        p = prefix_roota(arg_dest, unit);
        f = fopen(p, "wxe");
        if (!f)
                return log_error_errno(errno, "Failed to create unit file %s: %m", unit);

        fprintf(f,
                "# Automatically generated by systemd-gpt-auto-generator\n\n"
                "[Unit]\n"
                "Description=%s\n"
                "Documentation=man:systemd-gpt-auto-generator(8)\n"
                "[Automount]\n"
                "Where=%s\n"
                "TimeoutIdleSec="USEC_FMT"\n",
                description,
                where,
                timeout / USEC_PER_SEC);

        r = fflush_and_check(f);
        if (r < 0)
                return log_error_errno(r, "Failed to write unit file %s: %m", p);

        return generator_add_symlink(arg_dest, SPECIAL_LOCAL_FS_TARGET, "wants", unit);
}

static const char *esp_or_xbootldr_options(const DissectedPartition *p) {
        assert(p);

        /* Discoveried ESP and XBOOTLDR partition are always hardened with "noexec,nosuid,nodev".
         * If we probed vfat or have no idea about the file system then assume these file systems are vfat
         * and thus understand "umask=0077". */

        if (!p->fstype || streq(p->fstype, "vfat"))
                return "umask=0077,noexec,nosuid,nodev";

        return "noexec,nosuid,nodev";
}

static int add_xbootldr(DissectedPartition *p) {
        int r;

        assert(p);

        if (in_initrd()) {
                log_debug("In initrd, ignoring the XBOOTLDR partition.");
                return 0;
        }

        r = fstab_is_mount_point("/boot");
        if (r < 0)
                return log_error_errno(r, "Failed to parse fstab: %m");
        if (r > 0) {
                log_debug("/boot specified in fstab, ignoring XBOOTLDR partition.");
                return 0;
        }

        r = path_is_busy("/boot");
        if (r < 0)
                return r;
        if (r > 0)
                return 0;

        return add_automount("boot",
                             p->node,
                             "/boot",
                             p->fstype,
                             /* rw= */ true,
                             /* growfs= */ false,
                             esp_or_xbootldr_options(p),
                             "Boot Loader Partition",
                             120 * USEC_PER_SEC);
}

#if ENABLE_EFI
static int add_esp(DissectedPartition *p, bool has_xbootldr) {
        const char *esp_path = NULL, *id = NULL;
        int r;

        assert(p);

        if (in_initrd()) {
                log_debug("In initrd, ignoring the ESP.");
                return 0;
        }

        /* If /efi exists we'll use that. Otherwise we'll use /boot, as that's usually the better choice, but
         * only if there's no explicit XBOOTLDR partition around. */
        if (access("/efi", F_OK) < 0) {
                if (errno != ENOENT)
                        return log_error_errno(errno, "Failed to determine whether /efi exists: %m");

                /* Use /boot as fallback, but only if there's no XBOOTLDR partition */
                if (!has_xbootldr) {
                        esp_path = "/boot";
                        id = "boot";
                }
        }
        if (!esp_path)
                esp_path = "/efi";
        if (!id)
                id = "efi";

        /* We create an .automount which is not overridden by the .mount from the fstab generator. */
        r = fstab_is_mount_point(esp_path);
        if (r < 0)
                return log_error_errno(r, "Failed to parse fstab: %m");
        if (r > 0) {
                log_debug("%s specified in fstab, ignoring.", esp_path);
                return 0;
        }

        r = path_is_busy(esp_path);
        if (r < 0)
                return r;
        if (r > 0)
                return 0;

        if (is_efi_boot()) {
                sd_id128_t loader_uuid;

                /* If this is an EFI boot, be extra careful, and only mount the ESP if it was the ESP used for booting. */

                r = efi_loader_get_device_part_uuid(&loader_uuid);
                if (r == -ENOENT) {
                        log_debug("EFI loader partition unknown.");
                        return 0;
                }
                if (r < 0)
                        return log_error_errno(r, "Failed to read ESP partition UUID: %m");

                if (!sd_id128_equal(p->uuid, loader_uuid)) {
                        log_debug("Partition for %s does not appear to be the partition we are booted from.", p->node);
                        return 0;
                }
        } else
                log_debug("Not an EFI boot, skipping ESP check.");

        return add_automount(id,
                             p->node,
                             esp_path,
                             p->fstype,
                             /* rw= */ true,
                             /* growfs= */ false,
                             esp_or_xbootldr_options(p),
                             "EFI System Partition Automount",
                             120 * USEC_PER_SEC);
}
#else
static int add_esp(DissectedPartition *p, bool has_xbootldr) {
        return 0;
}
#endif

static int add_root_rw(DissectedPartition *p) {
        const char *path;
        int r;

        assert(p);

        if (in_initrd()) {
                log_debug("In initrd, not generating drop-in for systemd-remount-fs.service.");
                return 0;
        }

        if (arg_root_rw >= 0) {
                log_debug("Parameter ro/rw specified on kernel command line, not generating drop-in for systemd-remount-fs.service.");
                return 0;
        }

        if (!p->rw) {
                log_debug("Root partition marked read-only in GPT partition table, not generating drop-in for systemd-remount-fs.service.");
                return 0;
        }

        (void) generator_enable_remount_fs_service(arg_dest);

        path = strjoina(arg_dest, "/systemd-remount-fs.service.d/50-remount-rw.conf");

        r = write_string_file(path,
                              "# Automatically generated by systemd-gpt-generator\n\n"
                              "[Service]\n"
                              "Environment=SYSTEMD_REMOUNT_ROOT_RW=1\n",
                              WRITE_STRING_FILE_CREATE|WRITE_STRING_FILE_NOFOLLOW|WRITE_STRING_FILE_MKDIR_0755);
        if (r < 0)
                return log_error_errno(r, "Failed to write drop-in file %s: %m", path);

        return 0;
}

#if ENABLE_EFI
static int add_root_cryptsetup(void) {

        /* If a device /dev/gpt-auto-root-luks appears, then make it pull in systemd-cryptsetup-root.service, which
         * sets it up, and causes /dev/gpt-auto-root to appear which is all we are looking for. */

        return add_cryptsetup("root", "/dev/gpt-auto-root-luks", true, false, NULL);
}
#endif

static int add_root_mount(void) {
#if ENABLE_EFI
        int r;

        if (!is_efi_boot()) {
                log_debug("Not an EFI boot, not creating root mount.");
                return 0;
        }

        r = efi_loader_get_device_part_uuid(NULL);
        if (r == -ENOENT) {
                log_notice("EFI loader partition unknown, exiting.\n"
                           "(The boot loader did not set EFI variable LoaderDevicePartUUID.)");
                return 0;
        } else if (r < 0)
                return log_error_errno(r, "Failed to read ESP partition UUID: %m");

        /* OK, we have an ESP partition, this is fantastic, so let's
         * wait for a root device to show up. A udev rule will create
         * the link for us under the right name. */

        if (in_initrd()) {
                r = generator_write_initrd_root_device_deps(arg_dest, "/dev/gpt-auto-root");
                if (r < 0)
                        return 0;

                r = add_root_cryptsetup();
                if (r < 0)
                        return r;
        }

        /* Note that we do not need to enable systemd-remount-fs.service here. If
         * /etc/fstab exists, systemd-fstab-generator will pull it in for us. */

        return add_mount(
                        "root",
                        "/dev/gpt-auto-root",
                        in_initrd() ? "/sysroot" : "/",
                        NULL,
                        /* rw= */ arg_root_rw > 0,
                        /* growfs= */ false,
                        NULL,
                        "Root Partition",
                        in_initrd() ? SPECIAL_INITRD_ROOT_FS_TARGET : SPECIAL_LOCAL_FS_TARGET);
#else
        return 0;
#endif
}

static int enumerate_partitions(dev_t devnum) {
        _cleanup_close_ int fd = -1;
        _cleanup_(dissected_image_unrefp) DissectedImage *m = NULL;
        int r, k;

        r = open_parent_block_device(devnum, &fd);
        if (r <= 0)
                return r;

        r = dissect_image(
                        fd,
                        NULL, NULL,
                        UINT64_MAX,
                        USEC_INFINITY,
                        DISSECT_IMAGE_GPT_ONLY|
                        DISSECT_IMAGE_NO_UDEV|
                        DISSECT_IMAGE_USR_NO_ROOT,
                        &m);
        if (r == -ENOPKG) {
                log_debug_errno(r, "No suitable partition table found, ignoring.");
                return 0;
        }
        if (r < 0)
                return log_error_errno(r, "Failed to dissect: %m");

        if (m->partitions[PARTITION_SWAP].found) {
                k = add_swap(m->partitions[PARTITION_SWAP].node);
                if (k < 0)
                        r = k;
        }

        if (m->partitions[PARTITION_XBOOTLDR].found) {
                k = add_xbootldr(m->partitions + PARTITION_XBOOTLDR);
                if (k < 0)
                        r = k;
        }

        if (m->partitions[PARTITION_ESP].found) {
                k = add_esp(m->partitions + PARTITION_ESP, m->partitions[PARTITION_XBOOTLDR].found);
                if (k < 0)
                        r = k;
        }

        if (m->partitions[PARTITION_HOME].found) {
                k = add_partition_mount(m->partitions + PARTITION_HOME, "home", "/home", "Home Partition");
                if (k < 0)
                        r = k;
        }

        if (m->partitions[PARTITION_SRV].found) {
                k = add_partition_mount(m->partitions + PARTITION_SRV, "srv", "/srv", "Server Data Partition");
                if (k < 0)
                        r = k;
        }

        if (m->partitions[PARTITION_VAR].found) {
                k = add_partition_mount(m->partitions + PARTITION_VAR, "var", "/var", "Variable Data Partition");
                if (k < 0)
                        r = k;
        }

        if (m->partitions[PARTITION_TMP].found) {
                k = add_partition_mount(m->partitions + PARTITION_TMP, "var-tmp", "/var/tmp", "Temporary Data Partition");
                if (k < 0)
                        r = k;
        }

        if (m->partitions[PARTITION_ROOT].found) {
                k = add_root_rw(m->partitions + PARTITION_ROOT);
                if (k < 0)
                        r = k;
        }

        return r;
}

static int add_mounts(void) {
        dev_t devno;
        int r;

        r = get_block_device_harder("/", &devno);
        if (r == -EUCLEAN)
                return btrfs_log_dev_root(LOG_ERR, r, "root file system");
        if (r < 0)
                return log_error_errno(r, "Failed to determine block device of root file system: %m");
        if (r == 0) { /* Not backed by block device */
                r = get_block_device_harder("/usr", &devno);
                if (r == -EUCLEAN)
                        return btrfs_log_dev_root(LOG_ERR, r, "/usr");
                if (r < 0)
                        return log_error_errno(r, "Failed to determine block device of /usr file system: %m");
                if (r == 0) {
                        _cleanup_free_ char *p = NULL;
                        mode_t m;

                        /* If the root mount has been replaced by some form of volatile file system (overlayfs), the
                         * original root block device node is symlinked in /run/systemd/volatile-root. Let's read that
                         * here. */
                        r = readlink_malloc("/run/systemd/volatile-root", &p);
                        if (r == -ENOENT) {
                                log_debug("Neither root nor /usr file system are on a (single) block device.");
                                return 0;
                        }
                        if (r < 0)
                                return log_error_errno(r, "Failed to read symlink /run/systemd/volatile-root: %m");

                        r = device_path_parse_major_minor(p, &m, &devno);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse major/minor device node: %m");
                        if (!S_ISBLK(m))
                                return log_error_errno(SYNTHETIC_ERRNO(ENOTBLK), "Volatile root device is of wrong type.");
                }
        }

        return enumerate_partitions(devno);
}

static int parse_proc_cmdline_item(const char *key, const char *value, void *data) {
        int r;

        assert(key);

        if (proc_cmdline_key_streq(key, "systemd.gpt_auto") ||
            proc_cmdline_key_streq(key, "rd.systemd.gpt_auto")) {

                r = value ? parse_boolean(value) : 1;
                if (r < 0)
                        log_warning_errno(r, "Failed to parse gpt-auto switch \"%s\", ignoring: %m", value);
                else
                        arg_enabled = r;

        } else if (proc_cmdline_key_streq(key, "root")) {

                if (proc_cmdline_value_missing(key, value))
                        return 0;

                /* Disable root disk logic if there's a root= value
                 * specified (unless it happens to be "gpt-auto") */

                if (!streq(value, "gpt-auto")) {
                        arg_root_enabled = false;
                        log_debug("Disabling root partition auto-detection, root= is defined.");
                }

        } else if (proc_cmdline_key_streq(key, "roothash")) {

                if (proc_cmdline_value_missing(key, value))
                        return 0;

                /* Disable root disk logic if there's roothash= defined (i.e. verity enabled) */

                arg_root_enabled = false;

        } else if (proc_cmdline_key_streq(key, "rw") && !value)
                arg_root_rw = true;
        else if (proc_cmdline_key_streq(key, "ro") && !value)
                arg_root_rw = false;

        return 0;
}

static int run(const char *dest, const char *dest_early, const char *dest_late) {
        int r, k;

        assert_se(arg_dest = dest_late);

        if (detect_container() > 0) {
                log_debug("In a container, exiting.");
                return 0;
        }

        r = proc_cmdline_parse(parse_proc_cmdline_item, NULL, 0);
        if (r < 0)
                log_warning_errno(r, "Failed to parse kernel command line, ignoring: %m");

        if (!arg_enabled) {
                log_debug("Disabled, exiting.");
                return 0;
        }

        if (arg_root_enabled)
                r = add_root_mount();

        if (!in_initrd()) {
                k = add_mounts();
                if (r >= 0)
                        r = k;
        }

        return r;
}

DEFINE_MAIN_GENERATOR_FUNCTION(run);
