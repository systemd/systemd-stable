/* SPDX-License-Identifier: LGPL-2.1+ */

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "libudev.h"

#include "macro.h"
#include "udev-util.h"

static void* thread(void *p) {
        struct udev_device **d = p;

        assert_se(!(*d = udev_device_unref(*d)));

        return NULL;
}

int main(int argc, char *argv[]) {
        _cleanup_(udev_unrefp) struct udev *udev;
        struct udev_device *loopback;
        pthread_t t;

        assert_se(unsetenv("SYSTEMD_MEMPOOL") == 0);

        assert_se(udev = udev_new());
        assert_se(loopback = udev_device_new_from_syspath(udev, "/sys/class/net/lo"));

        assert_se(udev_device_get_properties_list_entry(loopback));

        assert_se(pthread_create(&t, NULL, thread, &loopback) == 0);
        assert_se(pthread_join(t, NULL) == 0);

        assert_se(!loopback);

        return 0;
}
