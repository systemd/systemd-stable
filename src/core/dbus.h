/* SPDX-License-Identifier: LGPL-2.1+ */
#pragma once

#include "sd-bus.h"

#include "manager.h"

int bus_send_pending_reload_message(Manager *m);

int bus_init_private(Manager *m);
int bus_init_api(Manager *m);
int bus_init_system(Manager *m);

void bus_done_private(Manager *m);
void bus_done_api(Manager *m);
void bus_done_system(Manager *m);
void bus_done(Manager *m);

int bus_fdset_add_all(Manager *m, FDSet *fds);

void bus_track_serialize(sd_bus_track *t, FILE *f, const char *prefix);
int bus_track_coldplug(Manager *m, sd_bus_track **t, bool recursive, char **l);

int manager_enqueue_sync_bus_names(Manager *m);

int bus_foreach_bus(Manager *m, sd_bus_track *subscribed2, int (*send_message)(sd_bus *bus, void *userdata), void *userdata);

int bus_verify_manage_units_async(Manager *m, sd_bus_message *call, sd_bus_error *error);
int bus_verify_manage_unit_files_async(Manager *m, sd_bus_message *call, sd_bus_error *error);
int bus_verify_reload_daemon_async(Manager *m, sd_bus_message *call, sd_bus_error *error);
int bus_verify_set_environment_async(Manager *m, sd_bus_message *call, sd_bus_error *error);

int bus_forward_agent_released(Manager *m, const char *path);

uint64_t manager_bus_n_queued_write(Manager *m);

void dump_bus_properties(FILE *f);
