/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "ask-password-api.h"
#include "cryptenroll-password.h"
#include "env-util.h"
#include "escape.h"
#include "memory-util.h"
#include "pwquality-util.h"
#include "strv.h"

int load_volume_key_password(
                struct crypt_device *cd,
                const char *cd_node,
                void *ret_vk,
                size_t *ret_vks) {

        _cleanup_(erase_and_freep) char *envpw = NULL;
        int r;

        assert_se(cd);
        assert_se(cd_node);
        assert_se(ret_vk);
        assert_se(ret_vks);

        r = getenv_steal_erase("PASSWORD", &envpw);
        if (r < 0)
                return log_error_errno(r, "Failed to acquire password from environment: %m");
        if (r > 0) {
                r = crypt_volume_key_get(
                                cd,
                                CRYPT_ANY_SLOT,
                                ret_vk,
                                ret_vks,
                                envpw,
                                strlen(envpw));
                if (r < 0)
                        return log_error_errno(r, "Password from environment variable $PASSWORD did not work.");
        } else {
                AskPasswordFlags ask_password_flags = ASK_PASSWORD_PUSH_CACHE|ASK_PASSWORD_ACCEPT_CACHED;
                _cleanup_free_ char *question = NULL, *disk_path = NULL;
                unsigned i = 5;
                const char *id;

                question = strjoin("Please enter current passphrase for disk ", cd_node, ":");
                if (!question)
                        return log_oom();

                disk_path = cescape(cd_node);
                if (!disk_path)
                        return log_oom();

                id = strjoina("cryptsetup:", disk_path);

                for (;;) {
                        _cleanup_strv_free_erase_ char **passwords = NULL;

                        if (--i == 0)
                                return log_error_errno(SYNTHETIC_ERRNO(ENOKEY),
                                                       "Too many attempts, giving up:");

                        r = ask_password_auto(
                                        question, "drive-harddisk", id, "cryptenroll", "cryptenroll.passphrase", USEC_INFINITY,
                                        ask_password_flags,
                                        &passwords);
                        if (r < 0)
                                return log_error_errno(r, "Failed to query password: %m");

                        r = -EPERM;
                        STRV_FOREACH(p, passwords) {
                                r = crypt_volume_key_get(
                                                cd,
                                                CRYPT_ANY_SLOT,
                                                ret_vk,
                                                ret_vks,
                                                *p,
                                                strlen(*p));
                                if (r >= 0)
                                        break;
                        }
                        if (r >= 0)
                                break;

                        log_error_errno(r, "Password not correct, please try again.");
                        ask_password_flags &= ~ASK_PASSWORD_ACCEPT_CACHED;
                }
        }

        return r;
}

int enroll_password(
                struct crypt_device *cd,
                const void *volume_key,
                size_t volume_key_size) {

        _cleanup_(erase_and_freep) char *new_password = NULL;
        _cleanup_free_ char *error = NULL;
        const char *node;
        int r, keyslot;

        assert_se(node = crypt_get_device_name(cd));

        r = getenv_steal_erase("NEWPASSWORD", &new_password);
        if (r < 0)
                return log_error_errno(r, "Failed to acquire password from environment: %m");
        if (r == 0) {
                _cleanup_free_ char *disk_path = NULL;
                unsigned i = 5;
                const char *id;

                assert_se(node = crypt_get_device_name(cd));

                (void) suggest_passwords();

                disk_path = cescape(node);
                if (!disk_path)
                        return log_oom();

                id = strjoina("cryptsetup:", disk_path);

                for (;;) {
                        _cleanup_strv_free_erase_ char **passwords = NULL, **passwords2 = NULL;
                        _cleanup_free_ char *question = NULL;

                        if (--i == 0)
                                return log_error_errno(SYNTHETIC_ERRNO(ENOKEY),
                                                       "Too many attempts, giving up:");

                        question = strjoin("Please enter new passphrase for disk ", node, ":");
                        if (!question)
                                return log_oom();

                        r = ask_password_auto(question, "drive-harddisk", id, "cryptenroll", "cryptenroll.new-passphrase", USEC_INFINITY, 0, &passwords);
                        if (r < 0)
                                return log_error_errno(r, "Failed to query password: %m");

                        assert(strv_length(passwords) == 1);

                        free(question);
                        question = strjoin("Please enter new passphrase for disk ", node, " (repeat):");
                        if (!question)
                                return log_oom();

                        r = ask_password_auto(question, "drive-harddisk", id, "cryptenroll", "cryptenroll.new-passphrase", USEC_INFINITY, 0, &passwords2);
                        if (r < 0)
                                return log_error_errno(r, "Failed to query password: %m");

                        assert(strv_length(passwords2) == 1);

                        if (strv_equal(passwords, passwords2)) {
                                new_password = passwords2[0];
                                passwords2 = mfree(passwords2);
                                break;
                        }

                        log_error("Password didn't match, try again.");
                }
        }

        r = quality_check_password(new_password, NULL, &error);
        if (r < 0)
                return log_error_errno(r, "Failed to check password for quality: %m");
        if (r == 0)
                log_warning("Specified password does not pass quality checks (%s), proceeding anyway.", error);

        keyslot = crypt_keyslot_add_by_volume_key(
                        cd,
                        CRYPT_ANY_SLOT,
                        volume_key,
                        volume_key_size,
                        new_password,
                        strlen(new_password));
        if (keyslot < 0)
                return log_error_errno(keyslot, "Failed to add new password to %s: %m", node);

        log_info("New password enrolled as key slot %i.", keyslot);
        return keyslot;
}
