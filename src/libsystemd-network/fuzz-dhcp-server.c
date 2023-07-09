/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "fuzz.h"

#include "sd-dhcp-server.c"

/* stub out network so that the server doesn't send */
ssize_t sendto(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen) {
        return len;
}

ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags) {
        return 0;
}

static int add_lease(sd_dhcp_server *server, const struct in_addr *server_address, uint8_t i) {
        _cleanup_(dhcp_lease_freep) DHCPLease *lease = NULL;
        static const uint8_t chaddr[] = {3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3};
        int r;

        assert(server);

        lease = new0(DHCPLease, 1);
        if (!lease)
                return -ENOMEM;

        lease->client_id.data = malloc(2);
        if (!lease->client_id.data)
                return -ENOMEM;

        lease->client_id.length = 2;
        lease->client_id.data[0] = 2;
        lease->client_id.data[1] = i;

        lease->address = htobe32(UINT32_C(10) << 24 | i);
        lease->gateway = server_address->s_addr;
        lease->expiration = UINT64_MAX;
        lease->htype = ARPHRD_ETHER;
        lease->hlen = ETH_ALEN;
        memcpy(lease->chaddr, chaddr, ETH_ALEN);

        lease->server = server; /* This must be set just before hashmap_put(). */

        r = hashmap_ensure_put(&server->bound_leases_by_client_id, &dhcp_lease_hash_ops, &lease->client_id, lease);
        if (r < 0)
                return r;

        r = hashmap_ensure_put(&server->bound_leases_by_address, NULL, UINT32_TO_PTR(lease->address), lease);
        if (r < 0)
                return r;

        TAKE_PTR(lease);

        return 0;
}

static int add_static_lease(sd_dhcp_server *server, uint8_t i) {
        uint8_t id[2] = { 2, i };
        int r;

        assert(server);

        r = sd_dhcp_server_set_static_lease(
                                server,
                                &(struct in_addr) { .s_addr = htobe32(UINT32_C(10) << 24 | i)},
                                id, ELEMENTSOF(id));

        return r;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
        _cleanup_(sd_dhcp_server_unrefp) sd_dhcp_server *server = NULL;
        struct in_addr address = { .s_addr = htobe32(UINT32_C(10) << 24 | UINT32_C(1))};
        _cleanup_free_ uint8_t *duped = NULL;

        if (size < sizeof(DHCPMessage))
                return 0;

        assert_se(duped = memdup(data, size));

        assert_se(sd_dhcp_server_new(&server, 1) >= 0);
        assert_se(sd_dhcp_server_attach_event(server, NULL, 0) >= 0);
        server->fd = open("/dev/null", O_RDWR|O_CLOEXEC|O_NOCTTY);
        assert_se(server->fd >= 0);
        assert_se(sd_dhcp_server_configure_pool(server, &address, 24, 0, 0) >= 0);

        /* add leases to the pool to expose additional code paths */
        assert_se(add_lease(server, &address, 2) >= 0);
        assert_se(add_lease(server, &address, 3) >= 0);

        /* add static leases */
        assert_se(add_static_lease(server, 3) >= 0);
        assert_se(add_static_lease(server, 4) >= 0);

        (void) dhcp_server_handle_message(server, (DHCPMessage*) duped, size);

        return 0;
}
