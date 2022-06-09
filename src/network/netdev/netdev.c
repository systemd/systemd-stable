/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <net/if.h>
#include <netinet/in.h>
#include <linux/if_arp.h>
#include <unistd.h>

#include "alloc-util.h"
#include "arphrd-util.h"
#include "bareudp.h"
#include "batadv.h"
#include "bond.h"
#include "bridge.h"
#include "conf-files.h"
#include "conf-parser.h"
#include "dummy.h"
#include "fd-util.h"
#include "fou-tunnel.h"
#include "geneve.h"
#include "ifb.h"
#include "ipoib.h"
#include "ipvlan.h"
#include "l2tp-tunnel.h"
#include "list.h"
#include "macsec.h"
#include "macvlan.h"
#include "netdev.h"
#include "netdevsim.h"
#include "netif-util.h"
#include "netlink-util.h"
#include "networkd-manager.h"
#include "networkd-queue.h"
#include "networkd-setlink.h"
#include "nlmon.h"
#include "path-lookup.h"
#include "siphash24.h"
#include "stat-util.h"
#include "string-table.h"
#include "string-util.h"
#include "strv.h"
#include "tunnel.h"
#include "tuntap.h"
#include "vcan.h"
#include "veth.h"
#include "vlan.h"
#include "vrf.h"
#include "vxcan.h"
#include "vxlan.h"
#include "wireguard.h"
#include "xfrm.h"

const NetDevVTable * const netdev_vtable[_NETDEV_KIND_MAX] = {
        [NETDEV_KIND_BAREUDP]   = &bare_udp_vtable,
        [NETDEV_KIND_BATADV]    = &batadv_vtable,
        [NETDEV_KIND_BOND]      = &bond_vtable,
        [NETDEV_KIND_BRIDGE]    = &bridge_vtable,
        [NETDEV_KIND_DUMMY]     = &dummy_vtable,
        [NETDEV_KIND_ERSPAN]    = &erspan_vtable,
        [NETDEV_KIND_FOU]       = &foutnl_vtable,
        [NETDEV_KIND_GENEVE]    = &geneve_vtable,
        [NETDEV_KIND_GRE]       = &gre_vtable,
        [NETDEV_KIND_GRETAP]    = &gretap_vtable,
        [NETDEV_KIND_IFB]       = &ifb_vtable,
        [NETDEV_KIND_IP6GRE]    = &ip6gre_vtable,
        [NETDEV_KIND_IP6GRETAP] = &ip6gretap_vtable,
        [NETDEV_KIND_IP6TNL]    = &ip6tnl_vtable,
        [NETDEV_KIND_IPIP]      = &ipip_vtable,
        [NETDEV_KIND_IPOIB]     = &ipoib_vtable,
        [NETDEV_KIND_IPVLAN]    = &ipvlan_vtable,
        [NETDEV_KIND_IPVTAP]    = &ipvtap_vtable,
        [NETDEV_KIND_L2TP]      = &l2tptnl_vtable,
        [NETDEV_KIND_MACSEC]    = &macsec_vtable,
        [NETDEV_KIND_MACVLAN]   = &macvlan_vtable,
        [NETDEV_KIND_MACVTAP]   = &macvtap_vtable,
        [NETDEV_KIND_NETDEVSIM] = &netdevsim_vtable,
        [NETDEV_KIND_NLMON]     = &nlmon_vtable,
        [NETDEV_KIND_SIT]       = &sit_vtable,
        [NETDEV_KIND_TAP]       = &tap_vtable,
        [NETDEV_KIND_TUN]       = &tun_vtable,
        [NETDEV_KIND_VCAN]      = &vcan_vtable,
        [NETDEV_KIND_VETH]      = &veth_vtable,
        [NETDEV_KIND_VLAN]      = &vlan_vtable,
        [NETDEV_KIND_VRF]       = &vrf_vtable,
        [NETDEV_KIND_VTI6]      = &vti6_vtable,
        [NETDEV_KIND_VTI]       = &vti_vtable,
        [NETDEV_KIND_VXCAN]     = &vxcan_vtable,
        [NETDEV_KIND_VXLAN]     = &vxlan_vtable,
        [NETDEV_KIND_WIREGUARD] = &wireguard_vtable,
        [NETDEV_KIND_XFRM]      = &xfrm_vtable,
};

static const char* const netdev_kind_table[_NETDEV_KIND_MAX] = {
        [NETDEV_KIND_BAREUDP]   = "bareudp",
        [NETDEV_KIND_BATADV]    = "batadv",
        [NETDEV_KIND_BOND]      = "bond",
        [NETDEV_KIND_BRIDGE]    = "bridge",
        [NETDEV_KIND_DUMMY]     = "dummy",
        [NETDEV_KIND_ERSPAN]    = "erspan",
        [NETDEV_KIND_FOU]       = "fou",
        [NETDEV_KIND_GENEVE]    = "geneve",
        [NETDEV_KIND_GRE]       = "gre",
        [NETDEV_KIND_GRETAP]    = "gretap",
        [NETDEV_KIND_IFB]       = "ifb",
        [NETDEV_KIND_IP6GRE]    = "ip6gre",
        [NETDEV_KIND_IP6GRETAP] = "ip6gretap",
        [NETDEV_KIND_IP6TNL]    = "ip6tnl",
        [NETDEV_KIND_IPIP]      = "ipip",
        [NETDEV_KIND_IPOIB]     = "ipoib",
        [NETDEV_KIND_IPVLAN]    = "ipvlan",
        [NETDEV_KIND_IPVTAP]    = "ipvtap",
        [NETDEV_KIND_L2TP]      = "l2tp",
        [NETDEV_KIND_MACSEC]    = "macsec",
        [NETDEV_KIND_MACVLAN]   = "macvlan",
        [NETDEV_KIND_MACVTAP]   = "macvtap",
        [NETDEV_KIND_NETDEVSIM] = "netdevsim",
        [NETDEV_KIND_NLMON]     = "nlmon",
        [NETDEV_KIND_SIT]       = "sit",
        [NETDEV_KIND_TAP]       = "tap",
        [NETDEV_KIND_TUN]       = "tun",
        [NETDEV_KIND_VCAN]      = "vcan",
        [NETDEV_KIND_VETH]      = "veth",
        [NETDEV_KIND_VLAN]      = "vlan",
        [NETDEV_KIND_VRF]       = "vrf",
        [NETDEV_KIND_VTI6]      = "vti6",
        [NETDEV_KIND_VTI]       = "vti",
        [NETDEV_KIND_VXCAN]     = "vxcan",
        [NETDEV_KIND_VXLAN]     = "vxlan",
        [NETDEV_KIND_WIREGUARD] = "wireguard",
        [NETDEV_KIND_XFRM]      = "xfrm",
};

DEFINE_STRING_TABLE_LOOKUP(netdev_kind, NetDevKind);

bool netdev_is_managed(NetDev *netdev) {
        if (!netdev || !netdev->manager || !netdev->ifname)
                return false;

        return hashmap_get(netdev->manager->netdevs, netdev->ifname) == netdev;
}

static bool netdev_is_stacked_and_independent(NetDev *netdev) {
        assert(netdev);

        if (!IN_SET(netdev_get_create_type(netdev), NETDEV_CREATE_STACKED, NETDEV_CREATE_AFTER_CONFIGURED))
                return false;

        switch (netdev->kind) {
        case NETDEV_KIND_ERSPAN:
                return ERSPAN(netdev)->independent;
        case NETDEV_KIND_GRE:
                return GRE(netdev)->independent;
        case NETDEV_KIND_GRETAP:
                return GRETAP(netdev)->independent;
        case NETDEV_KIND_IP6GRE:
                return IP6GRE(netdev)->independent;
        case NETDEV_KIND_IP6GRETAP:
                return IP6GRETAP(netdev)->independent;
        case NETDEV_KIND_IP6TNL:
                return IP6TNL(netdev)->independent;
        case NETDEV_KIND_IPIP:
                return IPIP(netdev)->independent;
        case NETDEV_KIND_SIT:
                return SIT(netdev)->independent;
        case NETDEV_KIND_VTI:
                return VTI(netdev)->independent;
        case NETDEV_KIND_VTI6:
                return VTI6(netdev)->independent;
        case NETDEV_KIND_VXLAN:
                return VXLAN(netdev)->independent;
        case NETDEV_KIND_XFRM:
                return XFRM(netdev)->independent;
        default:
                return false;
        }
}

static bool netdev_is_stacked(NetDev *netdev) {
        assert(netdev);

        if (!IN_SET(netdev_get_create_type(netdev), NETDEV_CREATE_STACKED, NETDEV_CREATE_AFTER_CONFIGURED))
                return false;

        if (netdev_is_stacked_and_independent(netdev))
                return false;

        return true;
}

static void netdev_detach_from_manager(NetDev *netdev) {
        if (netdev->ifname && netdev->manager)
                hashmap_remove(netdev->manager->netdevs, netdev->ifname);
}

static NetDev *netdev_free(NetDev *netdev) {
        assert(netdev);

        netdev_detach_from_manager(netdev);

        free(netdev->filename);

        free(netdev->description);
        free(netdev->ifname);
        condition_free_list(netdev->conditions);

        /* Invoke the per-kind done() destructor, but only if the state field is initialized. We conditionalize that
         * because we parse .netdev files twice: once to determine the kind (with a short, minimal NetDev structure
         * allocation, with no room for per-kind fields), and once to read the kind's properties (with a full,
         * comprehensive NetDev structure allocation with enough space for whatever the specific kind needs). Now, in
         * the first case we shouldn't try to destruct the per-kind NetDev fields on destruction, in the second case we
         * should. We use the state field to discern the two cases: it's _NETDEV_STATE_INVALID on the first "raw"
         * call. */
        if (netdev->state != _NETDEV_STATE_INVALID &&
            NETDEV_VTABLE(netdev) &&
            NETDEV_VTABLE(netdev)->done)
                NETDEV_VTABLE(netdev)->done(netdev);

        return mfree(netdev);
}

DEFINE_TRIVIAL_REF_UNREF_FUNC(NetDev, netdev, netdev_free);

void netdev_drop(NetDev *netdev) {
        if (!netdev)
                return;

        if (netdev_is_stacked(netdev)) {
                /* The netdev may be removed due to the underlying device removal, and the device may
                 * be re-added later. */
                netdev->state = NETDEV_STATE_LOADING;
                netdev->ifindex = 0;

                log_netdev_debug(netdev, "netdev removed");
                return;
        }

        netdev->state = NETDEV_STATE_LINGER;

        log_netdev_debug(netdev, "netdev removed");

        netdev_detach_from_manager(netdev);
        netdev_unref(netdev);
        return;
}

int netdev_get(Manager *manager, const char *name, NetDev **ret) {
        NetDev *netdev;

        assert(manager);
        assert(name);
        assert(ret);

        netdev = hashmap_get(manager->netdevs, name);
        if (!netdev) {
                *ret = NULL;
                return -ENOENT;
        }

        *ret = netdev;

        return 0;
}

void netdev_enter_failed(NetDev *netdev) {
        netdev->state = NETDEV_STATE_FAILED;
}

static int netdev_enter_ready(NetDev *netdev) {
        assert(netdev);
        assert(netdev->ifname);

        if (netdev->state != NETDEV_STATE_CREATING)
                return 0;

        netdev->state = NETDEV_STATE_READY;

        log_netdev_info(netdev, "netdev ready");

        if (NETDEV_VTABLE(netdev)->post_create)
                NETDEV_VTABLE(netdev)->post_create(netdev, NULL, NULL);

        return 0;
}

/* callback for netdev's created without a backing Link */
static int netdev_create_handler(sd_netlink *rtnl, sd_netlink_message *m, NetDev *netdev) {
        int r;

        assert(netdev);
        assert(netdev->state != _NETDEV_STATE_INVALID);

        r = sd_netlink_message_get_errno(m);
        if (r == -EEXIST)
                log_netdev_info(netdev, "netdev exists, using existing without changing its parameters");
        else if (r < 0) {
                log_netdev_warning_errno(netdev, r, "netdev could not be created: %m");
                netdev_enter_failed(netdev);

                return 1;
        }

        log_netdev_debug(netdev, "Created");

        return 1;
}

int netdev_set_ifindex(NetDev *netdev, sd_netlink_message *message) {
        uint16_t type;
        const char *kind;
        const char *received_kind;
        const char *received_name;
        int r, ifindex;

        assert(netdev);
        assert(message);

        r = sd_netlink_message_get_type(message, &type);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not get rtnl message type: %m");

        if (type != RTM_NEWLINK)
                return log_netdev_error_errno(netdev, SYNTHETIC_ERRNO(EINVAL), "Cannot set ifindex from unexpected rtnl message type.");

        r = sd_rtnl_message_link_get_ifindex(message, &ifindex);
        if (r < 0) {
                log_netdev_error_errno(netdev, r, "Could not get ifindex: %m");
                netdev_enter_failed(netdev);
                return r;
        } else if (ifindex <= 0) {
                log_netdev_error(netdev, "Got invalid ifindex: %d", ifindex);
                netdev_enter_failed(netdev);
                return -EINVAL;
        }

        if (netdev->ifindex > 0) {
                if (netdev->ifindex != ifindex) {
                        log_netdev_error(netdev, "Could not set ifindex to %d, already set to %d",
                                         ifindex, netdev->ifindex);
                        netdev_enter_failed(netdev);
                        return -EEXIST;
                } else
                        /* ifindex already set to the same for this netdev */
                        return 0;
        }

        r = sd_netlink_message_read_string(message, IFLA_IFNAME, &received_name);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not get IFNAME: %m");

        if (!streq(netdev->ifname, received_name)) {
                log_netdev_error(netdev, "Received newlink with wrong IFNAME %s", received_name);
                netdev_enter_failed(netdev);
                return -EINVAL;
        }

        r = sd_netlink_message_enter_container(message, IFLA_LINKINFO);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not get LINKINFO: %m");

        r = sd_netlink_message_read_string(message, IFLA_INFO_KIND, &received_kind);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not get KIND: %m");

        r = sd_netlink_message_exit_container(message);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not exit container: %m");

        if (netdev->kind == NETDEV_KIND_TAP)
                /* the kernel does not distinguish between tun and tap */
                kind = "tun";
        else {
                kind = netdev_kind_to_string(netdev->kind);
                if (!kind) {
                        log_netdev_error(netdev, "Could not get kind");
                        netdev_enter_failed(netdev);
                        return -EINVAL;
                }
        }

        if (!streq(kind, received_kind)) {
                log_netdev_error(netdev, "Received newlink with wrong KIND %s, expected %s",
                                 received_kind, kind);
                netdev_enter_failed(netdev);
                return -EINVAL;
        }

        netdev->ifindex = ifindex;

        log_netdev_debug(netdev, "netdev has index %d", netdev->ifindex);

        netdev_enter_ready(netdev);

        return 0;
}

#define HASH_KEY SD_ID128_MAKE(52,e1,45,bd,00,6f,29,96,21,c6,30,6d,83,71,04,48)

int netdev_generate_hw_addr(
                NetDev *netdev,
                Link *parent,
                const char *name,
                const struct hw_addr_data *hw_addr,
                struct hw_addr_data *ret) {

        struct hw_addr_data a = HW_ADDR_NULL;
        bool is_static = false;
        int r;

        assert(netdev);
        assert(name);
        assert(hw_addr);
        assert(ret);

        if (hw_addr_equal(hw_addr, &HW_ADDR_NONE)) {
                *ret = HW_ADDR_NULL;
                return 0;
        }

        if (hw_addr->length == 0) {
                uint64_t result;

                /* HardwareAddress= is not specified. */

                if (!NETDEV_VTABLE(netdev)->generate_mac)
                        goto finalize;

                if (!IN_SET(NETDEV_VTABLE(netdev)->iftype, ARPHRD_ETHER, ARPHRD_INFINIBAND))
                        goto finalize;

                r = net_get_unique_predictable_data_from_name(name, &HASH_KEY, &result);
                if (r < 0) {
                        log_netdev_warning_errno(netdev, r,
                                                 "Failed to generate persistent MAC address, ignoring: %m");
                        goto finalize;
                }

                a.length = arphrd_to_hw_addr_len(NETDEV_VTABLE(netdev)->iftype);

                switch (NETDEV_VTABLE(netdev)->iftype) {
                case ARPHRD_ETHER:
                        assert(a.length <= sizeof(result));
                        memcpy(a.bytes, &result, a.length);

                        if (ether_addr_is_null(&a.ether) || ether_addr_is_broadcast(&a.ether)) {
                                log_netdev_warning_errno(netdev, SYNTHETIC_ERRNO(EINVAL),
                                                         "Failed to generate persistent MAC address, ignoring: %m");
                                a = HW_ADDR_NULL;
                                goto finalize;
                        }

                        break;
                case ARPHRD_INFINIBAND:
                        if (result == 0) {
                                log_netdev_warning_errno(netdev, SYNTHETIC_ERRNO(EINVAL),
                                                         "Failed to generate persistent MAC address: %m");
                                goto finalize;
                        }

                        assert(a.length >= sizeof(result));
                        memzero(a.bytes, a.length - sizeof(result));
                        memcpy(a.bytes + a.length - sizeof(result), &result, sizeof(result));
                        break;
                default:
                        assert_not_reached();
                }

        } else {
                a = *hw_addr;
                is_static = true;
        }

        r = net_verify_hardware_address(name, is_static, NETDEV_VTABLE(netdev)->iftype,
                                        parent ? &parent->hw_addr : NULL, &a);
        if (r < 0)
                return r;

finalize:
        *ret = a;
        return 0;
}

static int netdev_create(NetDev *netdev, Link *link, link_netlink_message_handler_t callback) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *m = NULL;
        struct hw_addr_data hw_addr;
        int r;

        assert(netdev);
        assert(!link || callback);

        /* create netdev */
        if (NETDEV_VTABLE(netdev)->create) {
                assert(!link);

                r = NETDEV_VTABLE(netdev)->create(netdev);
                if (r < 0)
                        return r;

                log_netdev_debug(netdev, "Created");
                return 0;
        }

        r = sd_rtnl_message_new_link(netdev->manager->rtnl, &m, RTM_NEWLINK, 0);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not allocate RTM_NEWLINK message: %m");

        r = sd_netlink_message_append_string(m, IFLA_IFNAME, netdev->ifname);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not append IFLA_IFNAME, attribute: %m");

        r = netdev_generate_hw_addr(netdev, link, netdev->ifname, &netdev->hw_addr, &hw_addr);
        if (r < 0)
                return r;

        if (hw_addr.length > 0) {
                log_netdev_debug(netdev, "Using MAC address: %s", HW_ADDR_TO_STR(&hw_addr));
                r = netlink_message_append_hw_addr(m, IFLA_ADDRESS, &hw_addr);
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not append IFLA_ADDRESS attribute: %m");
        }

        if (netdev->mtu != 0) {
                r = sd_netlink_message_append_u32(m, IFLA_MTU, netdev->mtu);
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not append IFLA_MTU attribute: %m");
        }

        if (link) {
                r = sd_netlink_message_append_u32(m, IFLA_LINK, link->ifindex);
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not append IFLA_LINK attribute: %m");
        }

        r = sd_netlink_message_open_container(m, IFLA_LINKINFO);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not append IFLA_LINKINFO attribute: %m");

        if (NETDEV_VTABLE(netdev)->fill_message_create) {
                r = sd_netlink_message_open_container_union(m, IFLA_INFO_DATA, netdev_kind_to_string(netdev->kind));
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not append IFLA_INFO_DATA attribute: %m");

                r = NETDEV_VTABLE(netdev)->fill_message_create(netdev, link, m);
                if (r < 0)
                        return r;

                r = sd_netlink_message_close_container(m);
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not append IFLA_INFO_DATA attribute: %m");
        } else {
                r = sd_netlink_message_append_string(m, IFLA_INFO_KIND, netdev_kind_to_string(netdev->kind));
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not append IFLA_INFO_KIND attribute: %m");
        }

        r = sd_netlink_message_close_container(m);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not append IFLA_LINKINFO attribute: %m");

        if (link) {
                r = netlink_call_async(netdev->manager->rtnl, NULL, m, callback,
                                       link_netlink_destroy_callback, link);
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not send rtnetlink message: %m");

                link_ref(link);
        } else {
                r = netlink_call_async(netdev->manager->rtnl, NULL, m, netdev_create_handler,
                                       netdev_destroy_callback, netdev);
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not send rtnetlink message: %m");

                netdev_ref(netdev);
        }

        netdev->state = NETDEV_STATE_CREATING;

        log_netdev_debug(netdev, "Creating");
        return 0;
}

static int netdev_create_after_configured(NetDev *netdev, Link *link) {
        assert(netdev);
        assert(link);
        assert(NETDEV_VTABLE(netdev)->create_after_configured);

        return NETDEV_VTABLE(netdev)->create_after_configured(netdev, link);
}

int netdev_join(NetDev *netdev, Link *link, link_netlink_message_handler_t callback) {
        int r;

        assert(netdev);
        assert(netdev->manager);
        assert(netdev->manager->rtnl);

        switch (netdev_get_create_type(netdev)) {
        case NETDEV_CREATE_STACKED:
                r = netdev_create(netdev, link, callback);
                if (r < 0)
                        return r;

                break;
        case NETDEV_CREATE_AFTER_CONFIGURED:
                r = netdev_create_after_configured(netdev, link);
                if (r < 0)
                        return r;
                break;
        default:
                assert_not_reached();
        }

        return 0;
}

static bool netdev_is_ready_to_create(NetDev *netdev, Link *link) {
        assert(netdev);
        assert(link);

        if (netdev->state != NETDEV_STATE_LOADING)
                return false;

        if (!IN_SET(link->state, LINK_STATE_CONFIGURING, LINK_STATE_CONFIGURED))
                return false;

        if (netdev_get_create_type(netdev) == NETDEV_CREATE_AFTER_CONFIGURED &&
            link->state != LINK_STATE_CONFIGURED)
                return false;

        if (link->set_link_messages > 0)
                return false;

        /* If stacked netdevs are created before the underlying interface being activated, then
         * the activation policy for the netdevs are ignored. See issue #22593. */
        if (!link->activated)
                return false;

        return true;
}

int request_process_stacked_netdev(Request *req) {
        int r;

        assert(req);
        assert(req->link);
        assert(req->type == REQUEST_TYPE_STACKED_NETDEV);
        assert(req->netdev);
        assert(req->netlink_handler);

        if (!netdev_is_ready_to_create(req->netdev, req->link))
                return 0;

        r = netdev_join(req->netdev, req->link, req->netlink_handler);
        if (r < 0)
                return log_link_error_errno(req->link, r, "Failed to create stacked netdev '%s': %m", req->netdev->ifname);

        return 1;
}

static int link_create_stacked_netdev_handler_internal(sd_netlink *rtnl, sd_netlink_message *m, Link *link) {
        int r;

        assert(m);
        assert(link);

        if (IN_SET(link->state, LINK_STATE_FAILED, LINK_STATE_LINGER))
                return 0;

        r = sd_netlink_message_get_errno(m);
        if (r < 0 && r != -EEXIST) {
                log_link_message_warning_errno(link, m, r, "Could not create stacked netdev");
                link_enter_failed(link);
                return 0;
        }

        return 1;
}

static int link_create_stacked_netdev_handler(sd_netlink *rtnl, sd_netlink_message *m, Link *link) {
        assert(link);
        assert(link->create_stacked_netdev_messages > 0);

        link->create_stacked_netdev_messages--;

        if (link_create_stacked_netdev_handler_internal(rtnl, m, link) <= 0)
                return 0;

        if (link->create_stacked_netdev_messages == 0) {
                link->stacked_netdevs_created = true;
                log_link_debug(link, "Stacked netdevs created.");
                link_check_ready(link);
        }

        return 0;
}

static int link_create_stacked_netdev_after_configured_handler(sd_netlink *rtnl, sd_netlink_message *m, Link *link) {
        assert(link);
        assert(link->create_stacked_netdev_after_configured_messages > 0);

        link->create_stacked_netdev_after_configured_messages--;

        if (link_create_stacked_netdev_handler_internal(rtnl, m, link) <= 0)
                return 0;

        if (link->create_stacked_netdev_after_configured_messages == 0) {
                link->stacked_netdevs_after_configured_created = true;
                log_link_debug(link, "Stacked netdevs created.");
        }

        return 0;
}

int link_request_stacked_netdev(Link *link, NetDev *netdev) {
        int r;

        assert(link);
        assert(netdev);

        if (!netdev_is_stacked(netdev))
                return -EINVAL;

        if (!IN_SET(netdev->state, NETDEV_STATE_LOADING, NETDEV_STATE_FAILED) || netdev->ifindex > 0)
                return 0; /* Already created. */

        if (netdev_get_create_type(netdev) == NETDEV_CREATE_STACKED) {
                link->stacked_netdevs_created = false;
                r = link_queue_request(link, REQUEST_TYPE_STACKED_NETDEV, netdev, false,
                                       &link->create_stacked_netdev_messages,
                                       link_create_stacked_netdev_handler,
                                       NULL);
        } else {
                link->stacked_netdevs_after_configured_created = false;
                r = link_queue_request(link, REQUEST_TYPE_STACKED_NETDEV, netdev, false,
                                       &link->create_stacked_netdev_after_configured_messages,
                                       link_create_stacked_netdev_after_configured_handler,
                                       NULL);
        }
        if (r < 0)
                return log_link_error_errno(link, r, "Failed to request stacked netdev '%s': %m",
                                            netdev->ifname);

        log_link_debug(link, "Requested stacked netdev '%s'", netdev->ifname);
        return 0;
}

int netdev_load_one(Manager *manager, const char *filename) {
        _cleanup_(netdev_unrefp) NetDev *netdev_raw = NULL, *netdev = NULL;
        const char *dropin_dirname;
        int r;

        assert(manager);
        assert(filename);

        r = null_or_empty_path(filename);
        if (r == -ENOENT)
                return 0;
        if (r < 0)
                return r;
        if (r > 0) {
                log_debug("Skipping empty file: %s", filename);
                return 0;
        }

        netdev_raw = new(NetDev, 1);
        if (!netdev_raw)
                return log_oom();

        *netdev_raw = (NetDev) {
                .n_ref = 1,
                .kind = _NETDEV_KIND_INVALID,
                .state = _NETDEV_STATE_INVALID, /* an invalid state means done() of the implementation won't be called on destruction */
        };

        dropin_dirname = strjoina(basename(filename), ".d");
        r = config_parse_many(
                        STRV_MAKE_CONST(filename), NETWORK_DIRS, dropin_dirname,
                        NETDEV_COMMON_SECTIONS NETDEV_OTHER_SECTIONS,
                        config_item_perf_lookup, network_netdev_gperf_lookup,
                        CONFIG_PARSE_WARN,
                        netdev_raw,
                        NULL);
        if (r < 0)
                return r;

        /* skip out early if configuration does not match the environment */
        if (!condition_test_list(netdev_raw->conditions, environ, NULL, NULL, NULL)) {
                log_debug("%s: Conditions in the file do not match the system environment, skipping.", filename);
                return 0;
        }

        if (netdev_raw->kind == _NETDEV_KIND_INVALID) {
                log_warning("NetDev has no Kind= configured in %s. Ignoring", filename);
                return 0;
        }

        if (!netdev_raw->ifname) {
                log_warning("NetDev without Name= configured in %s. Ignoring", filename);
                return 0;
        }

        netdev = malloc0(NETDEV_VTABLE(netdev_raw)->object_size);
        if (!netdev)
                return log_oom();

        netdev->n_ref = 1;
        netdev->manager = manager;
        netdev->kind = netdev_raw->kind;
        netdev->state = NETDEV_STATE_LOADING; /* we initialize the state here for the first time,
                                                 so that done() will be called on destruction */

        if (NETDEV_VTABLE(netdev)->init)
                NETDEV_VTABLE(netdev)->init(netdev);

        r = config_parse_many(
                        STRV_MAKE_CONST(filename), NETWORK_DIRS, dropin_dirname,
                        NETDEV_VTABLE(netdev)->sections,
                        config_item_perf_lookup, network_netdev_gperf_lookup,
                        CONFIG_PARSE_WARN,
                        netdev, NULL);
        if (r < 0)
                return r;

        /* verify configuration */
        if (NETDEV_VTABLE(netdev)->config_verify) {
                r = NETDEV_VTABLE(netdev)->config_verify(netdev, filename);
                if (r < 0)
                        return 0;
        }

        netdev->filename = strdup(filename);
        if (!netdev->filename)
                return log_oom();

        r = hashmap_ensure_put(&netdev->manager->netdevs, &string_hash_ops, netdev->ifname, netdev);
        if (r == -ENOMEM)
                return log_oom();
        if (r == -EEXIST) {
                NetDev *n = hashmap_get(netdev->manager->netdevs, netdev->ifname);

                assert(n);
                if (!streq(netdev->filename, n->filename))
                        log_netdev_warning_errno(netdev, r,
                                                 "Device was already configured by file %s, ignoring %s.",
                                                 n->filename, netdev->filename);

                /* Clear ifname before netdev_free() is called. Otherwise, the NetDev object 'n' is
                 * removed from the hashmap 'manager->netdevs'. */
                netdev->ifname = mfree(netdev->ifname);
                return 0;
        }
        if (r < 0)
                return r;

        log_netdev_debug(netdev, "loaded %s", netdev_kind_to_string(netdev->kind));

        if (IN_SET(netdev_get_create_type(netdev), NETDEV_CREATE_MASTER, NETDEV_CREATE_INDEPENDENT)) {
                r = netdev_create(netdev, NULL, NULL);
                if (r < 0)
                        return r;
        }

        if (netdev_is_stacked_and_independent(netdev)) {
                r = netdev_create(netdev, NULL, NULL);
                if (r < 0)
                        return r;
        }

        netdev = NULL;

        return 0;
}

int netdev_load(Manager *manager, bool reload) {
        _cleanup_strv_free_ char **files = NULL;
        char **f;
        int r;

        assert(manager);

        if (!reload)
                hashmap_clear_with_destructor(manager->netdevs, netdev_unref);

        r = conf_files_list_strv(&files, ".netdev", NULL, 0, NETWORK_DIRS);
        if (r < 0)
                return log_error_errno(r, "Failed to enumerate netdev files: %m");

        STRV_FOREACH(f, files) {
                r = netdev_load_one(manager, *f);
                if (r < 0)
                        log_error_errno(r, "Failed to load %s, ignoring: %m", *f);
        }

        return 0;
}

int config_parse_netdev_kind(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        NetDevKind k, *kind = data;

        assert(filename);
        assert(rvalue);
        assert(data);

        k = netdev_kind_from_string(rvalue);
        if (k < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, k, "Failed to parse netdev kind, ignoring assignment: %s", rvalue);
                return 0;
        }

        if (*kind != _NETDEV_KIND_INVALID && *kind != k) {
                log_syntax(unit, LOG_WARNING, filename, line, 0,
                           "Specified netdev kind is different from the previous value '%s', ignoring assignment: %s",
                           netdev_kind_to_string(*kind), rvalue);
                return 0;
        }

        *kind = k;

        return 0;
}

int config_parse_netdev_hw_addr(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        struct hw_addr_data *hw_addr = data;

        assert(rvalue);
        assert(data);

        if (streq(rvalue, "none")) {
                *hw_addr = HW_ADDR_NONE;
                return 0;
        }

        return config_parse_hw_addr(unit, filename, line, section, section_line, lvalue, ltype, rvalue, data, userdata);
}
