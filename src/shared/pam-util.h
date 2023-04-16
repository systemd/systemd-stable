/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <security/pam_modules.h>

#include "sd-bus.h"

int pam_log_oom(pam_handle_t *handle);
int pam_bus_log_create_error(pam_handle_t *handle, int r);
int pam_bus_log_parse_error(pam_handle_t *handle, int r);

/* Use a different module name per different PAM module. They are all loaded in the same namespace, and this
 * helps avoid a clash in the internal data structures of sd-bus. It will be used as key for cache items. */
int pam_acquire_bus_connection(pam_handle_t *handle, const char *module_name, sd_bus **ret);
int pam_release_bus_connection(pam_handle_t *handle, const char *module_name);

void pam_cleanup_free(pam_handle_t *handle, void *data, int error_status);
