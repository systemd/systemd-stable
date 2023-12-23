/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stdbool.h>

#include "sd-id128.h"

#include "architecture.h"
#include "list.h"
#include "loop-util.h"
#include "macro.h"

typedef struct DissectedImage DissectedImage;
typedef struct DissectedPartition DissectedPartition;
typedef struct DecryptedImage DecryptedImage;
typedef struct MountOptions MountOptions;
typedef struct VeritySettings VeritySettings;

struct DissectedPartition {
        bool found:1;
        bool rw:1;
        bool growfs:1;
        int partno;                 /* -1 if there was no partition and the images contains a file system directly */
        Architecture architecture;  /* Intended architecture: either native, secondary or unset ARCHITECTURE_INVALID. */
        sd_id128_t uuid;            /* Partition entry UUID as reported by the GPT */
        char *fstype;
        char *node;
        char *label;
        char *decrypted_node;
        char *decrypted_fstype;
        char *mount_options;
        int mount_node_fd;
        uint64_t size;
        uint64_t offset;
};

#define DISSECTED_PARTITION_NULL                                        \
        ((DissectedPartition) {                                         \
                .partno = -1,                                           \
                .architecture = _ARCHITECTURE_INVALID,                  \
                .mount_node_fd = -1,                                    \
        })
#define TAKE_PARTITION(p)                                       \
        ({                                                      \
                DissectedPartition *_pp = &(p), _p = *_pp;      \
                *_pp = DISSECTED_PARTITION_NULL;                \
                _p;                                             \
        })

typedef enum PartitionDesignator {
        PARTITION_ROOT,
        PARTITION_ROOT_SECONDARY,  /* Secondary architecture */
        PARTITION_ROOT_OTHER,
        PARTITION_USR,
        PARTITION_USR_SECONDARY,
        PARTITION_USR_OTHER,
        PARTITION_HOME,
        PARTITION_SRV,
        PARTITION_ESP,
        PARTITION_XBOOTLDR,
        PARTITION_SWAP,
        PARTITION_ROOT_VERITY, /* verity data for the PARTITION_ROOT partition */
        PARTITION_ROOT_SECONDARY_VERITY, /* verity data for the PARTITION_ROOT_SECONDARY partition */
        PARTITION_ROOT_OTHER_VERITY,
        PARTITION_USR_VERITY,
        PARTITION_USR_SECONDARY_VERITY,
        PARTITION_USR_OTHER_VERITY,
        PARTITION_ROOT_VERITY_SIG, /* PKCS#7 signature for root hash for the PARTITION_ROOT partition */
        PARTITION_ROOT_SECONDARY_VERITY_SIG, /* ditto for the PARTITION_ROOT_SECONDARY partition */
        PARTITION_ROOT_OTHER_VERITY_SIG,
        PARTITION_USR_VERITY_SIG,
        PARTITION_USR_SECONDARY_VERITY_SIG,
        PARTITION_USR_OTHER_VERITY_SIG,
        PARTITION_TMP,
        PARTITION_VAR,
        _PARTITION_DESIGNATOR_MAX,
        _PARTITION_DESIGNATOR_INVALID = -EINVAL,
} PartitionDesignator;

static inline bool PARTITION_DESIGNATOR_VERSIONED(PartitionDesignator d) {
        /* Returns true for all designators where we want to support a concept of "versioning", i.e. which
         * likely contain software binaries (or hashes thereof) that make sense to be versioned as a
         * whole. We use this check to automatically pick the newest version of these partitions, by version
         * comparing the partition labels. */

        return IN_SET(d,
                      PARTITION_ROOT,
                      PARTITION_ROOT_SECONDARY,
                      PARTITION_ROOT_OTHER,
                      PARTITION_USR,
                      PARTITION_USR_SECONDARY,
                      PARTITION_USR_OTHER,
                      PARTITION_ROOT_VERITY,
                      PARTITION_ROOT_SECONDARY_VERITY,
                      PARTITION_ROOT_OTHER_VERITY,
                      PARTITION_USR_VERITY,
                      PARTITION_USR_SECONDARY_VERITY,
                      PARTITION_USR_OTHER_VERITY,
                      PARTITION_ROOT_VERITY_SIG,
                      PARTITION_ROOT_SECONDARY_VERITY_SIG,
                      PARTITION_ROOT_OTHER_VERITY_SIG,
                      PARTITION_USR_VERITY_SIG,
                      PARTITION_USR_SECONDARY_VERITY_SIG,
                      PARTITION_USR_OTHER_VERITY_SIG);
}

static inline PartitionDesignator PARTITION_VERITY_OF(PartitionDesignator p) {
        switch (p) {

        case PARTITION_ROOT:
                return PARTITION_ROOT_VERITY;

        case PARTITION_ROOT_SECONDARY:
                return PARTITION_ROOT_SECONDARY_VERITY;

        case PARTITION_ROOT_OTHER:
                return PARTITION_ROOT_OTHER_VERITY;

        case PARTITION_USR:
                return PARTITION_USR_VERITY;

        case PARTITION_USR_SECONDARY:
                return PARTITION_USR_SECONDARY_VERITY;

        case PARTITION_USR_OTHER:
                return PARTITION_USR_OTHER_VERITY;

        default:
                return _PARTITION_DESIGNATOR_INVALID;
        }
}

static inline PartitionDesignator PARTITION_VERITY_SIG_OF(PartitionDesignator p) {
        switch (p) {

        case PARTITION_ROOT:
                return PARTITION_ROOT_VERITY_SIG;

        case PARTITION_ROOT_SECONDARY:
                return PARTITION_ROOT_SECONDARY_VERITY_SIG;

        case PARTITION_ROOT_OTHER:
                return PARTITION_ROOT_OTHER_VERITY_SIG;

        case PARTITION_USR:
                return PARTITION_USR_VERITY_SIG;

        case PARTITION_USR_SECONDARY:
                return PARTITION_USR_SECONDARY_VERITY_SIG;

        case PARTITION_USR_OTHER:
                return PARTITION_USR_OTHER_VERITY_SIG;

        default:
                return _PARTITION_DESIGNATOR_INVALID;
        }
}

static inline PartitionDesignator PARTITION_ROOT_OF_ARCH(Architecture arch) {
        switch (arch) {

        case native_architecture():
                return PARTITION_ROOT;

#ifdef ARCHITECTURE_SECONDARY
        case ARCHITECTURE_SECONDARY:
                return PARTITION_ROOT_SECONDARY;
#endif

        default:
                return PARTITION_ROOT_OTHER;
        }
}

static inline PartitionDesignator PARTITION_USR_OF_ARCH(Architecture arch) {
        switch (arch) {

        case native_architecture():
                return PARTITION_USR;

#ifdef ARCHITECTURE_SECONDARY
        case ARCHITECTURE_SECONDARY:
                return PARTITION_USR_SECONDARY;
#endif

        default:
                return PARTITION_USR_OTHER;
        }
}

typedef enum DissectImageFlags {
        DISSECT_IMAGE_DEVICE_READ_ONLY         = 1 << 0,  /* Make device read-only */
        DISSECT_IMAGE_DISCARD_ON_LOOP          = 1 << 1,  /* Turn on "discard" if on a loop device and file system supports it */
        DISSECT_IMAGE_DISCARD                  = 1 << 2,  /* Turn on "discard" if file system supports it, on all block devices */
        DISSECT_IMAGE_DISCARD_ON_CRYPTO        = 1 << 3,  /* Turn on "discard" also on crypto devices */
        DISSECT_IMAGE_DISCARD_ANY              = DISSECT_IMAGE_DISCARD_ON_LOOP |
                                                 DISSECT_IMAGE_DISCARD |
                                                 DISSECT_IMAGE_DISCARD_ON_CRYPTO,
        DISSECT_IMAGE_GPT_ONLY                 = 1 << 4,  /* Only recognize images with GPT partition tables */
        DISSECT_IMAGE_GENERIC_ROOT             = 1 << 5,  /* If no partition table or only single generic partition, assume it's the root fs */
        DISSECT_IMAGE_MOUNT_ROOT_ONLY          = 1 << 6,  /* Mount only the root and /usr partitions */
        DISSECT_IMAGE_MOUNT_NON_ROOT_ONLY      = 1 << 7,  /* Mount only the non-root and non-/usr partitions */
        DISSECT_IMAGE_VALIDATE_OS              = 1 << 8,  /* Refuse mounting images that aren't identifiable as OS images */
        DISSECT_IMAGE_VALIDATE_OS_EXT          = 1 << 9,  /* Refuse mounting images that aren't identifiable as OS extension images */
        DISSECT_IMAGE_RELAX_VAR_CHECK          = 1 << 10, /* Don't insist that the UUID of /var is hashed from /etc/machine-id */
        DISSECT_IMAGE_FSCK                     = 1 << 11, /* File system check the partition before mounting (no effect when combined with DISSECT_IMAGE_READ_ONLY) */
        DISSECT_IMAGE_NO_PARTITION_TABLE       = 1 << 12, /* Only recognize single file system images */
        DISSECT_IMAGE_VERITY_SHARE             = 1 << 13, /* When activating a verity device, reuse existing one if already open */
        DISSECT_IMAGE_MKDIR                    = 1 << 14, /* Make top-level directory to mount right before mounting, if missing */
        DISSECT_IMAGE_USR_NO_ROOT              = 1 << 15, /* If no root fs is in the image, but /usr is, then allow this (so that we can mount the rootfs as tmpfs or so */
        DISSECT_IMAGE_REQUIRE_ROOT             = 1 << 16, /* Don't accept disks without root partition (or at least /usr partition if DISSECT_IMAGE_USR_NO_ROOT is set) */
        DISSECT_IMAGE_MOUNT_READ_ONLY          = 1 << 17, /* Make mounts read-only */
        DISSECT_IMAGE_READ_ONLY                = DISSECT_IMAGE_DEVICE_READ_ONLY |
                                                 DISSECT_IMAGE_MOUNT_READ_ONLY,
        DISSECT_IMAGE_GROWFS                   = 1 << 18, /* Grow file systems in partitions marked for that to the size of the partitions after mount */
        DISSECT_IMAGE_MOUNT_IDMAPPED           = 1 << 19, /* Mount mounts with kernel 5.12-style userns ID mapping, if file system type doesn't support uid=/gid= */
        DISSECT_IMAGE_ADD_PARTITION_DEVICES    = 1 << 20, /* Create partition devices via BLKPG_ADD_PARTITION */
        DISSECT_IMAGE_PIN_PARTITION_DEVICES    = 1 << 21, /* Open dissected partitions and decrypted partitions and pin them by fd */
        DISSECT_IMAGE_RELAX_SYSEXT_CHECK       = 1 << 22, /* Don't insist that the extension-release file name matches the image name */
} DissectImageFlags;

struct DissectedImage {
        bool encrypted:1;
        bool has_verity:1;         /* verity available in image, but not necessarily used */
        bool has_verity_sig:1;     /* pkcs#7 signature embedded in image */
        bool verity_ready:1;       /* verity available, fully specified and usable */
        bool verity_sig_ready:1;   /* verity signature logic, fully specified and usable */
        bool single_file_system:1; /* MBR/GPT or single file system */

        LoopDevice *loop;
        DissectedPartition partitions[_PARTITION_DESIGNATOR_MAX];
        DecryptedImage *decrypted_image;
        char *image_name;

        /* Meta information extracted from /etc/os-release and similar */
        char *hostname;
        sd_id128_t machine_id;
        char **machine_info;
        char **os_release;
        char **extension_release;
        int has_init_system;
};

struct MountOptions {
        PartitionDesignator partition_designator;
        char *options;
        LIST_FIELDS(MountOptions, mount_options);
};

struct VeritySettings {
        /* Binary root hash for the Verity Merkle tree */
        void *root_hash;
        size_t root_hash_size;

        /* PKCS#7 signature of the above */
        void *root_hash_sig;
        size_t root_hash_sig_size;

        /* Path to the verity data file, if stored externally */
        char *data_path;

        /* PARTITION_ROOT or PARTITION_USR, depending on what these Verity settings are for */
        PartitionDesignator designator;
};

#define VERITY_SETTINGS_DEFAULT {                               \
                .designator = _PARTITION_DESIGNATOR_INVALID     \
        }

MountOptions* mount_options_free_all(MountOptions *options);
DEFINE_TRIVIAL_CLEANUP_FUNC(MountOptions*, mount_options_free_all);
const char* mount_options_from_designator(const MountOptions *options, PartitionDesignator designator);

int probe_filesystem_full(int fd, const char *path, uint64_t offset, uint64_t size, char **ret_fstype);
static inline int probe_filesystem(const char *path, char **ret_fstype) {
        return probe_filesystem_full(-1, path, 0, UINT64_MAX, ret_fstype);
}
int dissect_image_file(
                const char *path,
                const VeritySettings *verity,
                const MountOptions *mount_options,
                DissectImageFlags flags,
                DissectedImage **ret);
int dissect_loop_device(LoopDevice *loop, const VeritySettings *verity, const MountOptions *mount_options, DissectImageFlags flags, DissectedImage **ret);
int dissect_loop_device_and_warn(LoopDevice *loop, const VeritySettings *verity, const MountOptions *mount_options, DissectImageFlags flags, DissectedImage **ret);

DissectedImage* dissected_image_unref(DissectedImage *m);
DEFINE_TRIVIAL_CLEANUP_FUNC(DissectedImage*, dissected_image_unref);

int dissected_image_decrypt(DissectedImage *m, const char *passphrase, const VeritySettings *verity, DissectImageFlags flags);
int dissected_image_decrypt_interactively(DissectedImage *m, const char *passphrase, const VeritySettings *verity, DissectImageFlags flags);
int dissected_image_mount(DissectedImage *m, const char *dest, uid_t uid_shift, uid_t uid_range, DissectImageFlags flags);
int dissected_image_mount_and_warn(DissectedImage *m, const char *where, uid_t uid_shift, uid_t uid_range, DissectImageFlags flags);

int dissected_image_acquire_metadata(DissectedImage *m, DissectImageFlags extra_flags);

DecryptedImage* decrypted_image_ref(DecryptedImage *p);
DecryptedImage* decrypted_image_unref(DecryptedImage *p);
DEFINE_TRIVIAL_CLEANUP_FUNC(DecryptedImage*, decrypted_image_unref);

int dissected_image_relinquish(DissectedImage *m);

const char* partition_designator_to_string(PartitionDesignator d) _const_;
PartitionDesignator partition_designator_from_string(const char *name) _pure_;

int verity_settings_load(VeritySettings *verity, const char *image, const char *root_hash_path, const char *root_hash_sig_path);
void verity_settings_done(VeritySettings *verity);

int dissected_image_load_verity_sig_partition(DissectedImage *m, int fd, VeritySettings *verity);

bool dissected_image_verity_candidate(const DissectedImage *image, PartitionDesignator d);
bool dissected_image_verity_ready(const DissectedImage *image, PartitionDesignator d);
bool dissected_image_verity_sig_ready(const DissectedImage *image, PartitionDesignator d);

int mount_image_privately_interactively(const char *path, DissectImageFlags flags, char **ret_directory, LoopDevice **ret_loop_device);

int verity_dissect_and_mount(int src_fd, const char *src, const char *dest, const MountOptions *options, const char *required_host_os_release_id, const char *required_host_os_release_version_id, const char *required_host_os_release_sysext_level, const char *required_sysext_scope);
