/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "libfido2-util.h"

#if HAVE_LIBFIDO2
#include "alloc-util.h"
#include "ask-password-api.h"
#include "dlfcn-util.h"
#include "format-table.h"
#include "locale-util.h"
#include "log.h"
#include "memory-util.h"
#include "random-util.h"
#include "strv.h"

static void *libfido2_dl = NULL;

int (*sym_fido_assert_allow_cred)(fido_assert_t *, const unsigned char *, size_t) = NULL;
void (*sym_fido_assert_free)(fido_assert_t **) = NULL;
size_t (*sym_fido_assert_hmac_secret_len)(const fido_assert_t *, size_t) = NULL;
const unsigned char* (*sym_fido_assert_hmac_secret_ptr)(const fido_assert_t *, size_t) = NULL;
fido_assert_t* (*sym_fido_assert_new)(void) = NULL;
int (*sym_fido_assert_set_clientdata_hash)(fido_assert_t *, const unsigned char *, size_t) = NULL;
int (*sym_fido_assert_set_extensions)(fido_assert_t *, int) = NULL;
int (*sym_fido_assert_set_hmac_salt)(fido_assert_t *, const unsigned char *, size_t) = NULL;
int (*sym_fido_assert_set_rp)(fido_assert_t *, const char *) = NULL;
int (*sym_fido_assert_set_up)(fido_assert_t *, fido_opt_t) = NULL;
int (*sym_fido_assert_set_uv)(fido_assert_t *, fido_opt_t) = NULL;
size_t (*sym_fido_cbor_info_extensions_len)(const fido_cbor_info_t *) = NULL;
char **(*sym_fido_cbor_info_extensions_ptr)(const fido_cbor_info_t *) = NULL;
void (*sym_fido_cbor_info_free)(fido_cbor_info_t **) = NULL;
fido_cbor_info_t* (*sym_fido_cbor_info_new)(void) = NULL;
size_t (*sym_fido_cbor_info_options_len)(const fido_cbor_info_t *) = NULL;
char** (*sym_fido_cbor_info_options_name_ptr)(const fido_cbor_info_t *) = NULL;
const bool* (*sym_fido_cbor_info_options_value_ptr)(const fido_cbor_info_t *) = NULL;
void (*sym_fido_cred_free)(fido_cred_t **) = NULL;
size_t (*sym_fido_cred_id_len)(const fido_cred_t *) = NULL;
const unsigned char* (*sym_fido_cred_id_ptr)(const fido_cred_t *) = NULL;
fido_cred_t* (*sym_fido_cred_new)(void) = NULL;
int (*sym_fido_cred_set_clientdata_hash)(fido_cred_t *, const unsigned char *, size_t) = NULL;
int (*sym_fido_cred_set_extensions)(fido_cred_t *, int) = NULL;
int (*sym_fido_cred_set_rk)(fido_cred_t *, fido_opt_t) = NULL;
int (*sym_fido_cred_set_rp)(fido_cred_t *, const char *, const char *) = NULL;
int (*sym_fido_cred_set_type)(fido_cred_t *, int) = NULL;
int (*sym_fido_cred_set_user)(fido_cred_t *, const unsigned char *, size_t, const char *, const char *, const char *) = NULL;
int (*sym_fido_cred_set_uv)(fido_cred_t *, fido_opt_t) = NULL;
void (*sym_fido_dev_free)(fido_dev_t **) = NULL;
int (*sym_fido_dev_get_assert)(fido_dev_t *, fido_assert_t *, const char *) = NULL;
int (*sym_fido_dev_get_cbor_info)(fido_dev_t *, fido_cbor_info_t *) = NULL;
void (*sym_fido_dev_info_free)(fido_dev_info_t **, size_t) = NULL;
int (*sym_fido_dev_info_manifest)(fido_dev_info_t *, size_t, size_t *) = NULL;
const char* (*sym_fido_dev_info_manufacturer_string)(const fido_dev_info_t *) = NULL;
const char* (*sym_fido_dev_info_product_string)(const fido_dev_info_t *) = NULL;
fido_dev_info_t* (*sym_fido_dev_info_new)(size_t) = NULL;
const char* (*sym_fido_dev_info_path)(const fido_dev_info_t *) = NULL;
const fido_dev_info_t* (*sym_fido_dev_info_ptr)(const fido_dev_info_t *, size_t) = NULL;
bool (*sym_fido_dev_is_fido2)(const fido_dev_t *) = NULL;
int (*sym_fido_dev_make_cred)(fido_dev_t *, fido_cred_t *, const char *) = NULL;
fido_dev_t* (*sym_fido_dev_new)(void) = NULL;
int (*sym_fido_dev_open)(fido_dev_t *, const char *) = NULL;
int (*sym_fido_dev_close)(fido_dev_t *) = NULL;
const char* (*sym_fido_strerr)(int) = NULL;

int dlopen_libfido2(void) {
        return dlopen_many_sym_or_warn(
                        &libfido2_dl, "libfido2.so.1", LOG_DEBUG,
                        DLSYM_ARG(fido_assert_allow_cred),
                        DLSYM_ARG(fido_assert_free),
                        DLSYM_ARG(fido_assert_hmac_secret_len),
                        DLSYM_ARG(fido_assert_hmac_secret_ptr),
                        DLSYM_ARG(fido_assert_new),
                        DLSYM_ARG(fido_assert_set_clientdata_hash),
                        DLSYM_ARG(fido_assert_set_extensions),
                        DLSYM_ARG(fido_assert_set_hmac_salt),
                        DLSYM_ARG(fido_assert_set_rp),
                        DLSYM_ARG(fido_assert_set_up),
                        DLSYM_ARG(fido_assert_set_uv),
                        DLSYM_ARG(fido_cbor_info_extensions_len),
                        DLSYM_ARG(fido_cbor_info_extensions_ptr),
                        DLSYM_ARG(fido_cbor_info_free),
                        DLSYM_ARG(fido_cbor_info_new),
                        DLSYM_ARG(fido_cbor_info_options_len),
                        DLSYM_ARG(fido_cbor_info_options_name_ptr),
                        DLSYM_ARG(fido_cbor_info_options_value_ptr),
                        DLSYM_ARG(fido_cred_free),
                        DLSYM_ARG(fido_cred_id_len),
                        DLSYM_ARG(fido_cred_id_ptr),
                        DLSYM_ARG(fido_cred_new),
                        DLSYM_ARG(fido_cred_set_clientdata_hash),
                        DLSYM_ARG(fido_cred_set_extensions),
                        DLSYM_ARG(fido_cred_set_rk),
                        DLSYM_ARG(fido_cred_set_rp),
                        DLSYM_ARG(fido_cred_set_type),
                        DLSYM_ARG(fido_cred_set_user),
                        DLSYM_ARG(fido_cred_set_uv),
                        DLSYM_ARG(fido_dev_free),
                        DLSYM_ARG(fido_dev_get_assert),
                        DLSYM_ARG(fido_dev_get_cbor_info),
                        DLSYM_ARG(fido_dev_info_free),
                        DLSYM_ARG(fido_dev_info_manifest),
                        DLSYM_ARG(fido_dev_info_manufacturer_string),
                        DLSYM_ARG(fido_dev_info_new),
                        DLSYM_ARG(fido_dev_info_path),
                        DLSYM_ARG(fido_dev_info_product_string),
                        DLSYM_ARG(fido_dev_info_ptr),
                        DLSYM_ARG(fido_dev_is_fido2),
                        DLSYM_ARG(fido_dev_make_cred),
                        DLSYM_ARG(fido_dev_new),
                        DLSYM_ARG(fido_dev_open),
                        DLSYM_ARG(fido_dev_close),
                        DLSYM_ARG(fido_strerr));
}

static int verify_features(
                fido_dev_t *d,
                const char *path,
                int log_level, /* the log level to use when device is not FIDO2 with hmac-secret */
                bool *ret_has_rk,
                bool *ret_has_client_pin,
                bool *ret_has_up,
                bool *ret_has_uv) {

        _cleanup_(fido_cbor_info_free_wrapper) fido_cbor_info_t *di = NULL;
        bool found_extension = false;
        char **e, **o;
        const bool *b;
        bool has_rk = false, has_client_pin = false, has_up = true, has_uv = false; /* Defaults are per table in 5.4 in FIDO2 spec */
        size_t n;
        int r;

        assert(d);
        assert(path);

        if (!sym_fido_dev_is_fido2(d))
                return log_full_errno(log_level,
                                      SYNTHETIC_ERRNO(ENODEV),
                                       "Specified device %s is not a FIDO2 device.", path);

        di = sym_fido_cbor_info_new();
        if (!di)
                return log_oom();

        r = sym_fido_dev_get_cbor_info(d, di);
        if (r != FIDO_OK)
                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                       "Failed to get CBOR device info for %s: %s", path, sym_fido_strerr(r));

        e = sym_fido_cbor_info_extensions_ptr(di);
        n = sym_fido_cbor_info_extensions_len(di);
        for (size_t i = 0; i < n; i++) {
                log_debug("FIDO2 device implements extension: %s", e[i]);
                if (streq(e[i], "hmac-secret"))
                        found_extension = true;
        }

        o = sym_fido_cbor_info_options_name_ptr(di);
        b = sym_fido_cbor_info_options_value_ptr(di);
        n = sym_fido_cbor_info_options_len(di);
        for (size_t i = 0; i < n; i++) {
                log_debug("FIDO2 device implements option %s: %s", o[i], yes_no(b[i]));
                if (streq(o[i], "rk"))
                        has_rk = b[i];
                if (streq(o[i], "clientPin"))
                        has_client_pin = b[i];
                if (streq(o[i], "up"))
                        has_up = b[i];
                if (streq(o[i], "uv"))
                        has_uv = b[i];
        }

        if (!found_extension)
                return log_full_errno(log_level,
                                      SYNTHETIC_ERRNO(ENODEV),
                                       "Specified device %s is a FIDO2 device, but does not support the required HMAC-SECRET extension.", path);

        log_debug("Has rk ('Resident Key') support: %s\n"
                  "Has clientPin support: %s\n"
                  "Has up ('User Presence') support: %s\n"
                  "Has uv ('User Verification') support: %s\n",
                  yes_no(has_rk),
                  yes_no(has_client_pin),
                  yes_no(has_up),
                  yes_no(has_uv));

        if (ret_has_rk)
                *ret_has_rk = has_rk;
        if (ret_has_client_pin)
                *ret_has_client_pin = has_client_pin;
        if (ret_has_up)
                *ret_has_up = has_up;
        if (ret_has_uv)
                *ret_has_uv = has_uv;

        return 0;
}

static int fido2_use_hmac_hash_specific_token(
                const char *path,
                const char *rp_id,
                const void *salt,
                size_t salt_size,
                const void *cid,
                size_t cid_size,
                char **pins,
                Fido2EnrollFlags required, /* client pin/user presence required */
                void **ret_hmac,
                size_t *ret_hmac_size) {

        _cleanup_(fido_assert_free_wrapper) fido_assert_t *a = NULL;
        _cleanup_(fido_dev_free_wrapper) fido_dev_t *d = NULL;
        _cleanup_(erase_and_freep) void *hmac_copy = NULL;
        bool has_up, has_client_pin, has_uv;
        size_t hmac_size;
        const void *hmac;
        int r;

        assert(path);
        assert(rp_id);
        assert(salt);
        assert(cid);
        assert(ret_hmac);
        assert(ret_hmac_size);

        d = sym_fido_dev_new();
        if (!d)
                return log_oom();

        r = sym_fido_dev_open(d, path);
        if (r != FIDO_OK)
                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                       "Failed to open FIDO2 device %s: %s", path, sym_fido_strerr(r));

        r = verify_features(d, path, LOG_ERR, NULL, &has_client_pin, &has_up, &has_uv);
        if (r < 0)
                return r;

        if (!has_client_pin && FLAGS_SET(required, FIDO2ENROLL_PIN))
                return log_error_errno(SYNTHETIC_ERRNO(EHWPOISON),
                                       "PIN required to unlock, but FIDO2 device %s does not support it.",
                                       path);

        if (!has_up && FLAGS_SET(required, FIDO2ENROLL_UP))
                return log_error_errno(SYNTHETIC_ERRNO(EHWPOISON),
                                       "User presence test required to unlock, but FIDO2 device %s does not support it.",
                                       path);

        if (!has_uv && FLAGS_SET(required, FIDO2ENROLL_UV))
                return log_error_errno(SYNTHETIC_ERRNO(EHWPOISON),
                                       "User verification required to unlock, but FIDO2 device %s does not support it.",
                                       path);

        a = sym_fido_assert_new();
        if (!a)
                return log_oom();

        r = sym_fido_assert_set_extensions(a, FIDO_EXT_HMAC_SECRET);
        if (r != FIDO_OK)
                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                       "Failed to enable HMAC-SECRET extension on FIDO2 assertion: %s", sym_fido_strerr(r));

        r = sym_fido_assert_set_hmac_salt(a, salt, salt_size);
        if (r != FIDO_OK)
                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                       "Failed to set salt on FIDO2 assertion: %s", sym_fido_strerr(r));

        r = sym_fido_assert_set_rp(a, rp_id);
        if (r != FIDO_OK)
                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                       "Failed to set FIDO2 assertion ID: %s", sym_fido_strerr(r));

        r = sym_fido_assert_set_clientdata_hash(a, (const unsigned char[32]) {}, 32);
        if (r != FIDO_OK)
                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                       "Failed to set FIDO2 assertion client data hash: %s", sym_fido_strerr(r));

        r = sym_fido_assert_allow_cred(a, cid, cid_size);
        if (r != FIDO_OK)
                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                       "Failed to add FIDO2 assertion credential ID: %s", sym_fido_strerr(r));

        log_info("Asking FIDO2 token for authentication.");

        if (has_up) {
                r = sym_fido_assert_set_up(a, FLAGS_SET(required, FIDO2ENROLL_UP) ? FIDO_OPT_TRUE : FIDO_OPT_FALSE);
                if (r != FIDO_OK)
                        return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                               "Failed to %s FIDO2 user presence test: %s",
                                               enable_disable(FLAGS_SET(required, FIDO2ENROLL_UP)),
                                               sym_fido_strerr(r));

                if (FLAGS_SET(required, FIDO2ENROLL_UP))
                        log_notice("%s%sPlease confirm presence on security token to unlock.",
                                   emoji_enabled() ? special_glyph(SPECIAL_GLYPH_TOUCH) : "",
                                   emoji_enabled() ? " " : "");
        }

        if (has_uv && !FLAGS_SET(required, FIDO2ENROLL_UV_OMIT)) {
                r = sym_fido_assert_set_uv(a, FLAGS_SET(required, FIDO2ENROLL_UV) ? FIDO_OPT_TRUE : FIDO_OPT_FALSE);
                if (r != FIDO_OK)
                        return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                               "Failed to %s FIDO2 user verification: %s",
                                               enable_disable(FLAGS_SET(required, FIDO2ENROLL_UV)),
                                               sym_fido_strerr(r));

                if (FLAGS_SET(required, FIDO2ENROLL_UV))
                        log_notice("%s%sPlease verify user on security token to unlock.",
                                   emoji_enabled() ? special_glyph(SPECIAL_GLYPH_TOUCH) : "",
                                   emoji_enabled() ? " " : "");
        }

        for (;;) {
                bool retry_with_up = false, retry_with_pin = false;

                if (FLAGS_SET(required, FIDO2ENROLL_PIN)) {
                        char **i;

                        /* OK, we need a pin, try with all pins in turn */
                        if (strv_isempty(pins))
                                r = FIDO_ERR_PIN_REQUIRED;
                        else
                                STRV_FOREACH(i, pins) {
                                        r = sym_fido_dev_get_assert(d, a, *i);
                                        if (r != FIDO_ERR_PIN_INVALID)
                                                break;
                                }

                } else
                        r = sym_fido_dev_get_assert(d, a, NULL);

                /* In some conditions, where a PIN or UP is required we might accept that. Let's check the
                 * conditions and if so try immediately again. */

                switch (r) {

                case FIDO_ERR_UP_REQUIRED:
                        /* So the token asked for "up". Try to turn it on, for compat with systemd 248 and try again. */

                        if (!has_up)
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                                       "Token asks for user presence test but doesn't advertise 'up' feature.");

                        if (FLAGS_SET(required, FIDO2ENROLL_UP))
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                                       "Token asks for user presence test but was already enabled.");

                        if (FLAGS_SET(required, FIDO2ENROLL_UP_IF_NEEDED)) {
                                log_notice("%s%sPlease confirm presence on security to unlock.",
                                           emoji_enabled() ? special_glyph(SPECIAL_GLYPH_TOUCH) : "",
                                           emoji_enabled() ? " " : "");
                                retry_with_up = true;
                        }

                        break;

                case FIDO_ERR_UNSUPPORTED_OPTION:
                        /* AuthenTrend ATKey.Pro returns this instead of FIDO_ERR_UP_REQUIRED, let's handle
                         * it gracefully (also see below.) */

                        if (has_up && (required & (FIDO2ENROLL_UP|FIDO2ENROLL_UP_IF_NEEDED)) == FIDO2ENROLL_UP_IF_NEEDED) {
                                log_notice("%s%sGot unsupported option error when when user presence test is turned off. Trying with user presence test turned on.",
                                           emoji_enabled() ? special_glyph(SPECIAL_GLYPH_TOUCH) : "",
                                           emoji_enabled() ? " " : "");
                                retry_with_up = true;
                        }

                        break;

                case FIDO_ERR_PIN_REQUIRED:
                        /* A pin was requested. Maybe supply one, if we are configured to do so on request */

                        if (!has_client_pin)
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                                       "Token asks for PIN but doesn't advertise 'clientPin' feature.");

                        if (FLAGS_SET(required, FIDO2ENROLL_PIN) && !strv_isempty(pins))
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                                       "Token asks for PIN but one was already supplied.");

                        if ((required & (FIDO2ENROLL_PIN|FIDO2ENROLL_PIN_IF_NEEDED)) == FIDO2ENROLL_PIN_IF_NEEDED) {
                                /* If a PIN so far wasn't specified but is requested by the device, and
                                 * FIDO2ENROLL_PIN_IF_NEEDED is set, then provide it */
                                log_debug("Retrying to create credential with PIN.");
                                retry_with_pin = true;
                        }

                        break;

                default:
                        break;
                }

                if (!retry_with_up && !retry_with_pin)
                        break;

                if (retry_with_up) {
                        r = sym_fido_assert_set_up(a, FIDO_OPT_TRUE);
                        if (r != FIDO_OK)
                                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                                       "Failed to enable FIDO2 user presence test: %s", sym_fido_strerr(r));

                        required |= FIDO2ENROLL_UP;
                }

                if (retry_with_pin)
                        required |= FIDO2ENROLL_PIN;
        }

        switch (r) {
        case FIDO_OK:
                break;
        case FIDO_ERR_NO_CREDENTIALS:
                return log_error_errno(SYNTHETIC_ERRNO(EBADSLT),
                                       "Wrong security token; needed credentials not present on token.");
        case FIDO_ERR_PIN_REQUIRED:
                return log_error_errno(SYNTHETIC_ERRNO(ENOANO),
                                       "Security token requires PIN.");
        case FIDO_ERR_PIN_AUTH_BLOCKED:
                return log_error_errno(SYNTHETIC_ERRNO(EOWNERDEAD),
                                       "PIN of security token is blocked, please remove/reinsert token.");
#ifdef FIDO_ERR_UV_BLOCKED
        case FIDO_ERR_UV_BLOCKED:
                return log_error_errno(SYNTHETIC_ERRNO(EOWNERDEAD),
                                       "Verification of security token is blocked, please remove/reinsert token.");
#endif
        case FIDO_ERR_PIN_INVALID:
                return log_error_errno(SYNTHETIC_ERRNO(ENOLCK),
                                       "PIN of security token incorrect.");
        case FIDO_ERR_UP_REQUIRED:
                return log_error_errno(SYNTHETIC_ERRNO(EMEDIUMTYPE),
                                       "User presence required.");
        case FIDO_ERR_ACTION_TIMEOUT:
                return log_error_errno(SYNTHETIC_ERRNO(ENOSTR),
                                       "Token action timeout. (User didn't interact with token quickly enough.)");
        default:
                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                       "Failed to ask token for assertion: %s", sym_fido_strerr(r));
        }

        hmac = sym_fido_assert_hmac_secret_ptr(a, 0);
        if (!hmac)
                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Failed to retrieve HMAC secret.");

        hmac_size = sym_fido_assert_hmac_secret_len(a, 0);

        hmac_copy = memdup(hmac, hmac_size);
        if (!hmac_copy)
                return log_oom();

        *ret_hmac = TAKE_PTR(hmac_copy);
        *ret_hmac_size = hmac_size;
        return 0;
}

int fido2_use_hmac_hash(
                const char *device,
                const char *rp_id,
                const void *salt,
                size_t salt_size,
                const void *cid,
                size_t cid_size,
                char **pins,
                Fido2EnrollFlags required, /* client pin/user presence required */
                void **ret_hmac,
                size_t *ret_hmac_size) {

        size_t allocated = 64, found = 0;
        fido_dev_info_t *di = NULL;
        int r;

        r = dlopen_libfido2();
        if (r < 0)
                return log_error_errno(r, "FIDO2 support is not installed.");

        if (device)
                return fido2_use_hmac_hash_specific_token(device, rp_id, salt, salt_size, cid, cid_size, pins, required, ret_hmac, ret_hmac_size);

        di = sym_fido_dev_info_new(allocated);
        if (!di)
                return log_oom();

        r = sym_fido_dev_info_manifest(di, allocated, &found);
        if (r == FIDO_ERR_INTERNAL) {
                /* The library returns FIDO_ERR_INTERNAL when no devices are found. I wish it wouldn't. */
                r = log_debug_errno(SYNTHETIC_ERRNO(EAGAIN), "Got FIDO_ERR_INTERNAL, assuming no devices.");
                goto finish;
        }
        if (r != FIDO_OK) {
                r = log_error_errno(SYNTHETIC_ERRNO(EIO), "Failed to enumerate FIDO2 devices: %s", sym_fido_strerr(r));
                goto finish;
        }

        for (size_t i = 0; i < found; i++) {
                const fido_dev_info_t *entry;
                const char *path;

                entry = sym_fido_dev_info_ptr(di, i);
                if (!entry) {
                        r = log_error_errno(SYNTHETIC_ERRNO(EIO),
                                            "Failed to get device information for FIDO device %zu.", i);
                        goto finish;
                }

                path = sym_fido_dev_info_path(entry);
                if (!path) {
                        r = log_error_errno(SYNTHETIC_ERRNO(EIO),
                                            "Failed to query FIDO device path.");
                        goto finish;
                }

                r = fido2_use_hmac_hash_specific_token(path, rp_id, salt, salt_size, cid, cid_size, pins, required, ret_hmac, ret_hmac_size);
                if (!IN_SET(r,
                            -EBADSLT, /* device doesn't understand our credential hash */
                            -ENODEV   /* device is not a FIDO2 device with HMAC-SECRET */))
                        goto finish;
        }

        r = -EAGAIN;

finish:
        sym_fido_dev_info_free(&di, allocated);
        return r;
}

#define FIDO2_SALT_SIZE 32

int fido2_generate_hmac_hash(
                const char *device,
                const char *rp_id,
                const char *rp_name,
                const void *user_id, size_t user_id_len,
                const char *user_name,
                const char *user_display_name,
                const char *user_icon,
                const char *askpw_icon_name,
                Fido2EnrollFlags lock_with,
                void **ret_cid, size_t *ret_cid_size,
                void **ret_salt, size_t *ret_salt_size,
                void **ret_secret, size_t *ret_secret_size,
                char **ret_usedpin,
                Fido2EnrollFlags *ret_locked_with) {

        _cleanup_(erase_and_freep) void *salt = NULL, *secret_copy = NULL;
        _cleanup_(fido_assert_free_wrapper) fido_assert_t *a = NULL;
        _cleanup_(fido_cred_free_wrapper) fido_cred_t *c = NULL;
        _cleanup_(fido_dev_free_wrapper) fido_dev_t *d = NULL;
        _cleanup_(erase_and_freep) char *used_pin = NULL;
        bool has_rk, has_client_pin, has_up, has_uv;
        _cleanup_free_ char *cid_copy = NULL;
        size_t cid_size, secret_size;
        const void *cid, *secret;
        int r;

        assert(device);
        assert(ret_cid);
        assert(ret_cid_size);
        assert(ret_salt);
        assert(ret_salt_size);
        assert(ret_secret);
        assert(ret_secret_size);

        /* Construction is like this: we generate a salt of 32 bytes. We then ask the FIDO2 device to
         * HMAC-SHA256 it for us with its internal key. The result is the key used by LUKS and account
         * authentication. LUKS and UNIX password auth all do their own salting before hashing, so that FIDO2
         * device never sees the volume key.
         *
         * S = HMAC-SHA256(I, D)
         *
         * with: S → LUKS/account authentication key                                         (never stored)
         *       I → internal key on FIDO2 device                              (stored in the FIDO2 device)
         *       D → salt we generate here               (stored in the privileged part of the JSON record)
         *
         */

        assert(device);
        assert((lock_with & ~(FIDO2ENROLL_PIN|FIDO2ENROLL_UP|FIDO2ENROLL_UV)) == 0);

        r = dlopen_libfido2();
        if (r < 0)
                return log_error_errno(r, "FIDO2 token support is not installed.");

        salt = malloc(FIDO2_SALT_SIZE);
        if (!salt)
                return log_oom();

        r = genuine_random_bytes(salt, FIDO2_SALT_SIZE, RANDOM_BLOCK);
        if (r < 0)
                return log_error_errno(r, "Failed to generate salt: %m");

        d = sym_fido_dev_new();
        if (!d)
                return log_oom();

        r = sym_fido_dev_open(d, device);
        if (r != FIDO_OK)
                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                       "Failed to open FIDO2 device %s: %s", device, sym_fido_strerr(r));

        r = verify_features(d, device, LOG_ERR, &has_rk, &has_client_pin, &has_up, &has_uv);
        if (r < 0)
                return r;

        /* While enrolling degrade gracefully if the requested feature set isn't available, but let the user know */
        if (!has_client_pin && FLAGS_SET(lock_with, FIDO2ENROLL_PIN)) {
                log_notice("Requested to lock with PIN, but FIDO2 device %s does not support it, disabling.", device);
                lock_with &= ~FIDO2ENROLL_PIN;
        }

        if (!has_up && FLAGS_SET(lock_with, FIDO2ENROLL_UP)) {
                log_notice("Locking with user presence test requested, but FIDO2 device %s does not support it, disabling.", device);
                lock_with &= ~FIDO2ENROLL_UP;
        }

        if (!has_uv && FLAGS_SET(lock_with, FIDO2ENROLL_UV)) {
                log_notice("Locking with user verification test requested, but FIDO2 device %s does not support it, disabling.", device);
                lock_with &= ~FIDO2ENROLL_UV;
        }

        c = sym_fido_cred_new();
        if (!c)
                return log_oom();

        r = sym_fido_cred_set_extensions(c, FIDO_EXT_HMAC_SECRET);
        if (r != FIDO_OK)
                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                       "Failed to enable HMAC-SECRET extension on FIDO2 credential: %s", sym_fido_strerr(r));

        r = sym_fido_cred_set_rp(c, rp_id, rp_name);
        if (r != FIDO_OK)
                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                       "Failed to set FIDO2 credential relying party ID/name: %s", sym_fido_strerr(r));

        r = sym_fido_cred_set_type(c, COSE_ES256);
        if (r != FIDO_OK)
                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                       "Failed to set FIDO2 credential type to ES256: %s", sym_fido_strerr(r));

        r = sym_fido_cred_set_user(
                        c,
                        user_id, user_id_len,
                        user_name,
                        user_display_name,
                        user_icon);
        if (r != FIDO_OK)
                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                       "Failed to set FIDO2 credential user data: %s", sym_fido_strerr(r));

        r = sym_fido_cred_set_clientdata_hash(c, (const unsigned char[32]) {}, 32);
        if (r != FIDO_OK)
                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                       "Failed to set FIDO2 client data hash: %s", sym_fido_strerr(r));

        if (has_rk) {
                r = sym_fido_cred_set_rk(c, FIDO_OPT_FALSE);
                if (r != FIDO_OK)
                        return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                               "Failed to turn off FIDO2 resident key option of credential: %s", sym_fido_strerr(r));
        }

        if (has_uv) {
                r = sym_fido_cred_set_uv(c, FIDO_OPT_FALSE);
                if (r != FIDO_OK)
                        return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                               "Failed to turn off FIDO2 user verification option of credential: %s", sym_fido_strerr(r));
        }

        /* As per specification "up" is assumed to be implicit when making credentials, hence we don't
         * explicitly enable/disable it here */

        log_info("Initializing FIDO2 credential on security token.");

        if (has_uv || has_up)
                log_notice("%s%s(Hint: This might require confirmation of user presence on security token.)",
                           emoji_enabled() ? special_glyph(SPECIAL_GLYPH_TOUCH) : "",
                           emoji_enabled() ? " " : "");

        r = sym_fido_dev_make_cred(d, c, NULL);
        if (r == FIDO_ERR_PIN_REQUIRED) {

                if (!has_client_pin)
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                               "Token asks for PIN but doesn't advertise 'clientPin' feature.");

                for (;;) {
                        _cleanup_(strv_free_erasep) char **pin = NULL;
                        char **i;

                        r = ask_password_auto("Please enter security token PIN:", askpw_icon_name, NULL, "fido2-pin", "fido2-pin", USEC_INFINITY, 0, &pin);
                        if (r < 0)
                                return log_error_errno(r, "Failed to acquire user PIN: %m");

                        r = FIDO_ERR_PIN_INVALID;
                        STRV_FOREACH(i, pin) {
                                if (isempty(*i)) {
                                        log_notice("PIN may not be empty.");
                                        continue;
                                }

                                r = sym_fido_dev_make_cred(d, c, *i);
                                if (r == FIDO_OK) {
                                        used_pin = strdup(*i);
                                        if (!used_pin)
                                                return log_oom();
                                        break;
                                }
                                if (r != FIDO_ERR_PIN_INVALID)
                                        break;
                        }

                        if (r != FIDO_ERR_PIN_INVALID)
                                break;

                        log_notice("PIN incorrect, please try again.");
                }
        }
        if (r == FIDO_ERR_PIN_AUTH_BLOCKED)
                return log_notice_errno(SYNTHETIC_ERRNO(EPERM),
                                        "Token PIN is currently blocked, please remove and reinsert token.");
#ifdef FIDO_ERR_UV_BLOCKED
        if (r == FIDO_ERR_UV_BLOCKED)
                return log_notice_errno(SYNTHETIC_ERRNO(EPERM),
                                        "Token verification is currently blocked, please remove and reinsert token.");
#endif
        if (r == FIDO_ERR_ACTION_TIMEOUT)
                return log_error_errno(SYNTHETIC_ERRNO(ENOSTR),
                                       "Token action timeout. (User didn't interact with token quickly enough.)");
        if (r != FIDO_OK)
                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                       "Failed to generate FIDO2 credential: %s", sym_fido_strerr(r));

        cid = sym_fido_cred_id_ptr(c);
        if (!cid)
                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Failed to get FIDO2 credential ID.");

        cid_size = sym_fido_cred_id_len(c);

        a = sym_fido_assert_new();
        if (!a)
                return log_oom();

        r = sym_fido_assert_set_extensions(a, FIDO_EXT_HMAC_SECRET);
        if (r != FIDO_OK)
                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                       "Failed to enable HMAC-SECRET extension on FIDO2 assertion: %s", sym_fido_strerr(r));

        r = sym_fido_assert_set_hmac_salt(a, salt, FIDO2_SALT_SIZE);
        if (r != FIDO_OK)
                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                       "Failed to set salt on FIDO2 assertion: %s", sym_fido_strerr(r));

        r = sym_fido_assert_set_rp(a, rp_id);
        if (r != FIDO_OK)
                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                       "Failed to set FIDO2 assertion ID: %s", sym_fido_strerr(r));

        r = sym_fido_assert_set_clientdata_hash(a, (const unsigned char[32]) {}, 32);
        if (r != FIDO_OK)
                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                       "Failed to set FIDO2 assertion client data hash: %s", sym_fido_strerr(r));

        r = sym_fido_assert_allow_cred(a, cid, cid_size);
        if (r != FIDO_OK)
                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                       "Failed to add FIDO2 assertion credential ID: %s", sym_fido_strerr(r));

        log_info("Generating secret key on FIDO2 security token.");

        if (has_up) {
                r = sym_fido_assert_set_up(a, FLAGS_SET(lock_with, FIDO2ENROLL_UP) ? FIDO_OPT_TRUE : FIDO_OPT_FALSE);
                if (r != FIDO_OK)
                        return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                               "Failed to %s FIDO2 user presence test: %s",
                                               enable_disable(FLAGS_SET(lock_with, FIDO2ENROLL_UP)),
                                               sym_fido_strerr(r));

                if (FLAGS_SET(lock_with, FIDO2ENROLL_UP))
                        log_notice("%s%sIn order to allow secret key generation, please confirm presence on security token.",
                                   emoji_enabled() ? special_glyph(SPECIAL_GLYPH_TOUCH) : "",
                                   emoji_enabled() ? " " : "");
        }

        if (has_uv) {
                r = sym_fido_assert_set_uv(a, FLAGS_SET(lock_with, FIDO2ENROLL_UV) ? FIDO_OPT_TRUE : FIDO_OPT_FALSE);
                if (r != FIDO_OK)
                        return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                               "Failed to %s FIDO user verification: %s",
                                               enable_disable(FLAGS_SET(lock_with, FIDO2ENROLL_UV)),
                                               sym_fido_strerr(r));

                if (FLAGS_SET(lock_with, FIDO2ENROLL_UV))
                        log_notice("%s%sIn order to allow secret key generation, please verify user on security token.",
                                   emoji_enabled() ? special_glyph(SPECIAL_GLYPH_TOUCH) : "",
                                   emoji_enabled() ? " " : "");
        }

        for (;;) {
                bool retry_with_up = false, retry_with_pin = false;

                r = sym_fido_dev_get_assert(d, a, FLAGS_SET(lock_with, FIDO2ENROLL_PIN) ? used_pin : NULL);

                switch (r) {

                case FIDO_ERR_UP_REQUIRED:
                        /* If the token asks for "up" when we turn off, then this might be a feature that
                         * isn't optional. Let's enable it */

                        if (!has_up)
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                                       "Token asks for user presence test but doesn't advertise 'up' feature.");

                        if (FLAGS_SET(lock_with, FIDO2ENROLL_UP))
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                                       "Token asks for user presence test but was already enabled.");

                        log_notice("%s%sLocking without user presence test requested, but FIDO2 device %s requires it, enabling.",
                                   emoji_enabled() ? special_glyph(SPECIAL_GLYPH_TOUCH) : "",
                                   emoji_enabled() ? " " : "",
                                   device);

                        retry_with_up = true;
                        break;

                case FIDO_ERR_UNSUPPORTED_OPTION:
                        /* AuthenTrend ATKey.Pro says it supports "up", but if we disable it it will fail
                         * with FIDO_ERR_UNSUPPORTED_OPTION, probably because it isn't actually
                         * optional. Let's see if turning it on works. This is very similar to the
                         * FIDO_ERR_UP_REQUIRED case, but since the error is so vague we implement it
                         * slightly more defensively. */

                        if (has_up && !FLAGS_SET(lock_with, FIDO2ENROLL_UP)) {
                                log_notice("%s%sGot unsupported option error when when user presence test is turned off. Trying with user presence test turned on.",
                                           emoji_enabled() ? special_glyph(SPECIAL_GLYPH_TOUCH) : "",
                                           emoji_enabled() ? " " : "");
                                retry_with_up = true;
                        }

                        break;

                case FIDO_ERR_PIN_REQUIRED:
                        if (!has_client_pin)
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                                       "Token asks for client PIN check but doesn't advertise 'clientPin' feature.");

                        if (FLAGS_SET(lock_with, FIDO2ENROLL_PIN))
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                                       "Token asks for user client PIN check but was already enabled.");

                        log_debug("Token requires PIN for assertion, enabling.");
                        retry_with_pin = true;
                        break;

                default:
                        break;
                }

                if (!retry_with_up && !retry_with_pin)
                        break;

                if (retry_with_up) {
                        r = sym_fido_assert_set_up(a, FIDO_OPT_TRUE);
                        if (r != FIDO_OK)
                                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Failed to enable FIDO2 user presence test: %s", sym_fido_strerr(r));

                        lock_with |= FIDO2ENROLL_UP;
                }

                if (retry_with_pin)
                        lock_with |= FIDO2ENROLL_PIN;
        }

        if (r == FIDO_ERR_ACTION_TIMEOUT)
                return log_error_errno(SYNTHETIC_ERRNO(ENOSTR),
                                       "Token action timeout. (User didn't interact with token quickly enough.)");
        if (r != FIDO_OK)
                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                       "Failed to ask token for assertion: %s", sym_fido_strerr(r));

        secret = sym_fido_assert_hmac_secret_ptr(a, 0);
        if (!secret)
                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Failed to retrieve HMAC secret.");

        secret_size = sym_fido_assert_hmac_secret_len(a, 0);

        secret_copy = memdup(secret, secret_size);
        if (!secret_copy)
                return log_oom();

        cid_copy = memdup(cid, cid_size);
        if (!cid_copy)
                return log_oom();

        *ret_cid = TAKE_PTR(cid_copy);
        *ret_cid_size = cid_size;
        *ret_salt = TAKE_PTR(salt);
        *ret_salt_size = FIDO2_SALT_SIZE;
        *ret_secret = TAKE_PTR(secret_copy);
        *ret_secret_size = secret_size;

        if (ret_usedpin)
                *ret_usedpin = TAKE_PTR(used_pin);

        if (ret_locked_with)
                *ret_locked_with = lock_with;

        return 0;
}
#endif

#if HAVE_LIBFIDO2
static int check_device_is_fido2_with_hmac_secret(const char *path) {
        _cleanup_(fido_dev_free_wrapper) fido_dev_t *d = NULL;
        int r;

        d = sym_fido_dev_new();
        if (!d)
                return log_oom();

        r = sym_fido_dev_open(d, path);
        if (r != FIDO_OK)
                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                       "Failed to open FIDO2 device %s: %s", path, sym_fido_strerr(r));

        r = verify_features(d, path, LOG_DEBUG, NULL, NULL, NULL, NULL);
        if (r == -ENODEV) /* Not a FIDO2 device, or not implementing 'hmac-secret' */
                return false;
        if (r < 0)
                return r;

        return true;
}
#endif

int fido2_list_devices(void) {
#if HAVE_LIBFIDO2
        _cleanup_(table_unrefp) Table *t = NULL;
        size_t allocated = 64, found = 0;
        fido_dev_info_t *di = NULL;
        int r;

        r = dlopen_libfido2();
        if (r < 0)
                return log_error_errno(r, "FIDO2 token support is not installed.");

        di = sym_fido_dev_info_new(allocated);
        if (!di)
                return log_oom();

        r = sym_fido_dev_info_manifest(di, allocated, &found);
        if (r == FIDO_ERR_INTERNAL || (r == FIDO_OK && found == 0)) {
                /* The library returns FIDO_ERR_INTERNAL when no devices are found. I wish it wouldn't. */
                log_info("No FIDO2 devices found.");
                r = 0;
                goto finish;
        }
        if (r != FIDO_OK) {
                r = log_error_errno(SYNTHETIC_ERRNO(EIO), "Failed to enumerate FIDO2 devices: %s", sym_fido_strerr(r));
                goto finish;
        }

        t = table_new("path", "manufacturer", "product");
        if (!t) {
                r = log_oom();
                goto finish;
        }

        for (size_t i = 0; i < found; i++) {
                const fido_dev_info_t *entry;

                entry = sym_fido_dev_info_ptr(di, i);
                if (!entry) {
                        r = log_error_errno(SYNTHETIC_ERRNO(EIO),
                                            "Failed to get device information for FIDO device %zu.", i);
                        goto finish;
                }

                r = check_device_is_fido2_with_hmac_secret(sym_fido_dev_info_path(entry));
                if (r < 0)
                        goto finish;
                if (!r)
                        continue;

                r = table_add_many(
                                t,
                                TABLE_PATH, sym_fido_dev_info_path(entry),
                                TABLE_STRING, sym_fido_dev_info_manufacturer_string(entry),
                                TABLE_STRING, sym_fido_dev_info_product_string(entry));
                if (r < 0) {
                        table_log_add_error(r);
                        goto finish;
                }
        }

        r = table_print(t, stdout);
        if (r < 0) {
                log_error_errno(r, "Failed to show device table: %m");
                goto finish;
        }

        r = 0;

finish:
        sym_fido_dev_info_free(&di, allocated);
        return r;
#else
        return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                               "FIDO2 tokens not supported on this build.");
#endif
}

int fido2_find_device_auto(char **ret) {
#if HAVE_LIBFIDO2
        _cleanup_free_ char *copy = NULL;
        size_t di_size = 64, found = 0;
        const fido_dev_info_t *entry;
        fido_dev_info_t *di = NULL;
        const char *path;
        int r;

        r = dlopen_libfido2();
        if (r < 0)
                return log_error_errno(r, "FIDO2 token support is not installed.");

        di = sym_fido_dev_info_new(di_size);
        if (!di)
                return log_oom();

        r = sym_fido_dev_info_manifest(di, di_size, &found);
        if (r == FIDO_ERR_INTERNAL || (r == FIDO_OK && found == 0)) {
                /* The library returns FIDO_ERR_INTERNAL when no devices are found. I wish it wouldn't. */
                r = log_error_errno(SYNTHETIC_ERRNO(ENODEV), "No FIDO devices found.");
                goto finish;
        }
        if (r != FIDO_OK) {
                r = log_error_errno(SYNTHETIC_ERRNO(EIO), "Failed to enumerate FIDO devices: %s", sym_fido_strerr(r));
                goto finish;
        }
        if (found > 1) {
                r = log_error_errno(SYNTHETIC_ERRNO(ENOTUNIQ), "More than one FIDO device found.");
                goto finish;
        }

        entry = sym_fido_dev_info_ptr(di, 0);
        if (!entry) {
                r = log_error_errno(SYNTHETIC_ERRNO(EIO),
                                    "Failed to get device information for FIDO device 0.");
                goto finish;
        }

        r = check_device_is_fido2_with_hmac_secret(sym_fido_dev_info_path(entry));
        if (r < 0)
                goto finish;
        if (!r) {
                r = log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP), "FIDO device discovered does not implement FIDO2 with 'hmac-secret' extension.");
                goto finish;
        }

        path = sym_fido_dev_info_path(entry);
        if (!path) {
                r = log_error_errno(SYNTHETIC_ERRNO(EIO),
                                    "Failed to query FIDO device path.");
                goto finish;
        }

        copy = strdup(path);
        if (!copy) {
                r = log_oom();
                goto finish;
        }

        *ret = TAKE_PTR(copy);
        r = 0;

finish:
        sym_fido_dev_info_free(&di, di_size);
        return r;
#else
        return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                               "FIDO2 tokens not supported on this build.");
#endif
}
