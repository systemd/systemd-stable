/* SPDX-License-Identifier: LGPL-2.1+ */

#include <sys/socket.h>
#include <linux/if.h>
#include <linux/fib_rules.h>
#include <stdio_ext.h>

#include "sd-daemon.h"
#include "sd-netlink.h"

#include "alloc-util.h"
#include "bus-util.h"
#include "conf-parser.h"
#include "def.h"
#include "dns-domain.h"
#include "fd-util.h"
#include "fileio.h"
#include "libudev-private.h"
#include "local-addresses.h"
#include "netlink-util.h"
#include "networkd-manager.h"
#include "ordered-set.h"
#include "path-util.h"
#include "set.h"
#include "udev-util.h"
#include "virt.h"

/* use 8 MB for receive socket kernel queue. */
#define RCVBUF_SIZE    (8*1024*1024)

const char* const network_dirs[] = {
        "/etc/systemd/network",
        "/run/systemd/network",
        "/usr/lib/systemd/network",
#if HAVE_SPLIT_USR
        "/lib/systemd/network",
#endif
        NULL};

static int setup_default_address_pool(Manager *m) {
        AddressPool *p;
        int r;

        assert(m);

        /* Add in the well-known private address ranges. */

        r = address_pool_new_from_string(m, &p, AF_INET6, "fc00::", 7);
        if (r < 0)
                return r;

        r = address_pool_new_from_string(m, &p, AF_INET, "192.168.0.0", 16);
        if (r < 0)
                return r;

        r = address_pool_new_from_string(m, &p, AF_INET, "172.16.0.0", 12);
        if (r < 0)
                return r;

        r = address_pool_new_from_string(m, &p, AF_INET, "10.0.0.0", 8);
        if (r < 0)
                return r;

        return 0;
}

static int manager_reset_all(Manager *m) {
        Link *link;
        Iterator i;
        int r;

        assert(m);

        HASHMAP_FOREACH(link, m->links, i) {
                r = link_carrier_reset(link);
                if (r < 0)
                        log_link_warning_errno(link, r, "Could not reset carrier: %m");
        }

        return 0;
}

static int match_prepare_for_sleep(sd_bus_message *message, void *userdata, sd_bus_error *ret_error) {
        Manager *m = userdata;
        int b, r;

        assert(message);
        assert(m);

        r = sd_bus_message_read(message, "b", &b);
        if (r < 0) {
                log_debug_errno(r, "Failed to parse PrepareForSleep signal: %m");
                return 0;
        }

        if (b)
                return 0;

        log_debug("Coming back from suspend, resetting all connections...");

        (void) manager_reset_all(m);

        return 0;
}

static int on_connected(sd_bus_message *message, void *userdata, sd_bus_error *ret_error) {
        Manager *m = userdata;

        assert(message);
        assert(m);

        /* Did we get a timezone or transient hostname from DHCP while D-Bus wasn't up yet? */
        if (m->dynamic_hostname)
                (void) manager_set_hostname(m, m->dynamic_hostname);
        if (m->dynamic_timezone)
                (void) manager_set_timezone(m, m->dynamic_timezone);

        return 0;
}

int manager_connect_bus(Manager *m) {
        int r;

        assert(m);

        if (m->bus)
                return 0;

        r = bus_open_system_watch_bind_with_description(&m->bus, "bus-api-network");
        if (r < 0)
                return log_error_errno(r, "Failed to connect to bus: %m");

        r = sd_bus_add_object_vtable(m->bus, NULL, "/org/freedesktop/network1", "org.freedesktop.network1.Manager", manager_vtable, m);
        if (r < 0)
                return log_error_errno(r, "Failed to add manager object vtable: %m");

        r = sd_bus_add_fallback_vtable(m->bus, NULL, "/org/freedesktop/network1/link", "org.freedesktop.network1.Link", link_vtable, link_object_find, m);
        if (r < 0)
               return log_error_errno(r, "Failed to add link object vtable: %m");

        r = sd_bus_add_node_enumerator(m->bus, NULL, "/org/freedesktop/network1/link", link_node_enumerator, m);
        if (r < 0)
                return log_error_errno(r, "Failed to add link enumerator: %m");

        r = sd_bus_add_fallback_vtable(m->bus, NULL, "/org/freedesktop/network1/network", "org.freedesktop.network1.Network", network_vtable, network_object_find, m);
        if (r < 0)
               return log_error_errno(r, "Failed to add network object vtable: %m");

        r = sd_bus_add_node_enumerator(m->bus, NULL, "/org/freedesktop/network1/network", network_node_enumerator, m);
        if (r < 0)
                return log_error_errno(r, "Failed to add network enumerator: %m");

        r = sd_bus_request_name_async(m->bus, NULL, "org.freedesktop.network1", 0, NULL, NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to request name: %m");

        r = sd_bus_attach_event(m->bus, m->event, 0);
        if (r < 0)
                return log_error_errno(r, "Failed to attach bus to event loop: %m");

        r = sd_bus_match_signal_async(
                        m->bus,
                        &m->connected_slot,
                        "org.freedesktop.DBus.Local",
                        NULL,
                        "org.freedesktop.DBus.Local",
                        "Connected",
                        on_connected, NULL, m);
        if (r < 0)
                return log_error_errno(r, "Failed to request match on Connected signal: %m");

        r = sd_bus_match_signal_async(
                        m->bus,
                        &m->prepare_for_sleep_slot,
                        "org.freedesktop.login1",
                        "/org/freedesktop/login1",
                        "org.freedesktop.login1.Manager",
                        "PrepareForSleep",
                        match_prepare_for_sleep, NULL, m);
        if (r < 0)
                log_warning_errno(r, "Failed to request match for PrepareForSleep, ignoring: %m");

        return 0;
}

static int manager_udev_process_link(Manager *m, struct udev_device *device) {
        Link *link = NULL;
        int r, ifindex;

        assert(m);
        assert(device);

        if (!streq_ptr(udev_device_get_action(device), "add"))
                return 0;

        ifindex = udev_device_get_ifindex(device);
        if (ifindex <= 0) {
                log_debug("Ignoring udev ADD event for device with invalid ifindex");
                return 0;
        }

        r = link_get(m, ifindex, &link);
        if (r == -ENODEV)
                return 0;
        else if (r < 0)
                return r;

        r = link_initialized(link, device);
        if (r < 0)
                return r;

        return 0;
}

static int manager_dispatch_link_udev(sd_event_source *source, int fd, uint32_t revents, void *userdata) {
        Manager *m = userdata;
        struct udev_monitor *monitor = m->udev_monitor;
        _cleanup_(udev_device_unrefp) struct udev_device *device = NULL;

        device = udev_monitor_receive_device(monitor);
        if (!device)
                return -ENOMEM;

        (void) manager_udev_process_link(m, device);

        return 0;
}

static int manager_connect_udev(Manager *m) {
        int r;

        /* udev does not initialize devices inside containers,
         * so we rely on them being already initialized before
         * entering the container */
        if (detect_container() > 0)
                return 0;

        m->udev = udev_new();
        if (!m->udev)
                return -ENOMEM;

        m->udev_monitor = udev_monitor_new_from_netlink(m->udev, "udev");
        if (!m->udev_monitor)
                return -ENOMEM;

        r = udev_monitor_filter_add_match_subsystem_devtype(m->udev_monitor, "net", NULL);
        if (r < 0)
                return log_error_errno(r, "Could not add udev monitor filter: %m");

        r = udev_monitor_enable_receiving(m->udev_monitor);
        if (r < 0) {
                log_error("Could not enable udev monitor");
                return r;
        }

        r = sd_event_add_io(m->event,
                        &m->udev_event_source,
                        udev_monitor_get_fd(m->udev_monitor),
                        EPOLLIN, manager_dispatch_link_udev,
                        m);
        if (r < 0)
                return r;

        r = sd_event_source_set_description(m->udev_event_source, "networkd-udev");
        if (r < 0)
                return r;

        return 0;
}

int manager_rtnl_process_route(sd_netlink *rtnl, sd_netlink_message *message, void *userdata) {
        Manager *m = userdata;
        Link *link = NULL;
        uint16_t type;
        uint32_t ifindex, priority = 0;
        unsigned char protocol, scope, tos, table, rt_type;
        int family;
        unsigned char dst_prefixlen, src_prefixlen;
        union in_addr_union dst = {}, gw = {}, src = {}, prefsrc = {};
        Route *route = NULL;
        int r;

        assert(rtnl);
        assert(message);
        assert(m);

        if (sd_netlink_message_is_error(message)) {
                r = sd_netlink_message_get_errno(message);
                if (r < 0)
                        log_warning_errno(r, "rtnl: failed to receive route, ignoring: %m");

                return 0;
        }

        r = sd_netlink_message_get_type(message, &type);
        if (r < 0) {
                log_warning_errno(r, "rtnl: could not get message type, ignoring: %m");
                return 0;
        } else if (!IN_SET(type, RTM_NEWROUTE, RTM_DELROUTE)) {
                log_warning("rtnl: received unexpected message type when processing route, ignoring");
                return 0;
        }

        r = sd_netlink_message_read_u32(message, RTA_OIF, &ifindex);
        if (r == -ENODATA) {
                log_debug("rtnl: received route without ifindex, ignoring");
                return 0;
        } else if (r < 0) {
                log_warning_errno(r, "rtnl: could not get ifindex from route, ignoring: %m");
                return 0;
        } else if (ifindex <= 0) {
                log_warning("rtnl: received route message with invalid ifindex, ignoring: %d", ifindex);
                return 0;
        } else {
                r = link_get(m, ifindex, &link);
                if (r < 0 || !link) {
                        /* when enumerating we might be out of sync, but we will
                         * get the route again, so just ignore it */
                        if (!m->enumerating)
                                log_warning("rtnl: received route for nonexistent link (%d), ignoring", ifindex);
                        return 0;
                }
        }

        r = sd_rtnl_message_route_get_family(message, &family);
        if (r < 0 || !IN_SET(family, AF_INET, AF_INET6)) {
                log_link_warning(link, "rtnl: received address with invalid family, ignoring");
                return 0;
        }

        r = sd_rtnl_message_route_get_protocol(message, &protocol);
        if (r < 0) {
                log_warning_errno(r, "rtnl: could not get route protocol: %m");
                return 0;
        }

        switch (family) {
        case AF_INET:
                r = sd_netlink_message_read_in_addr(message, RTA_DST, &dst.in);
                if (r < 0 && r != -ENODATA) {
                        log_link_warning_errno(link, r, "rtnl: received route without valid destination, ignoring: %m");
                        return 0;
                }

                r = sd_netlink_message_read_in_addr(message, RTA_GATEWAY, &gw.in);
                if (r < 0 && r != -ENODATA) {
                        log_link_warning_errno(link, r, "rtnl: received route with invalid gateway, ignoring: %m");
                        return 0;
                }

                r = sd_netlink_message_read_in_addr(message, RTA_SRC, &src.in);
                if (r < 0 && r != -ENODATA) {
                        log_link_warning_errno(link, r, "rtnl: received route with invalid source, ignoring: %m");
                        return 0;
                }

                r = sd_netlink_message_read_in_addr(message, RTA_PREFSRC, &prefsrc.in);
                if (r < 0 && r != -ENODATA) {
                        log_link_warning_errno(link, r, "rtnl: received route with invalid preferred source, ignoring: %m");
                        return 0;
                }

                break;

        case AF_INET6:
                r = sd_netlink_message_read_in6_addr(message, RTA_DST, &dst.in6);
                if (r < 0 && r != -ENODATA) {
                        log_link_warning_errno(link, r, "rtnl: received route without valid destination, ignoring: %m");
                        return 0;
                }

                r = sd_netlink_message_read_in6_addr(message, RTA_GATEWAY, &gw.in6);
                if (r < 0 && r != -ENODATA) {
                        log_link_warning_errno(link, r, "rtnl: received route with invalid gateway, ignoring: %m");
                        return 0;
                }

                r = sd_netlink_message_read_in6_addr(message, RTA_SRC, &src.in6);
                if (r < 0 && r != -ENODATA) {
                        log_link_warning_errno(link, r, "rtnl: received route with invalid source, ignoring: %m");
                        return 0;
                }

                r = sd_netlink_message_read_in6_addr(message, RTA_PREFSRC, &prefsrc.in6);
                if (r < 0 && r != -ENODATA) {
                        log_link_warning_errno(link, r, "rtnl: received route with invalid preferred source, ignoring: %m");
                        return 0;
                }

                break;

        default:
                assert_not_reached("Received unsupported address family");
                return 0;
        }

        r = sd_rtnl_message_route_get_dst_prefixlen(message, &dst_prefixlen);
        if (r < 0) {
                log_link_warning_errno(link, r, "rtnl: received route with invalid destination prefixlen, ignoring: %m");
                return 0;
        }

        r = sd_rtnl_message_route_get_src_prefixlen(message, &src_prefixlen);
        if (r < 0) {
                log_link_warning_errno(link, r, "rtnl: received route with invalid source prefixlen, ignoring: %m");
                return 0;
        }

        r = sd_rtnl_message_route_get_scope(message, &scope);
        if (r < 0) {
                log_link_warning_errno(link, r, "rtnl: received route with invalid scope, ignoring: %m");
                return 0;
        }

        r = sd_rtnl_message_route_get_tos(message, &tos);
        if (r < 0) {
                log_link_warning_errno(link, r, "rtnl: received route with invalid tos, ignoring: %m");
                return 0;
        }

        r = sd_rtnl_message_route_get_type(message, &rt_type);
        if (r < 0) {
                log_link_warning_errno(link, r, "rtnl: received route with invalid type, ignoring: %m");
                return 0;
        }

        r = sd_rtnl_message_route_get_table(message, &table);
        if (r < 0) {
                log_link_warning_errno(link, r, "rtnl: received route with invalid table, ignoring: %m");
                return 0;
        }

        r = sd_netlink_message_read_u32(message, RTA_PRIORITY, &priority);
        if (r < 0 && r != -ENODATA) {
                log_link_warning_errno(link, r, "rtnl: received route with invalid priority, ignoring: %m");
                return 0;
        }

        (void) route_get(link, family, &dst, dst_prefixlen, tos, priority, table, &route);

        switch (type) {
        case RTM_NEWROUTE:
                if (!route) {
                        /* A route appeared that we did not request */
                        r = route_add_foreign(link, family, &dst, dst_prefixlen, tos, priority, table, &route);
                        if (r < 0) {
                                log_link_warning_errno(link, r, "Failed to add route, ignoring: %m");
                                return 0;
                        }
                }

                route_update(route, &src, src_prefixlen, &gw, &prefsrc, scope, protocol, rt_type);

                break;

        case RTM_DELROUTE:
                route_free(route);
                break;

        default:
                assert_not_reached("Received invalid RTNL message type");
        }

        return 1;
}

int manager_rtnl_process_address(sd_netlink *rtnl, sd_netlink_message *message, void *userdata) {
        Manager *m = userdata;
        Link *link = NULL;
        uint16_t type;
        unsigned char flags;
        int family;
        unsigned char prefixlen;
        unsigned char scope;
        union in_addr_union in_addr;
        struct ifa_cacheinfo cinfo;
        Address *address = NULL;
        char buf[INET6_ADDRSTRLEN], valid_buf[FORMAT_TIMESPAN_MAX];
        const char *valid_str = NULL;
        int r, ifindex;

        assert(rtnl);
        assert(message);
        assert(m);

        if (sd_netlink_message_is_error(message)) {
                r = sd_netlink_message_get_errno(message);
                if (r < 0)
                        log_warning_errno(r, "rtnl: failed to receive address, ignoring: %m");

                return 0;
        }

        r = sd_netlink_message_get_type(message, &type);
        if (r < 0) {
                log_warning_errno(r, "rtnl: could not get message type, ignoring: %m");
                return 0;
        } else if (!IN_SET(type, RTM_NEWADDR, RTM_DELADDR)) {
                log_warning("rtnl: received unexpected message type when processing address, ignoring");
                return 0;
        }

        r = sd_rtnl_message_addr_get_ifindex(message, &ifindex);
        if (r < 0) {
                log_warning_errno(r, "rtnl: could not get ifindex from address, ignoring: %m");
                return 0;
        } else if (ifindex <= 0) {
                log_warning("rtnl: received address message with invalid ifindex, ignoring: %d", ifindex);
                return 0;
        } else {
                r = link_get(m, ifindex, &link);
                if (r < 0 || !link) {
                        /* when enumerating we might be out of sync, but we will
                         * get the address again, so just ignore it */
                        if (!m->enumerating)
                                log_warning("rtnl: received address for nonexistent link (%d), ignoring", ifindex);
                        return 0;
                }
        }

        r = sd_rtnl_message_addr_get_family(message, &family);
        if (r < 0 || !IN_SET(family, AF_INET, AF_INET6)) {
                log_link_warning(link, "rtnl: received address with invalid family, ignoring");
                return 0;
        }

        r = sd_rtnl_message_addr_get_prefixlen(message, &prefixlen);
        if (r < 0) {
                log_link_warning_errno(link, r, "rtnl: received address with invalid prefixlen, ignoring: %m");
                return 0;
        }

        r = sd_rtnl_message_addr_get_scope(message, &scope);
        if (r < 0) {
                log_link_warning_errno(link, r, "rtnl: received address with invalid scope, ignoring: %m");
                return 0;
        }

        r = sd_rtnl_message_addr_get_flags(message, &flags);
        if (r < 0) {
                log_link_warning_errno(link, r, "rtnl: received address with invalid flags, ignoring: %m");
                return 0;
        }

        switch (family) {
        case AF_INET:
                r = sd_netlink_message_read_in_addr(message, IFA_LOCAL, &in_addr.in);
                if (r < 0) {
                        log_link_warning_errno(link, r, "rtnl: received address without valid address, ignoring: %m");
                        return 0;
                }

                break;

        case AF_INET6:
                r = sd_netlink_message_read_in6_addr(message, IFA_ADDRESS, &in_addr.in6);
                if (r < 0) {
                        log_link_warning_errno(link, r, "rtnl: received address without valid address, ignoring: %m");
                        return 0;
                }

                break;

        default:
                assert_not_reached("Received unsupported address family");
        }

        if (!inet_ntop(family, &in_addr, buf, INET6_ADDRSTRLEN)) {
                log_link_warning(link, "Could not print address, ignoring");
                return 0;
        }

        r = sd_netlink_message_read_cache_info(message, IFA_CACHEINFO, &cinfo);
        if (r < 0 && r != -ENODATA) {
                log_link_warning_errno(link, r, "rtnl: cannot get IFA_CACHEINFO attribute, ignoring: %m");
                return 0;
        } else if (r >= 0) {
                if (cinfo.ifa_valid != CACHE_INFO_INFINITY_LIFE_TIME)
                        valid_str = format_timespan(valid_buf, FORMAT_TIMESPAN_MAX,
                                                    cinfo.ifa_valid * USEC_PER_SEC,
                                                    USEC_PER_SEC);
        }

        (void) address_get(link, family, &in_addr, prefixlen, &address);

        switch (type) {
        case RTM_NEWADDR:
                if (address)
                        log_link_debug(link, "Updating address: %s/%u (valid %s%s)", buf, prefixlen,
                                       valid_str ? "for " : "forever", strempty(valid_str));
                else {
                        /* An address appeared that we did not request */
                        r = address_add_foreign(link, family, &in_addr, prefixlen, &address);
                        if (r < 0) {
                                log_link_warning_errno(link, r, "Failed to add address %s/%u, ignoring: %m", buf, prefixlen);
                                return 0;
                        } else
                                log_link_debug(link, "Adding address: %s/%u (valid %s%s)", buf, prefixlen,
                                               valid_str ? "for " : "forever", strempty(valid_str));
                }

                r = address_update(address, flags, scope, &cinfo);
                if (r < 0) {
                        log_link_warning_errno(link, r, "Failed to update address %s/%u, ignoring: %m", buf, prefixlen);
                        return 0;
                }

                break;

        case RTM_DELADDR:

                if (address) {
                        log_link_debug(link, "Removing address: %s/%u (valid %s%s)", buf, prefixlen,
                                       valid_str ? "for " : "forever", strempty(valid_str));
                        (void) address_drop(address);
                } else
                        log_link_warning(link, "Removing non-existent address: %s/%u (valid %s%s), ignoring", buf, prefixlen,
                                         valid_str ? "for " : "forever", strempty(valid_str));

                break;
        default:
                assert_not_reached("Received invalid RTNL message type");
        }

        return 1;
}

static int manager_rtnl_process_link(sd_netlink *rtnl, sd_netlink_message *message, void *userdata) {
        Manager *m = userdata;
        Link *link = NULL;
        NetDev *netdev = NULL;
        uint16_t type;
        const char *name;
        int r, ifindex;

        assert(rtnl);
        assert(message);
        assert(m);

        if (sd_netlink_message_is_error(message)) {
                r = sd_netlink_message_get_errno(message);
                if (r < 0)
                        log_warning_errno(r, "rtnl: Could not receive link, ignoring: %m");

                return 0;
        }

        r = sd_netlink_message_get_type(message, &type);
        if (r < 0) {
                log_warning_errno(r, "rtnl: Could not get message type, ignoring: %m");
                return 0;
        } else if (!IN_SET(type, RTM_NEWLINK, RTM_DELLINK)) {
                log_warning("rtnl: Received unexpected message type when processing link, ignoring");
                return 0;
        }

        r = sd_rtnl_message_link_get_ifindex(message, &ifindex);
        if (r < 0) {
                log_warning_errno(r, "rtnl: Could not get ifindex from link, ignoring: %m");
                return 0;
        } else if (ifindex <= 0) {
                log_warning("rtnl: received link message with invalid ifindex %d, ignoring", ifindex);
                return 0;
        }

        r = sd_netlink_message_read_string(message, IFLA_IFNAME, &name);
        if (r < 0) {
                log_warning_errno(r, "rtnl: Received link message without ifname, ignoring: %m");
                return 0;
        }

        (void) link_get(m, ifindex, &link);
        (void) netdev_get(m, name, &netdev);

        switch (type) {
        case RTM_NEWLINK:
                if (!link) {
                        /* link is new, so add it */
                        r = link_add(m, message, &link);
                        if (r < 0) {
                                log_warning_errno(r, "Could not add new link, ignoring: %m");
                                return 0;
                        }
                }

                if (netdev) {
                        /* netdev exists, so make sure the ifindex matches */
                        r = netdev_set_ifindex(netdev, message);
                        if (r < 0) {
                                log_warning_errno(r, "Could not set ifindex on netdev, ignoring: %m");
                                return 0;
                        }
                }

                r = link_update(link, message);
                if (r < 0) {
                        log_warning_errno(r, "Could not update link, ignoring: %m");
                        return 0;
                }

                break;

        case RTM_DELLINK:
                link_drop(link);
                netdev_drop(netdev);

                break;

        default:
                assert_not_reached("Received invalid RTNL message type.");
        }

        return 1;
}

int manager_rtnl_process_rule(sd_netlink *rtnl, sd_netlink_message *message, void *userdata) {
        uint8_t tos = 0, to_prefixlen = 0, from_prefixlen = 0;
        union in_addr_union to = {}, from = {};
        RoutingPolicyRule *rule = NULL;
        uint32_t fwmark = 0, table = 0;
        char *iif = NULL, *oif = NULL;
        Manager *m = userdata;
        uint16_t type;
        int family;
        int r;

        assert(rtnl);
        assert(message);
        assert(m);

        if (sd_netlink_message_is_error(message)) {
                r = sd_netlink_message_get_errno(message);
                if (r < 0)
                        log_warning_errno(r, "rtnl: failed to receive rule, ignoring: %m");

                return 0;
        }

        r = sd_netlink_message_get_type(message, &type);
        if (r < 0) {
                log_warning_errno(r, "rtnl: could not get message type, ignoring: %m");
                return 0;
        } else if (!IN_SET(type, RTM_NEWRULE, RTM_DELRULE)) {
                log_warning("rtnl: received unexpected message type '%u' when processing rule, ignoring", type);
                return 0;
        }

        r = sd_rtnl_message_get_family(message, &family);
        if (r < 0) {
                log_warning_errno(r, "rtnl: could not get rule family, ignoring: %m");
                return 0;
        } else if (!IN_SET(family, AF_INET, AF_INET6)) {
                log_debug("rtnl: received address with invalid family %u, ignoring", family);
                return 0;
        }

        switch (family) {
        case AF_INET:
                r = sd_netlink_message_read_in_addr(message, FRA_SRC, &from.in);
                if (r < 0 && r != -ENODATA) {
                        log_warning_errno(r, "rtnl: could not get FRA_SRC attribute, ignoring: %m");
                        return 0;
                } else if (r >= 0) {
                        r = sd_rtnl_message_routing_policy_rule_get_rtm_src_prefixlen(message, &from_prefixlen);
                        if (r < 0) {
                                log_warning_errno(r, "rtnl: failed to retrieve rule from prefix length, ignoring: %m");
                                return 0;
                        }
                }

                r = sd_netlink_message_read_in_addr(message, FRA_DST, &to.in);
                if (r < 0 && r != -ENODATA) {
                        log_warning_errno(r, "rtnl: could not get FRA_DST attribute, ignoring: %m");
                        return 0;
                } else if (r >= 0) {
                        r = sd_rtnl_message_routing_policy_rule_get_rtm_dst_prefixlen(message, &to_prefixlen);
                        if (r < 0) {
                                log_warning_errno(r, "rtnl: failed to retrieve rule to prefix length, ignoring: %m");
                                return 0;
                        }
                }

                break;

        case AF_INET6:
                r = sd_netlink_message_read_in6_addr(message, FRA_SRC, &from.in6);
                if (r < 0 && r != -ENODATA) {
                        log_warning_errno(r, "rtnl: could not get FRA_SRC attribute, ignoring: %m");
                        return 0;
                } else if (r >= 0) {
                        r = sd_rtnl_message_routing_policy_rule_get_rtm_src_prefixlen(message, &from_prefixlen);
                        if (r < 0) {
                                log_warning_errno(r, "rtnl: failed to retrieve rule from prefix length, ignoring: %m");
                                return 0;
                        }
                }

                r = sd_netlink_message_read_in6_addr(message, FRA_DST, &to.in6);
                if (r < 0 && r != -ENODATA) {
                        log_warning_errno(r, "rtnl: could not get FRA_DST attribute, ignoring: %m");
                        return 0;
                } else if (r >= 0) {
                        r = sd_rtnl_message_routing_policy_rule_get_rtm_dst_prefixlen(message, &to_prefixlen);
                        if (r < 0) {
                                log_warning_errno(r, "rtnl: failed to retrieve rule to prefix length, ignoring: %m");
                                return 0;
                        }
                }

                break;

        default:
                assert_not_reached("Received unsupported address family");
        }

        if (from_prefixlen == 0 && to_prefixlen == 0)
                return 0;

        r = sd_netlink_message_read_u32(message, FRA_FWMARK, &fwmark);
        if (r < 0 && r != -ENODATA) {
                log_warning_errno(r, "rtnl: could not get FRA_FWMARK attribute, ignoring: %m");
                return 0;
        }

        r = sd_netlink_message_read_u32(message, FRA_TABLE, &table);
        if (r < 0 && r != -ENODATA) {
                log_warning_errno(r, "rtnl: could not get FRA_TABLE attribute, ignoring: %m");
                return 0;
        }

        r = sd_rtnl_message_routing_policy_rule_get_tos(message, &tos);
        if (r < 0 && r != -ENODATA) {
                log_warning_errno(r, "rtnl: could not get ip rule TOS, ignoring: %m");
                return 0;
        }

        r = sd_netlink_message_read_string(message, FRA_IIFNAME, (const char **) &iif);
        if (r < 0 && r != -ENODATA) {
                log_warning_errno(r, "rtnl: could not get FRA_IIFNAME attribute, ignoring: %m");
                return 0;
        }

        r = sd_netlink_message_read_string(message, FRA_OIFNAME, (const char **) &oif);
        if (r < 0 && r != -ENODATA) {
                log_warning_errno(r, "rtnl: could not get FRA_OIFNAME attribute, ignoring: %m");
                return 0;
        }

        (void) routing_policy_rule_get(m, family, &from, from_prefixlen, &to, to_prefixlen, tos, fwmark, table, iif, oif, &rule);

        switch (type) {
        case RTM_NEWRULE:
                if (!rule) {
                        r = routing_policy_rule_add_foreign(m, family, &from, from_prefixlen, &to, to_prefixlen, tos, fwmark, table, iif, oif, &rule);
                        if (r < 0) {
                                log_warning_errno(r, "Could not add rule, ignoring: %m");
                                return 0;
                        }
                }
                break;
        case RTM_DELRULE:
                routing_policy_rule_free(rule);

                break;

        default:
                assert_not_reached("Received invalid RTNL message type");
        }

        return 1;
}

static int systemd_netlink_fd(void) {
        int n, fd, rtnl_fd = -EINVAL;

        n = sd_listen_fds(true);
        if (n <= 0)
                return -EINVAL;

        for (fd = SD_LISTEN_FDS_START; fd < SD_LISTEN_FDS_START + n; fd ++) {
                if (sd_is_socket(fd, AF_NETLINK, SOCK_RAW, -1) > 0) {
                        if (rtnl_fd >= 0)
                                return -EINVAL;

                        rtnl_fd = fd;
                }
        }

        return rtnl_fd;
}

static int manager_connect_genl(Manager *m) {
        int r;

        assert(m);

        r = sd_genl_socket_open(&m->genl);
        if (r < 0)
                return r;

        r = sd_netlink_inc_rcvbuf(m->genl, RCVBUF_SIZE);
        if (r < 0)
                return r;

        r = sd_netlink_attach_event(m->genl, m->event, 0);
        if (r < 0)
                return r;

        return 0;
}

static int manager_connect_rtnl(Manager *m) {
        int fd, r;

        assert(m);

        fd = systemd_netlink_fd();
        if (fd < 0)
                r = sd_netlink_open(&m->rtnl);
        else
                r = sd_netlink_open_fd(&m->rtnl, fd);
        if (r < 0)
                return r;

        r = sd_netlink_inc_rcvbuf(m->rtnl, RCVBUF_SIZE);
        if (r < 0)
                return r;

        r = sd_netlink_attach_event(m->rtnl, m->event, 0);
        if (r < 0)
                return r;

        r = sd_netlink_add_match(m->rtnl, RTM_NEWLINK, &manager_rtnl_process_link, m);
        if (r < 0)
                return r;

        r = sd_netlink_add_match(m->rtnl, RTM_DELLINK, &manager_rtnl_process_link, m);
        if (r < 0)
                return r;

        r = sd_netlink_add_match(m->rtnl, RTM_NEWADDR, &manager_rtnl_process_address, m);
        if (r < 0)
                return r;

        r = sd_netlink_add_match(m->rtnl, RTM_DELADDR, &manager_rtnl_process_address, m);
        if (r < 0)
                return r;

        r = sd_netlink_add_match(m->rtnl, RTM_NEWROUTE, &manager_rtnl_process_route, m);
        if (r < 0)
                return r;

        r = sd_netlink_add_match(m->rtnl, RTM_DELROUTE, &manager_rtnl_process_route, m);
        if (r < 0)
                return r;

        r = sd_netlink_add_match(m->rtnl, RTM_NEWRULE, &manager_rtnl_process_rule, m);
        if (r < 0)
                return r;

        r = sd_netlink_add_match(m->rtnl, RTM_DELRULE, &manager_rtnl_process_rule, m);
        if (r < 0)
                return r;

        return 0;
}

static int ordered_set_put_in_addr_data(OrderedSet *s, const struct in_addr_data *address) {
        char *p;
        int r;

        assert(s);
        assert(address);

        r = in_addr_to_string(address->family, &address->address, &p);
        if (r < 0)
                return r;

        r = ordered_set_consume(s, p);
        if (r == -EEXIST)
                return 0;

        return r;
}

static int ordered_set_put_in_addr_datav(OrderedSet *s, const struct in_addr_data *addresses, unsigned n) {
        int r, c = 0;
        unsigned i;

        assert(s);
        assert(addresses || n == 0);

        for (i = 0; i < n; i++) {
                r = ordered_set_put_in_addr_data(s, addresses+i);
                if (r < 0)
                        return r;

                c += r;
        }

        return c;
}

static int ordered_set_put_in4_addr(OrderedSet *s, const struct in_addr *address) {
        char *p;
        int r;

        assert(s);
        assert(address);

        r = in_addr_to_string(AF_INET, (const union in_addr_union*) address, &p);
        if (r < 0)
                return r;

        r = ordered_set_consume(s, p);
        if (r == -EEXIST)
                return 0;

        return r;
}

static int ordered_set_put_in4_addrv(OrderedSet *s, const struct in_addr *addresses, unsigned n) {
        int r, c = 0;
        unsigned i;

        assert(s);
        assert(n == 0 || addresses);

        for (i = 0; i < n; i++) {
                r = ordered_set_put_in4_addr(s, addresses+i);
                if (r < 0)
                        return r;

                c += r;
        }

        return c;
}

static void print_string_set(FILE *f, const char *field, OrderedSet *s) {
        bool space = false;
        Iterator i;
        char *p;

        if (ordered_set_isempty(s))
                return;

        fputs(field, f);

        ORDERED_SET_FOREACH(p, s, i)
                fputs_with_space(f, p, NULL, &space);

        fputc('\n', f);
}

static int manager_save(Manager *m) {
        _cleanup_ordered_set_free_free_ OrderedSet *dns = NULL, *ntp = NULL, *search_domains = NULL, *route_domains = NULL;
        Link *link;
        Iterator i;
        _cleanup_free_ char *temp_path = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        LinkOperationalState operstate = LINK_OPERSTATE_OFF;
        const char *operstate_str;
        int r;

        assert(m);
        assert(m->state_file);

        /* We add all NTP and DNS server to a set, to filter out duplicates */
        dns = ordered_set_new(&string_hash_ops);
        if (!dns)
                return -ENOMEM;

        ntp = ordered_set_new(&string_hash_ops);
        if (!ntp)
                return -ENOMEM;

        search_domains = ordered_set_new(&dns_name_hash_ops);
        if (!search_domains)
                return -ENOMEM;

        route_domains = ordered_set_new(&dns_name_hash_ops);
        if (!route_domains)
                return -ENOMEM;

        HASHMAP_FOREACH(link, m->links, i) {
                if (link->flags & IFF_LOOPBACK)
                        continue;

                if (link->operstate > operstate)
                        operstate = link->operstate;

                if (!link->network)
                        continue;

                /* First add the static configured entries */
                r = ordered_set_put_in_addr_datav(dns, link->network->dns, link->network->n_dns);
                if (r < 0)
                        return r;

                r = ordered_set_put_strdupv(ntp, link->network->ntp);
                if (r < 0)
                        return r;

                r = ordered_set_put_strdupv(search_domains, link->network->search_domains);
                if (r < 0)
                        return r;

                r = ordered_set_put_strdupv(route_domains, link->network->route_domains);
                if (r < 0)
                        return r;

                if (!link->dhcp_lease)
                        continue;

                /* Secondly, add the entries acquired via DHCP */
                if (link->network->dhcp_use_dns) {
                        const struct in_addr *addresses;

                        r = sd_dhcp_lease_get_dns(link->dhcp_lease, &addresses);
                        if (r > 0) {
                                r = ordered_set_put_in4_addrv(dns, addresses, r);
                                if (r < 0)
                                        return r;
                        } else if (r < 0 && r != -ENODATA)
                                return r;
                }

                if (link->network->dhcp_use_ntp) {
                        const struct in_addr *addresses;

                        r = sd_dhcp_lease_get_ntp(link->dhcp_lease, &addresses);
                        if (r > 0) {
                                r = ordered_set_put_in4_addrv(ntp, addresses, r);
                                if (r < 0)
                                        return r;
                        } else if (r < 0 && r != -ENODATA)
                                return r;
                }

                if (link->network->dhcp_use_domains != DHCP_USE_DOMAINS_NO) {
                        const char *domainname;
                        char **domains = NULL;

                        OrderedSet *target_domains = (link->network->dhcp_use_domains == DHCP_USE_DOMAINS_YES) ? search_domains : route_domains;
                        r = sd_dhcp_lease_get_domainname(link->dhcp_lease, &domainname);
                        if (r >= 0) {
                                r = ordered_set_put_strdup(target_domains, domainname);
                                if (r < 0)
                                        return r;
                        } else if (r != -ENODATA)
                                return r;

                        r = sd_dhcp_lease_get_search_domains(link->dhcp_lease, &domains);
                        if (r >= 0) {
                                r = ordered_set_put_strdupv(target_domains, domains);
                                if (r < 0)
                                        return r;
                        } else if (r != -ENODATA)
                                return r;
                }
        }

        operstate_str = link_operstate_to_string(operstate);
        assert(operstate_str);

        r = fopen_temporary(m->state_file, &f, &temp_path);
        if (r < 0)
                return r;

        (void) __fsetlocking(f, FSETLOCKING_BYCALLER);
        (void) fchmod(fileno(f), 0644);

        fprintf(f,
                "# This is private data. Do not parse.\n"
                "OPER_STATE=%s\n", operstate_str);

        print_string_set(f, "DNS=", dns);
        print_string_set(f, "NTP=", ntp);
        print_string_set(f, "DOMAINS=", search_domains);
        print_string_set(f, "ROUTE_DOMAINS=", route_domains);

        r = routing_policy_serialize_rules(m->rules, f);
        if (r < 0)
                goto fail;

        r = fflush_and_check(f);
        if (r < 0)
                goto fail;

        if (rename(temp_path, m->state_file) < 0) {
                r = -errno;
                goto fail;
        }

        if (m->operational_state != operstate) {
                m->operational_state = operstate;
                r = manager_send_changed(m, "OperationalState", NULL);
                if (r < 0)
                        log_error_errno(r, "Could not emit changed OperationalState: %m");
        }

        m->dirty = false;

        return 0;

fail:
        (void) unlink(m->state_file);
        (void) unlink(temp_path);

        return log_error_errno(r, "Failed to save network state to %s: %m", m->state_file);
}

static int manager_dirty_handler(sd_event_source *s, void *userdata) {
        Manager *m = userdata;
        Link *link;
        Iterator i;
        int r;

        assert(m);

        if (m->dirty)
                manager_save(m);

        SET_FOREACH(link, m->dirty_links, i) {
                r = link_save(link);
                if (r >= 0)
                        link_clean(link);
        }

        return 1;
}

Link *manager_dhcp6_prefix_get(Manager *m, struct in6_addr *addr) {
        assert_return(m, NULL);
        assert_return(m->dhcp6_prefixes, NULL);
        assert_return(addr, NULL);

        return hashmap_get(m->dhcp6_prefixes, addr);
}

static int dhcp6_route_add_callback(sd_netlink *nl, sd_netlink_message *m,
                                       void *userdata) {
        Link *l = userdata;
        int r;
        union in_addr_union prefix;
        _cleanup_free_ char *buf = NULL;

        r = sd_netlink_message_get_errno(m);
        if (r != 0) {
                log_link_debug_errno(l, r, "Received error adding DHCPv6 Prefix Delegation route: %m");
                return 0;
        }

        r = sd_netlink_message_read_in6_addr(m, RTA_DST, &prefix.in6);
        if (r < 0) {
                log_link_debug_errno(l, r, "Could not read IPv6 address from DHCPv6 Prefix Delegation while adding route: %m");
                return 0;
        }

        (void) in_addr_to_string(AF_INET6, &prefix, &buf);
        log_link_debug(l, "Added DHCPv6 Prefix Deleagtion route %s/64",
                       strnull(buf));

        return 0;
}

int manager_dhcp6_prefix_add(Manager *m, struct in6_addr *addr, Link *link) {
        int r;
        Route *route;

        assert_return(m, -EINVAL);
        assert_return(m->dhcp6_prefixes, -ENODATA);
        assert_return(addr, -EINVAL);

        r = route_add(link, AF_INET6, (union in_addr_union *) addr, 64,
                      0, 0, 0, &route);
        if (r < 0)
                return r;

        r = route_configure(route, link, dhcp6_route_add_callback);
        if (r < 0)
                return r;

        return hashmap_put(m->dhcp6_prefixes, addr, link);
}

static int dhcp6_route_remove_callback(sd_netlink *nl, sd_netlink_message *m,
                                       void *userdata) {
        Link *l = userdata;
        int r;
        union in_addr_union prefix;
        _cleanup_free_ char *buf = NULL;

        r = sd_netlink_message_get_errno(m);
        if (r != 0) {
                log_link_debug_errno(l, r, "Received error on DHCPv6 Prefix Delegation route removal: %m");
                return 0;
        }

        r = sd_netlink_message_read_in6_addr(m, RTA_DST, &prefix.in6);
        if (r < 0) {
                log_link_debug_errno(l, r, "Could not read IPv6 address from DHCPv6 Prefix Delegation while removing route: %m");
                return 0;
        }

        (void) in_addr_to_string(AF_INET6, &prefix, &buf);
        log_link_debug(l, "Removed DHCPv6 Prefix Delegation route %s/64",
                       strnull(buf));

        return 0;
}

int manager_dhcp6_prefix_remove(Manager *m, struct in6_addr *addr) {
        Link *l;
        int r;
        Route *route;

        assert_return(m, -EINVAL);
        assert_return(m->dhcp6_prefixes, -ENODATA);
        assert_return(addr, -EINVAL);

        l = hashmap_remove(m->dhcp6_prefixes, addr);
        if (!l)
                return -EINVAL;

        (void) sd_radv_remove_prefix(l->radv, addr, 64);
        r = route_get(l, AF_INET6, (union in_addr_union *) addr, 64,
                      0, 0, 0, &route);
        if (r >= 0)
                (void) route_remove(route, l, dhcp6_route_remove_callback);

        return 0;
}

int manager_dhcp6_prefix_remove_all(Manager *m, Link *link) {
        Iterator i;
        Link *l;
        struct in6_addr *addr;

        assert_return(m, -EINVAL);
        assert_return(link, -EINVAL);

        HASHMAP_FOREACH_KEY(l, addr, m->dhcp6_prefixes, i) {
                if (l != link)
                        continue;

                (void) manager_dhcp6_prefix_remove(m, addr);
        }

        return 0;
}

static void dhcp6_prefixes_hash_func(const void *p, struct siphash *state) {
        const struct in6_addr *addr = p;

        assert(p);

        siphash24_compress(addr, sizeof(*addr), state);
}

static int dhcp6_prefixes_compare_func(const void *_a, const void *_b) {
        const struct in6_addr *a = _a, *b = _b;

        return memcmp(a, b, sizeof(*a));
}

static const struct hash_ops dhcp6_prefixes_hash_ops = {
        .hash = dhcp6_prefixes_hash_func,
        .compare = dhcp6_prefixes_compare_func,
};

int manager_new(Manager **ret, sd_event *event) {
        _cleanup_(manager_freep) Manager *m = NULL;
        int r;

        m = new0(Manager, 1);
        if (!m)
                return -ENOMEM;

        m->state_file = strdup("/run/systemd/netif/state");
        if (!m->state_file)
                return -ENOMEM;

        m->event = sd_event_ref(event);

        r = sd_event_add_post(m->event, NULL, manager_dirty_handler, m);
        if (r < 0)
                return r;

        r = manager_connect_rtnl(m);
        if (r < 0)
                return r;

        r = manager_connect_genl(m);
        if (r < 0)
                return r;

        r = manager_connect_udev(m);
        if (r < 0)
                return r;

        m->netdevs = hashmap_new(&string_hash_ops);
        if (!m->netdevs)
                return -ENOMEM;

        LIST_HEAD_INIT(m->networks);

        r = sd_resolve_default(&m->resolve);
        if (r < 0)
                return r;

        r = sd_resolve_attach_event(m->resolve, m->event, 0);
        if (r < 0)
                return r;

        r = setup_default_address_pool(m);
        if (r < 0)
                return r;

        m->dhcp6_prefixes = hashmap_new(&dhcp6_prefixes_hash_ops);
        if (!m->dhcp6_prefixes)
                return -ENOMEM;

        m->duid.type = DUID_TYPE_EN;

        (void) routing_policy_load_rules(m->state_file, &m->rules_saved);

        *ret = TAKE_PTR(m);

        return 0;
}

void manager_free(Manager *m) {
        Network *network;
        NetDev *netdev;
        Link *link;
        AddressPool *pool;

        if (!m)
                return;

        free(m->state_file);

        while ((network = m->networks))
                network_free(network);

        while ((link = hashmap_first(m->dhcp6_prefixes)))
                link_unref(link);
        hashmap_free(m->dhcp6_prefixes);

        while ((link = hashmap_first(m->links)))
                link_unref(link);
        hashmap_free(m->links);

        hashmap_free(m->networks_by_name);

        while ((netdev = hashmap_first(m->netdevs)))
                netdev_unref(netdev);
        hashmap_free(m->netdevs);

        while ((pool = m->address_pools))
                address_pool_free(pool);

        /* routing_policy_rule_free() access m->rules and m->rules_foreign.
         * So, it is necessary to set NULL after the sets are freed. */
        m->rules = set_free_with_destructor(m->rules, routing_policy_rule_free);
        m->rules_foreign = set_free_with_destructor(m->rules_foreign, routing_policy_rule_free);
        set_free_with_destructor(m->rules_saved, routing_policy_rule_free);

        sd_netlink_unref(m->rtnl);
        sd_netlink_unref(m->genl);
        sd_event_unref(m->event);

        sd_resolve_unref(m->resolve);

        sd_event_source_unref(m->udev_event_source);
        udev_monitor_unref(m->udev_monitor);
        udev_unref(m->udev);

        sd_bus_unref(m->bus);
        sd_bus_slot_unref(m->prepare_for_sleep_slot);
        sd_bus_slot_unref(m->connected_slot);

        free(m->dynamic_timezone);
        free(m->dynamic_hostname);

        free(m);
}

int manager_start(Manager *m) {
        Link *link;
        Iterator i;

        assert(m);

        /* The dirty handler will deal with future serialization, but the first one
           must be done explicitly. */

        manager_save(m);

        HASHMAP_FOREACH(link, m->links, i)
                link_save(link);

        return 0;
}

int manager_load_config(Manager *m) {
        int r;

        /* update timestamp */
        paths_check_timestamp(network_dirs, &m->network_dirs_ts_usec, true);

        r = netdev_load(m);
        if (r < 0)
                return r;

        r = network_load(m);
        if (r < 0)
                return r;

        return 0;
}

bool manager_should_reload(Manager *m) {
        return paths_check_timestamp(network_dirs, &m->network_dirs_ts_usec, false);
}

int manager_rtnl_enumerate_links(Manager *m) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *req = NULL, *reply = NULL;
        sd_netlink_message *link;
        int r;

        assert(m);
        assert(m->rtnl);

        r = sd_rtnl_message_new_link(m->rtnl, &req, RTM_GETLINK, 0);
        if (r < 0)
                return r;

        r = sd_netlink_message_request_dump(req, true);
        if (r < 0)
                return r;

        r = sd_netlink_call(m->rtnl, req, 0, &reply);
        if (r < 0)
                return r;

        for (link = reply; link; link = sd_netlink_message_next(link)) {
                int k;

                m->enumerating = true;

                k = manager_rtnl_process_link(m->rtnl, link, m);
                if (k < 0)
                        r = k;

                m->enumerating = false;
        }

        return r;
}

int manager_rtnl_enumerate_addresses(Manager *m) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *req = NULL, *reply = NULL;
        sd_netlink_message *addr;
        int r;

        assert(m);
        assert(m->rtnl);

        r = sd_rtnl_message_new_addr(m->rtnl, &req, RTM_GETADDR, 0, 0);
        if (r < 0)
                return r;

        r = sd_netlink_message_request_dump(req, true);
        if (r < 0)
                return r;

        r = sd_netlink_call(m->rtnl, req, 0, &reply);
        if (r < 0)
                return r;

        for (addr = reply; addr; addr = sd_netlink_message_next(addr)) {
                int k;

                m->enumerating = true;

                k = manager_rtnl_process_address(m->rtnl, addr, m);
                if (k < 0)
                        r = k;

                m->enumerating = false;
        }

        return r;
}

int manager_rtnl_enumerate_routes(Manager *m) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *req = NULL, *reply = NULL;
        sd_netlink_message *route;
        int r;

        assert(m);
        assert(m->rtnl);

        r = sd_rtnl_message_new_route(m->rtnl, &req, RTM_GETROUTE, 0, 0);
        if (r < 0)
                return r;

        r = sd_netlink_message_request_dump(req, true);
        if (r < 0)
                return r;

        r = sd_netlink_call(m->rtnl, req, 0, &reply);
        if (r < 0)
                return r;

        for (route = reply; route; route = sd_netlink_message_next(route)) {
                int k;

                m->enumerating = true;

                k = manager_rtnl_process_route(m->rtnl, route, m);
                if (k < 0)
                        r = k;

                m->enumerating = false;
        }

        return r;
}

int manager_rtnl_enumerate_rules(Manager *m) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *req = NULL, *reply = NULL;
        sd_netlink_message *rule;
        int r;

        assert(m);
        assert(m->rtnl);

        r = sd_rtnl_message_new_routing_policy_rule(m->rtnl, &req, RTM_GETRULE, 0);
        if (r < 0)
                return r;

        r = sd_netlink_message_request_dump(req, true);
        if (r < 0)
                return r;

        r = sd_netlink_call(m->rtnl, req, 0, &reply);
        if (r < 0) {
                if (r == -EOPNOTSUPP) {
                        log_debug("FIB Rules are not supported by the kernel. Ignoring.");
                        return 0;
                }

                return r;
        }

        for (rule = reply; rule; rule = sd_netlink_message_next(rule)) {
                int k;

                m->enumerating = true;

                k = manager_rtnl_process_rule(m->rtnl, rule, m);
                if (k < 0)
                        r = k;

                m->enumerating = false;
        }

        return r;
}

int manager_address_pool_acquire(Manager *m, int family, unsigned prefixlen, union in_addr_union *found) {
        AddressPool *p;
        int r;

        assert(m);
        assert(prefixlen > 0);
        assert(found);

        LIST_FOREACH(address_pools, p, m->address_pools) {
                if (p->family != family)
                        continue;

                r = address_pool_acquire(p, prefixlen, found);
                if (r != 0)
                        return r;
        }

        return 0;
}

Link* manager_find_uplink(Manager *m, Link *exclude) {
        _cleanup_free_ struct local_address *gateways = NULL;
        int n, i;

        assert(m);

        /* Looks for a suitable "uplink", via black magic: an
         * interface that is up and where the default route with the
         * highest priority points to. */

        n = local_gateways(m->rtnl, 0, AF_UNSPEC, &gateways);
        if (n < 0) {
                log_warning_errno(n, "Failed to determine list of default gateways: %m");
                return NULL;
        }

        for (i = 0; i < n; i++) {
                Link *link;

                link = hashmap_get(m->links, INT_TO_PTR(gateways[i].ifindex));
                if (!link) {
                        log_debug("Weird, found a gateway for a link we don't know. Ignoring.");
                        continue;
                }

                if (link == exclude)
                        continue;

                if (link->operstate < LINK_OPERSTATE_ROUTABLE)
                        continue;

                return link;
        }

        return NULL;
}

void manager_dirty(Manager *manager) {
        assert(manager);

        /* the serialized state in /run is no longer up-to-date */
        manager->dirty = true;
}

static int set_hostname_handler(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
        Manager *manager = userdata;
        const sd_bus_error *e;

        assert(m);
        assert(manager);

        e = sd_bus_message_get_error(m);
        if (e)
                log_warning_errno(sd_bus_error_get_errno(e), "Could not set hostname: %s", e->message);

        return 1;
}

int manager_set_hostname(Manager *m, const char *hostname) {
        int r;

        log_debug("Setting transient hostname: '%s'", strna(hostname));

        if (free_and_strdup(&m->dynamic_hostname, hostname) < 0)
                return log_oom();

        if (!m->bus || sd_bus_is_ready(m->bus) <= 0) {
                log_debug("Not connected to system bus, setting hostname later.");
                return 0;
        }

        r = sd_bus_call_method_async(
                        m->bus,
                        NULL,
                        "org.freedesktop.hostname1",
                        "/org/freedesktop/hostname1",
                        "org.freedesktop.hostname1",
                        "SetHostname",
                        set_hostname_handler,
                        m,
                        "sb",
                        hostname,
                        false);

        if (r < 0)
                return log_error_errno(r, "Could not set transient hostname: %m");

        return 0;
}

static int set_timezone_handler(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
        Manager *manager = userdata;
        const sd_bus_error *e;

        assert(m);
        assert(manager);

        e = sd_bus_message_get_error(m);
        if (e)
                log_warning_errno(sd_bus_error_get_errno(e), "Could not set timezone: %s", e->message);

        return 1;
}

int manager_set_timezone(Manager *m, const char *tz) {
        int r;

        assert(m);
        assert(tz);

        log_debug("Setting system timezone: '%s'", tz);
        if (free_and_strdup(&m->dynamic_timezone, tz) < 0)
                return log_oom();

        if (!m->bus || sd_bus_is_ready(m->bus) <= 0) {
                log_debug("Not connected to system bus, setting timezone later.");
                return 0;
        }

        r = sd_bus_call_method_async(
                        m->bus,
                        NULL,
                        "org.freedesktop.timedate1",
                        "/org/freedesktop/timedate1",
                        "org.freedesktop.timedate1",
                        "SetTimezone",
                        set_timezone_handler,
                        m,
                        "sb",
                        tz,
                        false);
        if (r < 0)
                return log_error_errno(r, "Could not set timezone: %m");

        return 0;
}
