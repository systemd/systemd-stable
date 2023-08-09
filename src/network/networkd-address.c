/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <net/if.h>
#include <net/if_arp.h>

#include "alloc-util.h"
#include "firewall-util.h"
#include "memory-util.h"
#include "netlink-util.h"
#include "networkd-address-pool.h"
#include "networkd-address.h"
#include "networkd-dhcp-server.h"
#include "networkd-ipv4acd.h"
#include "networkd-manager.h"
#include "networkd-netlabel.h"
#include "networkd-network.h"
#include "networkd-queue.h"
#include "networkd-route-util.h"
#include "networkd-route.h"
#include "parse-util.h"
#include "string-util.h"
#include "strv.h"
#include "strxcpyx.h"

#define ADDRESSES_PER_LINK_MAX 2048U
#define STATIC_ADDRESSES_PER_NETWORK_MAX 1024U

int address_flags_to_string_alloc(uint32_t flags, int family, char **ret) {
        _cleanup_free_ char *str = NULL;
        static const char* map[] = {
                [LOG2U(IFA_F_SECONDARY)]      = "secondary", /* This is also called "temporary" for ipv6. */
                [LOG2U(IFA_F_NODAD)]          = "nodad",
                [LOG2U(IFA_F_OPTIMISTIC)]     = "optimistic",
                [LOG2U(IFA_F_DADFAILED)]      = "dadfailed",
                [LOG2U(IFA_F_HOMEADDRESS)]    = "home-address",
                [LOG2U(IFA_F_DEPRECATED)]     = "deprecated",
                [LOG2U(IFA_F_TENTATIVE)]      = "tentative",
                [LOG2U(IFA_F_PERMANENT)]      = "permanent",
                [LOG2U(IFA_F_MANAGETEMPADDR)] = "manage-temporary-address",
                [LOG2U(IFA_F_NOPREFIXROUTE)]  = "no-prefixroute",
                [LOG2U(IFA_F_MCAUTOJOIN)]     = "auto-join",
                [LOG2U(IFA_F_STABLE_PRIVACY)] = "stable-privacy",
        };

        assert(IN_SET(family, AF_INET, AF_INET6));
        assert(ret);

        for (size_t i = 0; i < ELEMENTSOF(map); i++)
                if (FLAGS_SET(flags, 1 << i) && map[i])
                        if (!strextend_with_separator(
                                            &str, ",",
                                            family == AF_INET6 && (1 << i) == IFA_F_SECONDARY ? "temporary" : map[i]))
                                return -ENOMEM;

        *ret = TAKE_PTR(str);
        return 0;
}

int address_new(Address **ret) {
        _cleanup_(address_freep) Address *address = NULL;

        address = new(Address, 1);
        if (!address)
                return -ENOMEM;

        *address = (Address) {
                .family = AF_UNSPEC,
                .scope = RT_SCOPE_UNIVERSE,
                .lifetime_valid_usec = USEC_INFINITY,
                .lifetime_preferred_usec = USEC_INFINITY,
                .set_broadcast = -1,
        };

        *ret = TAKE_PTR(address);

        return 0;
}

static int address_new_static(Network *network, const char *filename, unsigned section_line, Address **ret) {
        _cleanup_(config_section_freep) ConfigSection *n = NULL;
        _cleanup_(address_freep) Address *address = NULL;
        int r;

        assert(network);
        assert(ret);
        assert(filename);
        assert(section_line > 0);

        r = config_section_new(filename, section_line, &n);
        if (r < 0)
                return r;

        address = ordered_hashmap_get(network->addresses_by_section, n);
        if (address) {
                *ret = TAKE_PTR(address);
                return 0;
        }

        if (ordered_hashmap_size(network->addresses_by_section) >= STATIC_ADDRESSES_PER_NETWORK_MAX)
                return -E2BIG;

        r = address_new(&address);
        if (r < 0)
                return r;

        address->network = network;
        address->section = TAKE_PTR(n);
        address->source = NETWORK_CONFIG_SOURCE_STATIC;
        /* This will be adjusted in address_section_verify(). */
        address->duplicate_address_detection = _ADDRESS_FAMILY_INVALID;

        r = ordered_hashmap_ensure_put(&network->addresses_by_section, &config_section_hash_ops, address->section, address);
        if (r < 0)
                return r;

        *ret = TAKE_PTR(address);
        return 0;
}

Address *address_free(Address *address) {
        if (!address)
                return NULL;

        if (address->network) {
                assert(address->section);
                ordered_hashmap_remove(address->network->addresses_by_section, address->section);
        }

        if (address->link) {
                set_remove(address->link->addresses, address);

                if (address->family == AF_INET6 &&
                    in6_addr_equal(&address->in_addr.in6, &address->link->ipv6ll_address))
                        memzero(&address->link->ipv6ll_address, sizeof(struct in6_addr));
        }

        sd_ipv4acd_unref(address->acd);

        config_section_free(address->section);
        free(address->label);
        free(address->netlabel);
        return mfree(address);
}

static bool address_lifetime_is_valid(const Address *a) {
        assert(a);

        return
                a->lifetime_valid_usec == USEC_INFINITY ||
                a->lifetime_valid_usec > now(CLOCK_BOOTTIME);
}

bool address_is_ready(const Address *a) {
        assert(a);

        if (!ipv4acd_bound(a))
                return false;

        if (FLAGS_SET(a->flags, IFA_F_TENTATIVE))
                return false;

        if (FLAGS_SET(a->state, NETWORK_CONFIG_STATE_REMOVING))
                return false;

        if (!FLAGS_SET(a->state, NETWORK_CONFIG_STATE_CONFIGURED))
                return false;

        return address_lifetime_is_valid(a);
}

void link_mark_addresses(Link *link, NetworkConfigSource source) {
        Address *a;

        assert(link);

        SET_FOREACH(a, link->addresses) {
                if (a->source != source)
                        continue;

                address_mark(a);
        }
}

static bool address_needs_to_set_broadcast(const Address *a, Link *link) {
        assert(a);
        assert(link);

        if (a->family != AF_INET)
                return false;

        if (in4_addr_is_set(&a->in_addr_peer.in))
                return false;

        /* A /31 or /32 IPv4 address does not have a broadcast address.
         * See https://tools.ietf.org/html/rfc3021 */
        if (a->prefixlen > 30)
                return false;

        /* If explicitly configured, do not update the address. */
        if (in4_addr_is_set(&a->broadcast))
                return false;

        if (a->set_broadcast >= 0)
                return a->set_broadcast;

        /* Defaults to true, except for wireguard, as typical configuration for wireguard does not set
         * broadcast. */
        return !streq_ptr(link->kind, "wireguard");
}

void address_set_broadcast(Address *a, Link *link) {
        assert(a);
        assert(link);

        if (!address_needs_to_set_broadcast(a, link))
                return;

        a->broadcast.s_addr = a->in_addr.in.s_addr | htobe32(UINT32_C(0xffffffff) >> a->prefixlen);
}

static void address_set_cinfo(Manager *m, const Address *a, struct ifa_cacheinfo *cinfo) {
        usec_t now_usec;

        assert(m);
        assert(a);
        assert(cinfo);

        assert_se(sd_event_now(m->event, CLOCK_BOOTTIME, &now_usec) >= 0);

        *cinfo = (struct ifa_cacheinfo) {
                .ifa_valid = usec_to_sec(a->lifetime_valid_usec, now_usec),
                .ifa_prefered = usec_to_sec(a->lifetime_preferred_usec, now_usec),
        };
}

static void address_set_lifetime(Manager *m, Address *a, const struct ifa_cacheinfo *cinfo) {
        usec_t now_usec;

        assert(m);
        assert(a);
        assert(cinfo);

        assert_se(sd_event_now(m->event, CLOCK_BOOTTIME, &now_usec) >= 0);

        a->lifetime_valid_usec = sec_to_usec(cinfo->ifa_valid, now_usec);
        a->lifetime_preferred_usec = sec_to_usec(cinfo->ifa_prefered, now_usec);
}

static uint32_t address_prefix(const Address *a) {
        assert(a);

        /* make sure we don't try to shift by 32.
         * See ISO/IEC 9899:TC3 § 6.5.7.3. */
        if (a->prefixlen == 0)
                return 0;

        if (a->in_addr_peer.in.s_addr != 0)
                return be32toh(a->in_addr_peer.in.s_addr) >> (32 - a->prefixlen);
        else
                return be32toh(a->in_addr.in.s_addr) >> (32 - a->prefixlen);
}

static void address_kernel_hash_func(const Address *a, struct siphash *state) {
        assert(a);

        siphash24_compress(&a->family, sizeof(a->family), state);

        switch (a->family) {
        case AF_INET:
                siphash24_compress(&a->prefixlen, sizeof(a->prefixlen), state);

                uint32_t prefix = address_prefix(a);
                siphash24_compress(&prefix, sizeof(prefix), state);

                _fallthrough_;
        case AF_INET6:
                siphash24_compress(&a->in_addr, FAMILY_ADDRESS_SIZE(a->family), state);
                break;
        default:
                /* treat any other address family as AF_UNSPEC */
                break;
        }
}

static int address_kernel_compare_func(const Address *a1, const Address *a2) {
        int r;

        r = CMP(a1->family, a2->family);
        if (r != 0)
                return r;

        switch (a1->family) {
        case AF_INET:
                /* See kernel's find_matching_ifa() in net/ipv4/devinet.c */
                r = CMP(a1->prefixlen, a2->prefixlen);
                if (r != 0)
                        return r;

                r = CMP(address_prefix(a1), address_prefix(a2));
                if (r != 0)
                        return r;

                _fallthrough_;
        case AF_INET6:
                /* See kernel's ipv6_get_ifaddr() in net/ipv6/addrconf.c */
                return memcmp(&a1->in_addr, &a2->in_addr, FAMILY_ADDRESS_SIZE(a1->family));
        default:
                /* treat any other address family as AF_UNSPEC */
                return 0;
        }
}

DEFINE_PRIVATE_HASH_OPS(
        address_kernel_hash_ops,
        Address,
        address_kernel_hash_func,
        address_kernel_compare_func);

DEFINE_PRIVATE_HASH_OPS_WITH_KEY_DESTRUCTOR(
        address_kernel_hash_ops_free,
        Address,
        address_kernel_hash_func,
        address_kernel_compare_func,
        address_free);

/* The functions below are mainly used by managing Request. */
static void address_hash_func(const Address *a, struct siphash *state) {
        assert(a);

        siphash24_compress(&a->family, sizeof(a->family), state);

        /* treat any other address family as AF_UNSPEC */
        if (!IN_SET(a->family, AF_INET, AF_INET6))
                return;

        siphash24_compress(&a->prefixlen, sizeof(a->prefixlen), state);
        siphash24_compress(&a->in_addr, FAMILY_ADDRESS_SIZE(a->family), state);
        siphash24_compress(&a->in_addr_peer, FAMILY_ADDRESS_SIZE(a->family), state);

        if (a->family == AF_INET) {
                /* On update, the kernel ignores the address label and broadcast address, hence we need
                 * to distinguish addresses with different labels or broadcast addresses. Otherwise,
                 * the label or broadcast address change will not be applied when we reconfigure the
                 * interface. */
                siphash24_compress_string(a->label, state);
                siphash24_compress(&a->broadcast, sizeof(a->broadcast), state);
        }
}

int address_compare_func(const Address *a1, const Address *a2) {
        int r;

        r = CMP(a1->family, a2->family);
        if (r != 0)
                return r;

        if (!IN_SET(a1->family, AF_INET, AF_INET6))
                return 0;

        r = CMP(a1->prefixlen, a2->prefixlen);
        if (r != 0)
                return r;

        r = memcmp(&a1->in_addr, &a2->in_addr, FAMILY_ADDRESS_SIZE(a1->family));
        if (r != 0)
                return r;

        r = memcmp(&a1->in_addr_peer, &a2->in_addr_peer, FAMILY_ADDRESS_SIZE(a1->family));
        if (r != 0)
                return r;

        if (a1->family == AF_INET) {
                r = strcmp_ptr(a1->label, a2->label);
                if (r != 0)
                        return r;

                r = CMP(a1->broadcast.s_addr, a2->broadcast.s_addr);
                if (r != 0)
                        return r;
        }

        return 0;
}

int address_equal(const Address *a1, const Address *a2) {
        if (a1 == a2)
                return true;

        if (!a1 || !a2)
                return false;

        return address_compare_func(a1, a2) == 0;
}

static int address_equalify(Address *address, const Address *src) {
        int r;

        assert(address);
        assert(src);

        if (address_kernel_compare_func(address, src) != 0)
                return -EINVAL;

        if (address->family == AF_INET) {
                address->broadcast = src->broadcast;
                r = free_and_strdup(&address->label, src->label);
                if (r < 0)
                        return r;
        } else {
                address->prefixlen = src->prefixlen;
                address->in_addr_peer = src->in_addr_peer;
        }

        return 0;
}

int address_dup(const Address *src, Address **ret) {
        _cleanup_(address_freep) Address *dest = NULL;
        int r;

        assert(src);
        assert(ret);

        dest = newdup(Address, src, 1);
        if (!dest)
                return -ENOMEM;

        /* clear all pointers */
        dest->network = NULL;
        dest->section = NULL;
        dest->link = NULL;
        dest->label = NULL;
        dest->acd = NULL;
        dest->netlabel = NULL;

        if (src->family == AF_INET) {
                r = free_and_strdup(&dest->label, src->label);
                if (r < 0)
                        return r;
        }

        r = free_and_strdup(&dest->netlabel, src->netlabel);
        if (r < 0)
                return r;

        *ret = TAKE_PTR(dest);
        return 0;
}

static int address_set_masquerade(Address *address, bool add) {
        union in_addr_union masked;
        int r;

        assert(address);
        assert(address->link);

        if (!address->link->network)
                return 0;

        if (address->family == AF_INET &&
            !FLAGS_SET(address->link->network->ip_masquerade, ADDRESS_FAMILY_IPV4))
                return 0;

        if (address->family == AF_INET6 &&
            !FLAGS_SET(address->link->network->ip_masquerade, ADDRESS_FAMILY_IPV6))
                return 0;

        if (address->scope >= RT_SCOPE_LINK)
                return 0;

        if (address->ip_masquerade_done == add)
                return 0;

        masked = address->in_addr;
        r = in_addr_mask(address->family, &masked, address->prefixlen);
        if (r < 0)
                return r;

        r = fw_add_masquerade(&address->link->manager->fw_ctx, add, address->family, &masked, address->prefixlen);
        if (r < 0)
                return r;

        address->ip_masquerade_done = add;

        return 0;
}

static int address_add(Link *link, Address *address) {
        int r;

        assert(link);
        assert(address);

        r = set_ensure_put(&link->addresses, &address_kernel_hash_ops_free, address);
        if (r < 0)
                return r;
        if (r == 0)
                return -EEXIST;

        address->link = link;
        return 0;
}

static int address_update(Address *address) {
        Link *link;
        int r;

        assert(address);
        assert(address->link);

        link = address->link;

        if (address_is_ready(address) &&
            address->family == AF_INET6 &&
            in6_addr_is_link_local(&address->in_addr.in6) &&
            in6_addr_is_null(&link->ipv6ll_address)) {

                link->ipv6ll_address = address->in_addr.in6;

                r = link_ipv6ll_gained(link);
                if (r < 0)
                        return r;
        }

        if (IN_SET(link->state, LINK_STATE_FAILED, LINK_STATE_LINGER))
                return 0;

        r = address_set_masquerade(address, true);
        if (r < 0)
                return log_link_warning_errno(link, r, "Could not enable IP masquerading: %m");

        address_add_netlabel(address);

        if (address_is_ready(address) && address->callback) {
                r = address->callback(address);
                if (r < 0)
                        return r;
        }

        link_update_operstate(link, true);
        link_check_ready(link);
        return 0;
}

static int address_drop(Address *address) {
        Link *link;
        bool ready;
        int r;

        assert(address);
        assert(address->link);

        ready = address_is_ready(address);
        link = address->link;

        r = address_set_masquerade(address, false);
        if (r < 0)
                log_link_warning_errno(link, r, "Failed to disable IP masquerading, ignoring: %m");

        address_del_netlabel(address);

        if (address->state == 0)
                address_free(address);

        link_update_operstate(link, true);

        if (link && !ready)
                link_check_ready(link);

        return 0;
}

int address_get(Link *link, const Address *in, Address **ret) {
        Address *existing;

        assert(link);
        assert(in);

        existing = set_get(link->addresses, in);
        if (!existing)
                return -ENOENT;

        if (ret)
                *ret = existing;
        return 0;
}

int link_get_address(Link *link, int family, const union in_addr_union *address, unsigned char prefixlen, Address **ret) {
        Address *a;
        int r;

        assert(link);
        assert(IN_SET(family, AF_INET, AF_INET6));
        assert(address);

        /* This find an Address object on the link which matches the given address and prefix length
         * and does not have peer address. When the prefixlen is zero, then an Address object with an
         * arbitrary prefixlen will be returned. */

        if (family == AF_INET6 || prefixlen != 0) {
                _cleanup_(address_freep) Address *tmp = NULL;

                /* In this case, we can use address_get(). */

                r = address_new(&tmp);
                if (r < 0)
                        return r;

                tmp->family = family;
                tmp->in_addr = *address;
                tmp->prefixlen = prefixlen;

                r = address_get(link, tmp, &a);
                if (r < 0)
                        return r;

                if (family == AF_INET6) {
                        /* IPv6 addresses are managed without peer address and prefix length. Hence, we need
                         * to check them explicitly. */
                        if (in_addr_is_set(family, &a->in_addr_peer))
                                return -ENOENT;
                        if (prefixlen != 0 && a->prefixlen != prefixlen)
                                return -ENOENT;
                }

                if (ret)
                        *ret = a;

                return 0;
        }

        SET_FOREACH(a, link->addresses) {
                if (a->family != family)
                        continue;

                if (!in_addr_equal(family, &a->in_addr, address))
                        continue;

                if (in_addr_is_set(family, &a->in_addr_peer))
                        continue;

                if (ret)
                        *ret = a;

                return 0;
        }

        return -ENOENT;
}

int manager_get_address(Manager *manager, int family, const union in_addr_union *address, unsigned char prefixlen, Address **ret) {
        Link *link;

        assert(manager);
        assert(IN_SET(family, AF_INET, AF_INET6));
        assert(address);

        HASHMAP_FOREACH(link, manager->links_by_index) {
                if (!IN_SET(link->state, LINK_STATE_CONFIGURING, LINK_STATE_CONFIGURED))
                        continue;

                if (link_get_address(link, family, address, prefixlen, ret) >= 0)
                        return 0;
        }

        return -ENOENT;
}

bool manager_has_address(Manager *manager, int family, const union in_addr_union *address, bool check_ready) {
        Address *a;

        assert(manager);
        assert(IN_SET(family, AF_INET, AF_INET6));
        assert(address);

        if (manager_get_address(manager, family, address, 0, &a) < 0)
                return false;

        return check_ready ? address_is_ready(a) : (address_exists(a) && address_lifetime_is_valid(a));
}

const char* format_lifetime(char *buf, size_t l, usec_t lifetime_usec) {
        assert(buf);
        assert(l > 4);

        if (lifetime_usec == USEC_INFINITY)
                return "forever";

        sprintf(buf, "for ");
        /* format_timespan() never fails */
        assert_se(format_timespan(buf + 4, l - 4, usec_sub_unsigned(lifetime_usec, now(CLOCK_BOOTTIME)), USEC_PER_SEC));
        return buf;
}

static void log_address_debug(const Address *address, const char *str, const Link *link) {
        _cleanup_free_ char *state = NULL, *flags_str = NULL, *scope_str = NULL;

        assert(address);
        assert(str);
        assert(link);

        if (!DEBUG_LOGGING)
                return;

        (void) network_config_state_to_string_alloc(address->state, &state);

        const char *peer = in_addr_is_set(address->family, &address->in_addr_peer) ?
                IN_ADDR_TO_STRING(address->family, &address->in_addr_peer) : NULL;

        (void) address_flags_to_string_alloc(address->flags, address->family, &flags_str);
        (void) route_scope_to_string_alloc(address->scope, &scope_str);

        log_link_debug(link, "%s %s address (%s): %s%s%s/%u (valid %s, preferred %s), flags: %s, scope: %s",
                       str, strna(network_config_source_to_string(address->source)), strna(state),
                       IN_ADDR_TO_STRING(address->family, &address->in_addr),
                       peer ? " peer " : "", strempty(peer), address->prefixlen,
                       FORMAT_LIFETIME(address->lifetime_valid_usec),
                       FORMAT_LIFETIME(address->lifetime_preferred_usec),
                       strna(flags_str), strna(scope_str));
}

static int address_set_netlink_message(const Address *address, sd_netlink_message *m, Link *link) {
        uint32_t flags;
        int r;

        assert(address);
        assert(m);
        assert(link);

        r = sd_rtnl_message_addr_set_prefixlen(m, address->prefixlen);
        if (r < 0)
                return r;

        /* On remove, only IFA_F_MANAGETEMPADDR flag for IPv6 addresses are used. But anyway, set all
         * flags except tentative flag here unconditionally. Without setting the flag, the template
         * addresses generated by kernel will not be removed automatically when the main address is
         * removed. */
        flags = address->flags & ~IFA_F_TENTATIVE;
        r = sd_rtnl_message_addr_set_flags(m, flags & 0xff);
        if (r < 0)
                return r;

        if ((flags & ~0xff) != 0) {
                r = sd_netlink_message_append_u32(m, IFA_FLAGS, flags);
                if (r < 0)
                        return r;
        }

        r = netlink_message_append_in_addr_union(m, IFA_LOCAL, address->family, &address->in_addr);
        if (r < 0)
                return r;

        return 0;
}

static int address_remove_handler(sd_netlink *rtnl, sd_netlink_message *m, Link *link) {
        int r;

        assert(m);
        assert(link);

        if (IN_SET(link->state, LINK_STATE_FAILED, LINK_STATE_LINGER))
                return 0;

        r = sd_netlink_message_get_errno(m);
        if (r < 0 && r != -EADDRNOTAVAIL)
                log_link_message_warning_errno(link, m, r, "Could not drop address");

        return 1;
}

int address_remove(Address *address) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *m = NULL;
        Link *link;
        int r;

        assert(address);
        assert(IN_SET(address->family, AF_INET, AF_INET6));
        assert(address->link);
        assert(address->link->ifindex > 0);
        assert(address->link->manager);
        assert(address->link->manager->rtnl);

        link = address->link;

        log_address_debug(address, "Removing", link);

        r = sd_rtnl_message_new_addr(link->manager->rtnl, &m, RTM_DELADDR,
                                     link->ifindex, address->family);
        if (r < 0)
                return log_link_warning_errno(link, r, "Could not allocate RTM_DELADDR message: %m");

        r = address_set_netlink_message(address, m, link);
        if (r < 0)
                return log_link_warning_errno(link, r, "Could not set netlink attributes: %m");

        r = netlink_call_async(link->manager->rtnl, NULL, m,
                               address_remove_handler,
                               link_netlink_destroy_callback, link);
        if (r < 0)
                return log_link_warning_errno(link, r, "Could not send rtnetlink message: %m");

        link_ref(link);

        address_enter_removing(address);

        /* The operational state is determined by address state and carrier state. Hence, if we remove
         * an address, the operational state may be changed. */
        link_update_operstate(link, true);
        return 0;
}

int address_remove_and_drop(Address *address) {
        if (!address)
                return 0;

        address_cancel_request(address);

        if (address_exists(address))
                return address_remove(address);

        return address_drop(address);
}

bool link_address_is_dynamic(const Link *link, const Address *address) {
        Route *route;

        assert(link);
        assert(address);

        if (address->lifetime_preferred_usec != USEC_INFINITY)
                return true;

        /* Even when the address is leased from a DHCP server, networkd assign the address
         * without lifetime when KeepConfiguration=dhcp. So, let's check that we have
         * corresponding routes with RTPROT_DHCP. */
        SET_FOREACH(route, link->routes) {
                if (route->source != NETWORK_CONFIG_SOURCE_FOREIGN)
                        continue;

                /* The route is not assigned yet, or already removed. Ignoring. */
                if (!route_exists(route))
                        continue;

                if (route->protocol != RTPROT_DHCP)
                        continue;

                if (address->family != route->family)
                        continue;

                if (in_addr_equal(address->family, &address->in_addr, &route->prefsrc))
                        return true;
        }

        return false;
}

int link_drop_ipv6ll_addresses(Link *link) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *req = NULL, *reply = NULL;
        int r;

        assert(link);
        assert(link->manager);
        assert(link->manager->rtnl);

        /* IPv6LL address may be in the tentative state, and in that case networkd has not received it.
         * So, we need to dump all IPv6 addresses. */

        if (link_may_have_ipv6ll(link, /* check_multicast = */ false))
                return 0;

        r = sd_rtnl_message_new_addr(link->manager->rtnl, &req, RTM_GETADDR, link->ifindex, AF_INET6);
        if (r < 0)
                return r;

        r = sd_netlink_message_set_request_dump(req, true);
        if (r < 0)
                return r;

        r = sd_netlink_call(link->manager->rtnl, req, 0, &reply);
        if (r < 0)
                return r;

        for (sd_netlink_message *addr = reply; addr; addr = sd_netlink_message_next(addr)) {
                _cleanup_(address_freep) Address *a = NULL;
                unsigned char flags, prefixlen;
                struct in6_addr address;
                Address *existing;
                int ifindex;

                /* NETLINK_GET_STRICT_CHK socket option is supported since kernel 4.20. To support
                 * older kernels, we need to check ifindex here. */
                r = sd_rtnl_message_addr_get_ifindex(addr, &ifindex);
                if (r < 0) {
                        log_link_debug_errno(link, r, "rtnl: received address message without valid ifindex, ignoring: %m");
                        continue;
                } else if (link->ifindex != ifindex)
                        continue;

                r = sd_rtnl_message_addr_get_flags(addr, &flags);
                if (r < 0) {
                        log_link_debug_errno(link, r, "rtnl: received address message without valid flags, ignoring: %m");
                        continue;
                }

                r = sd_rtnl_message_addr_get_prefixlen(addr, &prefixlen);
                if (r < 0) {
                        log_link_debug_errno(link, r, "rtnl: received address message without prefixlen, ignoring: %m");
                        continue;
                }

                if (sd_netlink_message_read_in6_addr(addr, IFA_LOCAL, NULL) >= 0)
                        /* address with peer, ignoring. */
                        continue;

                r = sd_netlink_message_read_in6_addr(addr, IFA_ADDRESS, &address);
                if (r < 0) {
                        log_link_debug_errno(link, r, "rtnl: received address message without valid address, ignoring: %m");
                        continue;
                }

                if (!in6_addr_is_link_local(&address))
                         continue;

                r = address_new(&a);
                if (r < 0)
                        return -ENOMEM;

                a->family = AF_INET6;
                a->in_addr.in6 = address;
                a->prefixlen = prefixlen;
                a->flags = flags;

                if (address_get(link, a, &existing) < 0) {
                        r = address_add(link, a);
                        if (r < 0)
                                return r;

                        existing = TAKE_PTR(a);
                }

                r = address_remove(existing);
                if (r < 0)
                        return r;
        }

        return 0;
}

int link_drop_foreign_addresses(Link *link) {
        Address *address;
        int k, r = 0;

        assert(link);
        assert(link->network);

        /* First, mark all addresses. */
        SET_FOREACH(address, link->addresses) {
                /* We consider IPv6LL addresses to be managed by the kernel, or dropped in link_drop_ipv6ll_addresses() */
                if (address->family == AF_INET6 && in6_addr_is_link_local(&address->in_addr.in6))
                        continue;

                /* Do not remove localhost address (127.0.0.1 and ::1) */
                if (link->flags & IFF_LOOPBACK && in_addr_is_localhost_one(address->family, &address->in_addr) > 0)
                        continue;

                /* Ignore addresses we configured. */
                if (address->source != NETWORK_CONFIG_SOURCE_FOREIGN)
                        continue;

                /* Ignore addresses not assigned yet or already removing. */
                if (!address_exists(address))
                        continue;

                /* link_address_is_dynamic() is slightly heavy. Let's call the function only when KeepConfiguration= is set. */
                if (IN_SET(link->network->keep_configuration, KEEP_CONFIGURATION_DHCP, KEEP_CONFIGURATION_STATIC) &&
                    link_address_is_dynamic(link, address) == (link->network->keep_configuration == KEEP_CONFIGURATION_DHCP))
                        continue;

                address_mark(address);
        }

        /* Then, unmark requested addresses. */
        ORDERED_HASHMAP_FOREACH(address, link->network->addresses_by_section) {
                Address *existing;

                /* On update, the kernel ignores the address label and broadcast address. Hence we need to
                 * distinguish addresses with different labels or broadcast addresses. Thus, we need to check
                 * the existing address with address_equal(). Otherwise, the label or broadcast address
                 * change will not be applied when we reconfigure the interface. */
                if (address_get(link, address, &existing) >= 0 && address_equal(address, existing))
                        address_unmark(existing);
        }

        /* Finally, remove all marked addresses. */
        SET_FOREACH(address, link->addresses) {
                if (!address_is_marked(address))
                        continue;

                k = address_remove(address);
                if (k < 0 && r >= 0)
                        r = k;
        }

        return r;
}

int link_drop_managed_addresses(Link *link) {
        Address *address;
        int k, r = 0;

        assert(link);

        SET_FOREACH(address, link->addresses) {
                /* Do not touch addresses managed by kernel or other tools. */
                if (address->source == NETWORK_CONFIG_SOURCE_FOREIGN)
                        continue;

                /* Ignore addresses not assigned yet or already removing. */
                if (!address_exists(address))
                        continue;

                k = address_remove(address);
                if (k < 0 && r >= 0) {
                        r = k;
                        continue;
                }
        }

        return r;
}

void link_foreignize_addresses(Link *link) {
        Address *address;

        assert(link);

        SET_FOREACH(address, link->addresses)
                address->source = NETWORK_CONFIG_SOURCE_FOREIGN;
}

static int address_acquire(Link *link, const Address *original, Address **ret) {
        _cleanup_(address_freep) Address *na = NULL;
        union in_addr_union in_addr;
        int r;

        assert(link);
        assert(original);
        assert(ret);

        /* Something useful was configured? just use it */
        if (in_addr_is_set(original->family, &original->in_addr)) {
                *ret = NULL;
                return 0;
        }

        /* The address is configured to be 0.0.0.0 or [::] by the user?
         * Then let's acquire something more useful from the pool. */
        r = address_pool_acquire(link->manager, original->family, original->prefixlen, &in_addr);
        if (r < 0)
                return r;
        if (r == 0)
                return -EBUSY;

        /* Pick first address in range for ourselves. */
        if (original->family == AF_INET)
                in_addr.in.s_addr = in_addr.in.s_addr | htobe32(1);
        else if (original->family == AF_INET6)
                in_addr.in6.s6_addr[15] |= 1;

        r = address_dup(original, &na);
        if (r < 0)
                return r;

        na->in_addr = in_addr;

        *ret = TAKE_PTR(na);
        return 1;
}

int address_configure_handler_internal(sd_netlink *rtnl, sd_netlink_message *m, Link *link, const char *error_msg) {
        int r;

        assert(rtnl);
        assert(m);
        assert(link);
        assert(error_msg);

        r = sd_netlink_message_get_errno(m);
        if (r < 0 && r != -EEXIST) {
                log_link_message_warning_errno(link, m, r, error_msg);
                link_enter_failed(link);
                return 0;
        }

        return 1;
}

static int address_configure(const Address *address, const struct ifa_cacheinfo *c, Link *link, Request *req) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *m = NULL;
        int r;

        assert(address);
        assert(IN_SET(address->family, AF_INET, AF_INET6));
        assert(c);
        assert(link);
        assert(link->ifindex > 0);
        assert(link->manager);
        assert(link->manager->rtnl);
        assert(req);

        log_address_debug(address, "Configuring", link);

        r = sd_rtnl_message_new_addr_update(link->manager->rtnl, &m, link->ifindex, address->family);
        if (r < 0)
                return r;

        r = address_set_netlink_message(address, m, link);
        if (r < 0)
                return r;

        r = sd_rtnl_message_addr_set_scope(m, address->scope);
        if (r < 0)
                return r;

        if (in_addr_is_set(address->family, &address->in_addr_peer)) {
                r = netlink_message_append_in_addr_union(m, IFA_ADDRESS, address->family, &address->in_addr_peer);
                if (r < 0)
                        return r;
        } else if (in4_addr_is_set(&address->broadcast)) {
                r = sd_netlink_message_append_in_addr(m, IFA_BROADCAST, &address->broadcast);
                if (r < 0)
                        return r;
        }

        if (address->family == AF_INET && address->label) {
                r = sd_netlink_message_append_string(m, IFA_LABEL, address->label);
                if (r < 0)
                        return r;
        }

        r = sd_netlink_message_append_cache_info(m, IFA_CACHEINFO, c);
        if (r < 0)
                return r;

        r = sd_netlink_message_append_u32(m, IFA_RT_PRIORITY, address->route_metric);
        if (r < 0)
                return r;

        return request_call_netlink_async(link->manager->rtnl, m, req);
}

static bool address_is_ready_to_configure(Link *link, const Address *address) {
        assert(link);
        assert(address);

        if (!link_is_ready_to_configure(link, false))
                return false;

        if (address_is_removing(address))
                return false;

        if (!ipv4acd_bound(address))
                return false;

        /* Refuse adding more than the limit */
        if (set_size(link->addresses) >= ADDRESSES_PER_LINK_MAX)
                return false;

        return true;
}

static int address_process_request(Request *req, Link *link, Address *address) {
        struct ifa_cacheinfo c;
        int r;

        assert(req);
        assert(link);
        assert(address);

        if (!address_is_ready_to_configure(link, address))
                return 0;

        address_set_cinfo(link->manager, address, &c);
        if (c.ifa_valid == 0) {
                log_link_debug(link, "Refuse to configure %s address %s, as its valid lifetime is zero.",
                               network_config_source_to_string(address->source),
                               IN_ADDR_PREFIX_TO_STRING(address->family, &address->in_addr, address->prefixlen));
                address_cancel_requesting(address);
                return 1;
        }

        r = address_configure(address, &c, link, req);
        if (r < 0)
                return log_link_warning_errno(link, r, "Failed to configure address: %m");

        address_enter_configuring(address);
        return 1;
}

int link_request_address(
                Link *link,
                Address *address,
                bool consume_object,
                unsigned *message_counter,
                address_netlink_handler_t netlink_handler,
                Request **ret) {

        Address *acquired, *existing = NULL;
        int r;

        assert(link);
        assert(address);
        assert(address->source != NETWORK_CONFIG_SOURCE_FOREIGN);

        r = address_acquire(link, address, &acquired);
        if (r < 0)
                return log_link_warning_errno(link, r, "Failed to acquire an address from pool: %m");
        if (r > 0) {
                if (consume_object)
                        address_free(address);

                address = acquired;
                consume_object = true;
        }

        if (address_needs_to_set_broadcast(address, link)) {
                if (!consume_object) {
                        Address *a;

                        r = address_dup(address, &a);
                        if (r < 0)
                                return r;

                        address = a;
                        consume_object = true;
                }

                address_set_broadcast(address, link);
        }

        (void) address_get(link, address, &existing);

        if (address->lifetime_valid_usec == 0) {
                if (consume_object)
                        address_free(address);

                /* The requested address is outdated. Let's remove it. */
                return address_remove_and_drop(existing);
        }

        if (!existing) {
                _cleanup_(address_freep) Address *tmp = NULL;

                if (consume_object)
                        tmp = address;
                else {
                        r = address_dup(address, &tmp);
                        if (r < 0)
                                return r;
                }

                /* Consider address tentative until we get the real flags from the kernel */
                tmp->flags |= IFA_F_TENTATIVE;

                r = address_add(link, tmp);
                if (r < 0)
                        return r;

                existing = TAKE_PTR(tmp);
        } else {
                r = address_equalify(existing, address);
                if (r < 0)
                        return r;
                existing->source = address->source;
                existing->provider = address->provider;
                existing->duplicate_address_detection = address->duplicate_address_detection;
                existing->lifetime_valid_usec = address->lifetime_valid_usec;
                existing->lifetime_preferred_usec = address->lifetime_preferred_usec;
                if (consume_object)
                        address_free(address);
        }

        r = ipv4acd_configure(existing);
        if (r < 0)
                return r;

        log_address_debug(existing, "Requesting", link);
        r = link_queue_request_safe(link, REQUEST_TYPE_ADDRESS,
                                    existing, NULL,
                                    address_hash_func,
                                    address_compare_func,
                                    address_process_request,
                                    message_counter, netlink_handler, ret);
        if (r < 0)
                return log_link_warning_errno(link, r, "Failed to request address: %m");
        if (r == 0)
                return 0;

        address_enter_requesting(existing);

        return 1;
}

static int static_address_handler(sd_netlink *rtnl, sd_netlink_message *m, Request *req, Link *link, Address *address) {
        int r;

        assert(link);

        r = address_configure_handler_internal(rtnl, m, link, "Failed to set static address");
        if (r <= 0)
                return r;

        if (link->static_address_messages == 0) {
                log_link_debug(link, "Addresses set");
                link->static_addresses_configured = true;
                link_check_ready(link);
        }

        return 1;
}

int link_request_static_address(Link *link, Address *address, bool consume) {
        assert(link);
        assert(address);
        assert(address->source == NETWORK_CONFIG_SOURCE_STATIC);

        return link_request_address(link, address, consume, &link->static_address_messages,
                                    static_address_handler, NULL);
}

int link_request_static_addresses(Link *link) {
        Address *a;
        int r;

        assert(link);
        assert(link->network);

        link->static_addresses_configured = false;

        ORDERED_HASHMAP_FOREACH(a, link->network->addresses_by_section) {
                r = link_request_static_address(link, a, false);
                if (r < 0)
                        return r;
        }

        r = link_request_radv_addresses(link);
        if (r < 0)
                return r;

        r = link_request_dhcp_server_address(link);
        if (r < 0)
                return r;

        if (link->static_address_messages == 0) {
                link->static_addresses_configured = true;
                link_check_ready(link);
        } else {
                log_link_debug(link, "Setting addresses");
                link_set_state(link, LINK_STATE_CONFIGURING);
        }

        return 0;
}

void address_cancel_request(Address *address) {
        Request req;

        assert(address);
        assert(address->link);

        if (!address_is_requesting(address))
                return;

        req = (Request) {
                .link = address->link,
                .type = REQUEST_TYPE_ADDRESS,
                .userdata = address,
                .hash_func = (hash_func_t) address_hash_func,
                .compare_func = (compare_func_t) address_compare_func,
        };

        request_detach(address->link->manager, &req);
        address_cancel_requesting(address);
}

int manager_rtnl_process_address(sd_netlink *rtnl, sd_netlink_message *message, Manager *m) {
        _cleanup_(address_freep) Address *tmp = NULL;
        struct ifa_cacheinfo cinfo;
        Link *link = NULL;
        uint16_t type;
        Address *address = NULL;
        int ifindex, r;

        assert(rtnl);
        assert(message);
        assert(m);

        if (sd_netlink_message_is_error(message)) {
                r = sd_netlink_message_get_errno(message);
                if (r < 0)
                        log_message_warning_errno(message, r, "rtnl: failed to receive address message, ignoring");

                return 0;
        }

        r = sd_netlink_message_get_type(message, &type);
        if (r < 0) {
                log_warning_errno(r, "rtnl: could not get message type, ignoring: %m");
                return 0;
        } else if (!IN_SET(type, RTM_NEWADDR, RTM_DELADDR)) {
                log_warning("rtnl: received unexpected message type %u when processing address, ignoring.", type);
                return 0;
        }

        r = sd_rtnl_message_addr_get_ifindex(message, &ifindex);
        if (r < 0) {
                log_warning_errno(r, "rtnl: could not get ifindex from message, ignoring: %m");
                return 0;
        } else if (ifindex <= 0) {
                log_warning("rtnl: received address message with invalid ifindex %d, ignoring.", ifindex);
                return 0;
        }

        r = link_get_by_index(m, ifindex, &link);
        if (r < 0 || !link) {
                /* when enumerating we might be out of sync, but we will get the address again, so just
                 * ignore it */
                if (!m->enumerating)
                        log_warning("rtnl: received address for link '%d' we don't know about, ignoring.", ifindex);
                return 0;
        }

        r = address_new(&tmp);
        if (r < 0)
                return log_oom();

        r = sd_rtnl_message_addr_get_family(message, &tmp->family);
        if (r < 0) {
                log_link_warning(link, "rtnl: received address message without family, ignoring.");
                return 0;
        } else if (!IN_SET(tmp->family, AF_INET, AF_INET6)) {
                log_link_debug(link, "rtnl: received address message with invalid family '%i', ignoring.", tmp->family);
                return 0;
        }

        r = sd_rtnl_message_addr_get_prefixlen(message, &tmp->prefixlen);
        if (r < 0) {
                log_link_warning_errno(link, r, "rtnl: received address message without prefixlen, ignoring: %m");
                return 0;
        }

        r = sd_rtnl_message_addr_get_scope(message, &tmp->scope);
        if (r < 0) {
                log_link_warning_errno(link, r, "rtnl: received address message without scope, ignoring: %m");
                return 0;
        }

        r = sd_netlink_message_read_u32(message, IFA_FLAGS, &tmp->flags);
        if (r == -ENODATA) {
                unsigned char flags;

                /* For old kernels. */
                r = sd_rtnl_message_addr_get_flags(message, &flags);
                if (r >= 0)
                        tmp->flags = flags;
        }
        if (r < 0) {
                log_link_warning_errno(link, r, "rtnl: received address message without flags, ignoring: %m");
                return 0;
        }

        switch (tmp->family) {
        case AF_INET:
                r = sd_netlink_message_read_in_addr(message, IFA_LOCAL, &tmp->in_addr.in);
                if (r < 0) {
                        log_link_warning_errno(link, r, "rtnl: received address message without valid address, ignoring: %m");
                        return 0;
                }

                r = sd_netlink_message_read_in_addr(message, IFA_ADDRESS, &tmp->in_addr_peer.in);
                if (r < 0 && r != -ENODATA) {
                        log_link_warning_errno(link, r, "rtnl: could not get peer address from address message, ignoring: %m");
                        return 0;
                } else if (r >= 0) {
                        if (in4_addr_equal(&tmp->in_addr.in, &tmp->in_addr_peer.in))
                                tmp->in_addr_peer = IN_ADDR_NULL;
                }

                r = sd_netlink_message_read_in_addr(message, IFA_BROADCAST, &tmp->broadcast);
                if (r < 0 && r != -ENODATA) {
                        log_link_warning_errno(link, r, "rtnl: could not get broadcast from address message, ignoring: %m");
                        return 0;
                }

                r = sd_netlink_message_read_string_strdup(message, IFA_LABEL, &tmp->label);
                if (r < 0 && r != -ENODATA) {
                        log_link_warning_errno(link, r, "rtnl: could not get label from address message, ignoring: %m");
                        return 0;
                } else if (r >= 0 && streq_ptr(tmp->label, link->ifname))
                        tmp->label = mfree(tmp->label);

                break;

        case AF_INET6:
                r = sd_netlink_message_read_in6_addr(message, IFA_LOCAL, &tmp->in_addr.in6);
                if (r >= 0) {
                        /* Have peer address. */
                        r = sd_netlink_message_read_in6_addr(message, IFA_ADDRESS, &tmp->in_addr_peer.in6);
                        if (r < 0) {
                                log_link_warning_errno(link, r, "rtnl: could not get peer address from address message, ignoring: %m");
                                return 0;
                        }
                } else if (r == -ENODATA) {
                        /* Does not have peer address. */
                        r = sd_netlink_message_read_in6_addr(message, IFA_ADDRESS, &tmp->in_addr.in6);
                        if (r < 0) {
                                log_link_warning_errno(link, r, "rtnl: received address message without valid address, ignoring: %m");
                                return 0;
                        }
                } else {
                        log_link_warning_errno(link, r, "rtnl: could not get local address from address message, ignoring: %m");
                        return 0;
                }

                break;

        default:
                assert_not_reached();
        }

        r = sd_netlink_message_read_cache_info(message, IFA_CACHEINFO, &cinfo);
        if (r < 0 && r != -ENODATA) {
                log_link_warning_errno(link, r, "rtnl: cannot get IFA_CACHEINFO attribute, ignoring: %m");
                return 0;
        }

        (void) address_get(link, tmp, &address);

        switch (type) {
        case RTM_NEWADDR:
                if (address) {
                        /* update flags and etc. */
                        r = address_equalify(address, tmp);
                        if (r < 0) {
                                log_link_warning_errno(link, r, "Failed to update properties of address %s, ignoring: %m",
                                                       IN_ADDR_PREFIX_TO_STRING(address->family, &address->in_addr, address->prefixlen));
                                return 0;
                        }
                        address->flags = tmp->flags;
                        address->scope = tmp->scope;
                        address_set_lifetime(m, address, &cinfo);
                        address_enter_configured(address);
                        log_address_debug(address, "Received updated", link);
                } else {
                        address_set_lifetime(m, tmp, &cinfo);
                        address_enter_configured(tmp);
                        log_address_debug(tmp, "Received new", link);

                        r = address_add(link, tmp);
                        if (r < 0) {
                                log_link_warning_errno(link, r, "Failed to remember foreign address %s, ignoring: %m",
                                                       IN_ADDR_PREFIX_TO_STRING(tmp->family, &tmp->in_addr, tmp->prefixlen));
                                return 0;
                        }

                        address = TAKE_PTR(tmp);
                }

                /* address_update() logs internally, so we don't need to here. */
                r = address_update(address);
                if (r < 0)
                        link_enter_failed(link);

                break;

        case RTM_DELADDR:
                if (address) {
                        address_enter_removed(address);
                        log_address_debug(address, address->state == 0 ? "Forgetting" : "Removed", link);
                        (void) address_drop(address);
                } else
                        log_address_debug(tmp, "Kernel removed unknown", link);

                break;

        default:
                assert_not_reached();
        }

        return 1;
}

int config_parse_broadcast(
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

        Network *network = userdata;
        _cleanup_(address_free_or_set_invalidp) Address *n = NULL;
        union in_addr_union u;
        int r;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = address_new_static(network, filename, section_line, &n);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to allocate new address, ignoring assignment: %m");
                return 0;
        }

        if (isempty(rvalue)) {
                /* The broadcast address will be calculated based on Address=, and set if the link is
                 * not a wireguard interface. Here, we do not check or set n->family. */
                n->broadcast = (struct in_addr) {};
                n->set_broadcast = -1;
                TAKE_PTR(n);
                return 0;
        }

        r = parse_boolean(rvalue);
        if (r >= 0) {
                /* The broadcast address will be calculated based on Address=. Here, we do not check or
                 * set n->family. */
                n->broadcast = (struct in_addr) {};
                n->set_broadcast = r;
                TAKE_PTR(n);
                return 0;
        }

        if (n->family == AF_INET6) {
                log_syntax(unit, LOG_WARNING, filename, line, 0,
                           "Broadcast is not valid for IPv6 addresses, ignoring assignment: %s", rvalue);
                return 0;
        }

        r = in_addr_from_string(AF_INET, rvalue, &u);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Broadcast is invalid, ignoring assignment: %s", rvalue);
                return 0;
        }
        if (in4_addr_is_null(&u.in)) {
                log_syntax(unit, LOG_WARNING, filename, line, 0,
                           "Broadcast cannot be ANY address, ignoring assignment: %s", rvalue);
                return 0;
        }

        n->broadcast = u.in;
        n->set_broadcast = true;
        n->family = AF_INET;
        TAKE_PTR(n);

        return 0;
}

int config_parse_address(
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

        Network *network = userdata;
        _cleanup_(address_free_or_set_invalidp) Address *n = NULL;
        union in_addr_union buffer;
        unsigned char prefixlen;
        int r, f;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        if (streq(section, "Network"))
                /* we are not in an Address section, so use line number instead. */
                r = address_new_static(network, filename, line, &n);
        else
                r = address_new_static(network, filename, section_line, &n);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to allocate new address, ignoring assignment: %m");
                return 0;
        }

        /* Address=address/prefixlen */
        r = in_addr_prefix_from_string_auto_internal(rvalue, PREFIXLEN_REFUSE, &f, &buffer, &prefixlen);
        if (r == -ENOANO) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "An address '%s' is specified without prefix length. "
                           "The behavior of parsing addresses without prefix length will be changed in the future release. "
                           "Please specify prefix length explicitly.", rvalue);

                r = in_addr_prefix_from_string_auto_internal(rvalue, PREFIXLEN_LEGACY, &f, &buffer, &prefixlen);
        }
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r, "Invalid address '%s', ignoring assignment: %m", rvalue);
                return 0;
        }

        if (n->family != AF_UNSPEC && f != n->family) {
                log_syntax(unit, LOG_WARNING, filename, line, 0, "Address is incompatible, ignoring assignment: %s", rvalue);
                return 0;
        }

        if (in_addr_is_null(f, &buffer)) {
                /* Will use address from address pool. Note that for ipv6 case, prefix of the address
                 * pool is 8, but 40 bit is used by the global ID and 16 bit by the subnet ID. So,
                 * let's limit the prefix length to 64 or larger. See RFC4193. */
                if ((f == AF_INET && prefixlen < 8) ||
                    (f == AF_INET6 && prefixlen < 64)) {
                        log_syntax(unit, LOG_WARNING, filename, line, 0,
                                   "Null address with invalid prefixlen='%u', ignoring assignment: %s",
                                   prefixlen, rvalue);
                        return 0;
                }
        }

        n->family = f;
        n->prefixlen = prefixlen;

        if (streq(lvalue, "Address"))
                n->in_addr = buffer;
        else
                n->in_addr_peer = buffer;

        TAKE_PTR(n);
        return 0;
}

int config_parse_label(
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

        _cleanup_(address_free_or_set_invalidp) Address *n = NULL;
        Network *network = userdata;
        int r;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = address_new_static(network, filename, section_line, &n);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to allocate new address, ignoring assignment: %m");
                return 0;
        }

        if (isempty(rvalue)) {
                n->label = mfree(n->label);
                TAKE_PTR(n);
                return 0;
        }

        if (!address_label_valid(rvalue)) {
                log_syntax(unit, LOG_WARNING, filename, line, 0,
                           "Interface label is too long or invalid, ignoring assignment: %s", rvalue);
                return 0;
        }

        r = free_and_strdup(&n->label, rvalue);
        if (r < 0)
                return log_oom();

        TAKE_PTR(n);
        return 0;
}

int config_parse_lifetime(
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

        Network *network = userdata;
        _cleanup_(address_free_or_set_invalidp) Address *n = NULL;
        usec_t k;
        int r;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = address_new_static(network, filename, section_line, &n);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to allocate new address, ignoring assignment: %m");
                return 0;
        }

        /* We accept only "forever", "infinity", empty, or "0". */
        if (STR_IN_SET(rvalue, "forever", "infinity", ""))
                k = USEC_INFINITY;
        else if (streq(rvalue, "0"))
                k = 0;
        else {
                log_syntax(unit, LOG_WARNING, filename, line, 0,
                           "Invalid PreferredLifetime= value, ignoring: %s", rvalue);
                return 0;
        }

        n->lifetime_preferred_usec = k;
        TAKE_PTR(n);

        return 0;
}

int config_parse_address_flags(
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

        Network *network = userdata;
        _cleanup_(address_free_or_set_invalidp) Address *n = NULL;
        int r;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = address_new_static(network, filename, section_line, &n);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to allocate new address, ignoring assignment: %m");
                return 0;
        }

        r = parse_boolean(rvalue);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to parse %s=, ignoring: %s", lvalue, rvalue);
                return 0;
        }

        if (streq(lvalue, "AddPrefixRoute"))
                r = !r;

        SET_FLAG(n->flags, ltype, r);

        TAKE_PTR(n);
        return 0;
}

int config_parse_address_scope(
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

        Network *network = userdata;
        _cleanup_(address_free_or_set_invalidp) Address *n = NULL;
        int r;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = address_new_static(network, filename, section_line, &n);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to allocate new address, ignoring assignment: %m");
                return 0;
        }

        r = route_scope_from_string(rvalue);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Could not parse address scope \"%s\", ignoring assignment: %m", rvalue);
                return 0;
        }

        n->scope = r;
        n->scope_set = true;
        TAKE_PTR(n);
        return 0;
}

int config_parse_address_route_metric(
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

        Network *network = userdata;
        _cleanup_(address_free_or_set_invalidp) Address *n = NULL;
        int r;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = address_new_static(network, filename, section_line, &n);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to allocate new address, ignoring assignment: %m");
                return 0;
        }

        r = safe_atou32(rvalue, &n->route_metric);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Could not parse %s=, ignoring assignment: %s", lvalue, rvalue);
                return 0;
        }

        TAKE_PTR(n);
        return 0;
}

int config_parse_duplicate_address_detection(
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

        Network *network = userdata;
        _cleanup_(address_free_or_set_invalidp) Address *n = NULL;
        int r;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = address_new_static(network, filename, section_line, &n);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to allocate new address, ignoring assignment: %m");
                return 0;
        }

        r = parse_boolean(rvalue);
        if (r >= 0) {
                log_syntax(unit, LOG_WARNING, filename, line, 0,
                           "For historical reasons, %s=%s means %s=%s. "
                           "Please use 'both', 'ipv4', 'ipv6' or 'none' instead.",
                           lvalue, rvalue, lvalue, r ? "none" : "both");
                n->duplicate_address_detection = r ? ADDRESS_FAMILY_NO : ADDRESS_FAMILY_YES;
                n = NULL;
                return 0;
        }

        AddressFamily a = duplicate_address_detection_address_family_from_string(rvalue);
        if (a < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, a,
                           "Failed to parse %s=, ignoring: %s", lvalue, rvalue);
                return 0;
        }
        n->duplicate_address_detection = a;

        TAKE_PTR(n);
        return 0;
}

int config_parse_address_netlabel(
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

        Network *network = userdata;
        _cleanup_(address_free_or_set_invalidp) Address *n = NULL;
        int r;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);
        assert(network);

        r = address_new_static(network, filename, section_line, &n);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to allocate new address, ignoring assignment: %m");
                return 0;
        }

        r = config_parse_string(unit, filename, line, section, section_line,
                                lvalue, CONFIG_PARSE_STRING_SAFE, rvalue, &n->netlabel, network);
        if (r < 0)
                return r;

        TAKE_PTR(n);
        return 0;
}

static int address_section_verify(Address *address) {
        if (section_is_invalid(address->section))
                return -EINVAL;

        if (address->family == AF_UNSPEC) {
                assert(address->section);

                return log_warning_errno(SYNTHETIC_ERRNO(EINVAL),
                                         "%s: Address section without Address= field configured. "
                                         "Ignoring [Address] section from line %u.",
                                         address->section->filename, address->section->line);
        }

        assert(IN_SET(address->family, AF_INET, AF_INET6));

        if (in4_addr_is_set(&address->broadcast) &&
            (address->family == AF_INET6 || address->prefixlen > 30 ||
             in_addr_is_set(address->family, &address->in_addr_peer))) {
                log_warning("%s: broadcast address is set for an IPv6 address, "
                            "an IPv4 address with peer address, or with prefix length larger than 30. "
                            "Ignoring Broadcast= setting in the [Address] section from line %u.",
                            address->section->filename, address->section->line);

                address->broadcast.s_addr = 0;
        }

        if (address->family == AF_INET6 && address->label) {
                log_warning("%s: address label is set for IPv6 address in the [Address] section from line %u. "
                            "Ignoring Label= setting.",
                            address->section->filename, address->section->line);

                address->label = mfree(address->label);
        }

        if (!address->scope_set) {
                if (in_addr_is_localhost(address->family, &address->in_addr) > 0)
                        address->scope = RT_SCOPE_HOST;
                else if (in_addr_is_link_local(address->family, &address->in_addr) > 0)
                        address->scope = RT_SCOPE_LINK;
        }

        if (address->duplicate_address_detection < 0) {
                if (address->family == AF_INET6)
                        address->duplicate_address_detection = ADDRESS_FAMILY_IPV6;
                else if (in4_addr_is_link_local(&address->in_addr.in))
                        address->duplicate_address_detection = ADDRESS_FAMILY_IPV4;
                else
                        address->duplicate_address_detection = ADDRESS_FAMILY_NO;
        } else if (address->duplicate_address_detection == ADDRESS_FAMILY_IPV6 && address->family == AF_INET)
                log_warning("%s: DuplicateAddressDetection=ipv6 is specified for IPv4 address, ignoring.",
                            address->section->filename);
        else if (address->duplicate_address_detection == ADDRESS_FAMILY_IPV4 && address->family == AF_INET6)
                log_warning("%s: DuplicateAddressDetection=ipv4 is specified for IPv6 address, ignoring.",
                            address->section->filename);

        if (address->family == AF_INET6 &&
            !FLAGS_SET(address->duplicate_address_detection, ADDRESS_FAMILY_IPV6))
                address->flags |= IFA_F_NODAD;

        return 0;
}

int network_drop_invalid_addresses(Network *network) {
        _cleanup_set_free_ Set *addresses = NULL;
        Address *address;
        int r;

        assert(network);

        ORDERED_HASHMAP_FOREACH(address, network->addresses_by_section) {
                Address *dup;

                if (address_section_verify(address) < 0) {
                        /* Drop invalid [Address] sections or Address= settings in [Network].
                         * Note that address_free() will drop the address from addresses_by_section. */
                        address_free(address);
                        continue;
                }

                /* Always use the setting specified later. So, remove the previously assigned setting. */
                dup = set_remove(addresses, address);
                if (dup) {
                        log_warning("%s: Duplicated address %s is specified at line %u and %u, "
                                    "dropping the address setting specified at line %u.",
                                    dup->section->filename,
                                    IN_ADDR_PREFIX_TO_STRING(address->family, &address->in_addr, address->prefixlen),
                                    address->section->line,
                                    dup->section->line, dup->section->line);
                        /* address_free() will drop the address from addresses_by_section. */
                        address_free(dup);
                }

                /* Use address_kernel_hash_ops, instead of address_kernel_hash_ops_free. Otherwise, the
                 * Address objects will be freed. */
                r = set_ensure_put(&addresses, &address_kernel_hash_ops, address);
                if (r < 0)
                        return log_oom();
                assert(r > 0);
        }

        return 0;
}
