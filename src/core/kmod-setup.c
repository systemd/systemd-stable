/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <unistd.h>

#include "alloc-util.h"
#include "bus-util.h"
#include "capability-util.h"
#include "efi-api.h"
#include "fileio.h"
#include "kmod-setup.h"
#include "macro.h"
#include "recurse-dir.h"
#include "string-util.h"
#include "strv.h"
#include "virt.h"

#if HAVE_KMOD
#include "module-util.h"

static void systemd_kmod_log(
                void *data,
                int priority,
                const char *file, int line,
                const char *fn,
                const char *format,
                va_list args) {

        /* library logging is enabled at debug only */
        DISABLE_WARNING_FORMAT_NONLITERAL;
        log_internalv(LOG_DEBUG, 0, file, line, fn, format, args);
        REENABLE_WARNING;
}

static int match_modalias_recurse_dir_cb(
                RecurseDirEvent event,
                const char *path,
                int dir_fd,
                int inode_fd,
                const struct dirent *de,
                const struct statx *sx,
                void *userdata) {

        _cleanup_free_ char *alias = NULL;
        char **modaliases = ASSERT_PTR(userdata);
        int r;

        if (event != RECURSE_DIR_ENTRY)
                return RECURSE_DIR_CONTINUE;

        if (de->d_type != DT_REG)
                return RECURSE_DIR_CONTINUE;

        if (!streq(de->d_name, "modalias"))
                return RECURSE_DIR_CONTINUE;

        r = read_one_line_file(path, &alias);
        if (r < 0) {
                log_debug_errno(r, "Failed to read %s, ignoring: %m", path);
                return RECURSE_DIR_LEAVE_DIRECTORY;
        }

        if (startswith_strv(alias, modaliases))
                return 1;

        return RECURSE_DIR_LEAVE_DIRECTORY;
}

static bool has_virtio_feature(const char *name, char **modaliases) {
        int r;

        /* Directory traversal might be slow, hence let's do a cheap check first if it's even worth it */
        if (detect_vm() == VIRTUALIZATION_NONE)
                return false;

        r = recurse_dir_at(
                        AT_FDCWD,
                        "/sys/devices/pci0000:00",
                        /* statx_mask= */ 0,
                        /* n_depth_max= */ 3,
                        RECURSE_DIR_ENSURE_TYPE,
                        match_modalias_recurse_dir_cb,
                        modaliases);
        if (r < 0)
                log_debug_errno(r, "Failed to determine whether host has %s device, ignoring: %m", name);

        return r > 0;
}

static bool has_virtio_rng(void) {
        return has_virtio_feature("virtio-rng", STRV_MAKE("pci:v00001AF4d00001005", "pci:v00001AF4d00001044"));
}

static bool has_virtio_console(void) {
        return has_virtio_feature("virtio-console", STRV_MAKE("virtio:d00000003v", "virtio:d0000000Bv"));
}

static bool has_virtio_vsock(void) {
        return has_virtio_feature("virtio-vsock", STRV_MAKE("virtio:d00000013v"));
}

static bool has_virtiofs(void) {
        return has_virtio_feature("virtiofs", STRV_MAKE("virtio:d0000001Av"));
}

static bool has_virtio_pci(void) {
        return has_virtio_feature("virtio-pci", STRV_MAKE("pci:v00001AF4d"));
}

static bool in_qemu(void) {
        return IN_SET(detect_vm(), VIRTUALIZATION_KVM, VIRTUALIZATION_QEMU);
}
#endif

int kmod_setup(void) {
#if HAVE_KMOD

        static const struct {
                const char *module;
                const char *path;
                bool warn_if_unavailable:1;
                bool warn_if_module:1;
                bool (*condition_fn)(void);
        } kmod_table[] = {
                /* This one we need to load explicitly, since auto-loading on use doesn't work
                 * before udev created the ghost device nodes, and we need it earlier than that. */
                { "autofs4",                    "/sys/class/misc/autofs",    true,  false, NULL               },

                /* This one we need to load explicitly, since auto-loading of IPv6 is not done when
                 * we try to configure ::1 on the loopback device. */
                { "ipv6",                       "/sys/module/ipv6",          false, true,  NULL               },

                /* This should never be a module */
                { "unix",                       "/proc/net/unix",            true,  true,  NULL               },

#if HAVE_LIBIPTC
                /* netfilter is needed by networkd, nspawn among others, and cannot be autoloaded */
                { "ip_tables",                  "/proc/net/ip_tables_names", false, false, NULL               },
#endif
                /* virtio_rng would be loaded by udev later, but real entropy might be needed very early */
                { "virtio_rng",                 NULL,                        false, false, has_virtio_rng     },

                /* we want early logging to hvc consoles if possible, and make sure systemd-getty-generator
                 * can rely on all consoles being probed already.*/
                { "virtio_console",             NULL,                        false, false, has_virtio_console },

                /* Make sure we can send sd-notify messages over vsock as early as possible. */
                { "vmw_vsock_virtio_transport", NULL,                        false, false, has_virtio_vsock   },

                /* We can't wait for specific virtiofs tags to show up as device nodes so we have to load the
                 * virtiofs and virtio_pci modules early to make sure the virtiofs tags are found when
                 * sysroot.mount is started.
                 *
                 * TODO: Remove these again once https://gitlab.com/virtio-fs/virtiofsd/-/issues/128 is
                 * resolved and the kernel fix is widely available. */
                { "virtiofs",                   "/sys/module/virtiofs",      false, false, has_virtiofs       },
                { "virtio_pci",                 "/sys/module/virtio_pci",    false, false, has_virtio_pci     },

                /* qemu_fw_cfg would be loaded by udev later, but we want to import credentials from it super early */
                { "qemu_fw_cfg",                "/sys/firmware/qemu_fw_cfg", false, false, in_qemu            },

                /* dmi-sysfs is needed to import credentials from it super early */
                { "dmi-sysfs",                  "/sys/firmware/dmi/entries", false, false, NULL               },

#if HAVE_TPM2
                /* Make sure the tpm subsystem is available which ConditionSecurity=tpm2 depends on. */
                { "tpm",                        "/sys/class/tpmrm",          false, false, efi_has_tpm2       },
#endif
        };
        _cleanup_(kmod_unrefp) struct kmod_ctx *ctx = NULL;
        unsigned i;

        if (have_effective_cap(CAP_SYS_MODULE) <= 0)
                return 0;

        for (i = 0; i < ELEMENTSOF(kmod_table); i++) {
                if (kmod_table[i].path && access(kmod_table[i].path, F_OK) >= 0)
                        continue;

                if (kmod_table[i].condition_fn && !kmod_table[i].condition_fn())
                        continue;

                if (kmod_table[i].warn_if_module)
                        log_debug("Your kernel apparently lacks built-in %s support. Might be "
                                  "a good idea to compile it in. We'll now try to work around "
                                  "this by loading the module...", kmod_table[i].module);

                if (!ctx) {
                        ctx = kmod_new(NULL, NULL);
                        if (!ctx)
                                return log_oom();

                        kmod_set_log_fn(ctx, systemd_kmod_log, NULL);
                        kmod_load_resources(ctx);
                }

                (void) module_load_and_warn(ctx, kmod_table[i].module, kmod_table[i].warn_if_unavailable);
        }

#endif
        return 0;
}
