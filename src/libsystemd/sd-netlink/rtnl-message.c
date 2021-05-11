/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <netinet/in.h>
#include <linux/if_addrlabel.h>
#include <linux/if_bridge.h>
#include <linux/nexthop.h>
#include <stdbool.h>
#include <unistd.h>

#include "sd-netlink.h"

#include "format-util.h"
#include "netlink-internal.h"
#include "netlink-types.h"
#include "netlink-util.h"
#include "socket-util.h"
#include "util.h"

int sd_rtnl_message_route_set_dst_prefixlen(sd_netlink_message *m, unsigned char prefixlen) {
        struct rtmsg *rtm;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_route(m->hdr->nlmsg_type), -EINVAL);

        rtm = NLMSG_DATA(m->hdr);

        if ((rtm->rtm_family == AF_INET && prefixlen > 32) ||
            (rtm->rtm_family == AF_INET6 && prefixlen > 128))
                return -ERANGE;

        rtm->rtm_dst_len = prefixlen;

        return 0;
}

int sd_rtnl_message_route_set_src_prefixlen(sd_netlink_message *m, unsigned char prefixlen) {
        struct rtmsg *rtm;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_route(m->hdr->nlmsg_type), -EINVAL);

        rtm = NLMSG_DATA(m->hdr);

        if ((rtm->rtm_family == AF_INET && prefixlen > 32) ||
            (rtm->rtm_family == AF_INET6 && prefixlen > 128))
                return -ERANGE;

        rtm->rtm_src_len = prefixlen;

        return 0;
}

int sd_rtnl_message_route_set_scope(sd_netlink_message *m, unsigned char scope) {
        struct rtmsg *rtm;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_route(m->hdr->nlmsg_type), -EINVAL);

        rtm = NLMSG_DATA(m->hdr);

        rtm->rtm_scope = scope;

        return 0;
}

int sd_rtnl_message_route_set_flags(sd_netlink_message *m, unsigned flags) {
        struct rtmsg *rtm;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_route(m->hdr->nlmsg_type), -EINVAL);

        rtm = NLMSG_DATA(m->hdr);

        rtm->rtm_flags = flags;

        return 0;
}

int sd_rtnl_message_route_get_flags(const sd_netlink_message *m, unsigned *flags) {
        struct rtmsg *rtm;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_route(m->hdr->nlmsg_type), -EINVAL);
        assert_return(flags, -EINVAL);

        rtm = NLMSG_DATA(m->hdr);

        *flags = rtm->rtm_flags;

        return 0;
}

int sd_rtnl_message_route_set_table(sd_netlink_message *m, unsigned char table) {
        struct rtmsg *rtm;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_route(m->hdr->nlmsg_type), -EINVAL);

        rtm = NLMSG_DATA(m->hdr);

        rtm->rtm_table = table;

        return 0;
}

int sd_rtnl_message_route_get_family(const sd_netlink_message *m, int *family) {
        struct rtmsg *rtm;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_route(m->hdr->nlmsg_type), -EINVAL);
        assert_return(family, -EINVAL);

        rtm = NLMSG_DATA(m->hdr);

        *family = rtm->rtm_family;

        return 0;
}

int sd_rtnl_message_route_set_family(sd_netlink_message *m, int family) {
        struct rtmsg *rtm;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_route(m->hdr->nlmsg_type), -EINVAL);

        rtm = NLMSG_DATA(m->hdr);

        rtm->rtm_family = family;

        return 0;
}

int sd_rtnl_message_route_get_type(const sd_netlink_message *m, unsigned char *type) {
        struct rtmsg *rtm;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_route(m->hdr->nlmsg_type), -EINVAL);
        assert_return(type, -EINVAL);

        rtm = NLMSG_DATA(m->hdr);

        *type = rtm->rtm_type;

        return 0;
}

int sd_rtnl_message_route_set_type(sd_netlink_message *m, unsigned char type) {
        struct rtmsg *rtm;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_route(m->hdr->nlmsg_type), -EINVAL);

        rtm = NLMSG_DATA(m->hdr);

        rtm->rtm_type = type;

        return 0;
}

int sd_rtnl_message_route_get_protocol(const sd_netlink_message *m, unsigned char *protocol) {
        struct rtmsg *rtm;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_route(m->hdr->nlmsg_type), -EINVAL);
        assert_return(protocol, -EINVAL);

        rtm = NLMSG_DATA(m->hdr);

        *protocol = rtm->rtm_protocol;

        return 0;
}

int sd_rtnl_message_route_get_scope(const sd_netlink_message *m, unsigned char *scope) {
        struct rtmsg *rtm;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_route(m->hdr->nlmsg_type), -EINVAL);
        assert_return(scope, -EINVAL);

        rtm = NLMSG_DATA(m->hdr);

        *scope = rtm->rtm_scope;

        return 0;
}

int sd_rtnl_message_route_get_tos(const sd_netlink_message *m, unsigned char *tos) {
        struct rtmsg *rtm;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_route(m->hdr->nlmsg_type), -EINVAL);
        assert_return(tos, -EINVAL);

        rtm = NLMSG_DATA(m->hdr);

        *tos = rtm->rtm_tos;

        return 0;
}

int sd_rtnl_message_route_get_table(const sd_netlink_message *m, unsigned char *table) {
        struct rtmsg *rtm;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_route(m->hdr->nlmsg_type), -EINVAL);
        assert_return(table, -EINVAL);

        rtm = NLMSG_DATA(m->hdr);

        *table = rtm->rtm_table;

        return 0;
}

int sd_rtnl_message_route_get_dst_prefixlen(const sd_netlink_message *m, unsigned char *dst_len) {
        struct rtmsg *rtm;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_route(m->hdr->nlmsg_type), -EINVAL);
        assert_return(dst_len, -EINVAL);

        rtm = NLMSG_DATA(m->hdr);

        *dst_len = rtm->rtm_dst_len;

        return 0;
}

int sd_rtnl_message_route_get_src_prefixlen(const sd_netlink_message *m, unsigned char *src_len) {
        struct rtmsg *rtm;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_route(m->hdr->nlmsg_type), -EINVAL);
        assert_return(src_len, -EINVAL);

        rtm = NLMSG_DATA(m->hdr);

        *src_len = rtm->rtm_src_len;

        return 0;
}

int sd_rtnl_message_new_route(sd_netlink *rtnl, sd_netlink_message **ret,
                              uint16_t nlmsg_type, int rtm_family,
                              unsigned char rtm_protocol) {
        struct rtmsg *rtm;
        int r;

        assert_return(rtnl_message_type_is_route(nlmsg_type), -EINVAL);
        assert_return((nlmsg_type == RTM_GETROUTE && rtm_family == AF_UNSPEC) ||
                      IN_SET(rtm_family, AF_INET, AF_INET6), -EINVAL);
        assert_return(ret, -EINVAL);

        r = message_new(rtnl, ret, nlmsg_type);
        if (r < 0)
                return r;

        if (nlmsg_type == RTM_NEWROUTE)
                (*ret)->hdr->nlmsg_flags |= NLM_F_CREATE | NLM_F_APPEND;

        rtm = NLMSG_DATA((*ret)->hdr);

        rtm->rtm_family = rtm_family;
        rtm->rtm_scope = RT_SCOPE_UNIVERSE;
        rtm->rtm_type = RTN_UNICAST;
        rtm->rtm_table = RT_TABLE_MAIN;
        rtm->rtm_protocol = rtm_protocol;

        return 0;
}

int sd_rtnl_message_new_nexthop(sd_netlink *rtnl, sd_netlink_message **ret,
                                uint16_t nhmsg_type, int nh_family,
                                unsigned char nh_protocol) {
        struct nhmsg *nhm;
        int r;

        assert_return(rtnl_message_type_is_nexthop(nhmsg_type), -EINVAL);
        assert_return((nhmsg_type == RTM_GETNEXTHOP && nh_family == AF_UNSPEC) ||
                      IN_SET(nh_family, AF_INET, AF_INET6), -EINVAL);
        assert_return(ret, -EINVAL);

        r = message_new(rtnl, ret, nhmsg_type);
        if (r < 0)
                return r;

        if (nhmsg_type == RTM_NEWNEXTHOP)
                (*ret)->hdr->nlmsg_flags |= NLM_F_CREATE | NLM_F_APPEND;

        nhm = NLMSG_DATA((*ret)->hdr);

        nhm->nh_family = nh_family;
        nhm->nh_scope = RT_SCOPE_UNIVERSE;
        nhm->nh_protocol = nh_protocol;

        return 0;
}

int sd_rtnl_message_nexthop_set_flags(sd_netlink_message *m, uint8_t flags) {
        struct nhmsg *nhm;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_nexthop(m->hdr->nlmsg_type), -EINVAL);

        nhm = NLMSG_DATA(m->hdr);
        nhm->nh_flags |= flags;

        return 0;
}

int sd_rtnl_message_nexthop_set_family(sd_netlink_message *m, uint8_t family) {
        struct nhmsg *nhm;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);

        nhm = NLMSG_DATA(m->hdr);
        nhm->nh_family = family;

        return 0;
}

int sd_rtnl_message_nexthop_get_family(const sd_netlink_message *m, uint8_t *family) {
        struct nhmsg *nhm;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);

        nhm = NLMSG_DATA(m->hdr);
        *family = nhm->nh_family ;

        return 0;
}

int sd_rtnl_message_neigh_set_flags(sd_netlink_message *m, uint8_t flags) {
        struct ndmsg *ndm;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_neigh(m->hdr->nlmsg_type), -EINVAL);

        ndm = NLMSG_DATA(m->hdr);
        ndm->ndm_flags |= flags;

        return 0;
}

int sd_rtnl_message_neigh_set_state(sd_netlink_message *m, uint16_t state) {
        struct ndmsg *ndm;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_neigh(m->hdr->nlmsg_type), -EINVAL);

        ndm = NLMSG_DATA(m->hdr);
        ndm->ndm_state |= state;

        return 0;
}

int sd_rtnl_message_neigh_get_flags(const sd_netlink_message *m, uint8_t *flags) {
        struct ndmsg *ndm;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_neigh(m->hdr->nlmsg_type), -EINVAL);

        ndm = NLMSG_DATA(m->hdr);
        *flags = ndm->ndm_flags;

        return 0;
}

int sd_rtnl_message_neigh_get_state(const sd_netlink_message *m, uint16_t *state) {
        struct ndmsg *ndm;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_neigh(m->hdr->nlmsg_type), -EINVAL);

        ndm = NLMSG_DATA(m->hdr);
        *state = ndm->ndm_state;

        return 0;
}

int sd_rtnl_message_neigh_get_family(const sd_netlink_message *m, int *family) {
        struct ndmsg *ndm;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_neigh(m->hdr->nlmsg_type), -EINVAL);
        assert_return(family, -EINVAL);

        ndm = NLMSG_DATA(m->hdr);

        *family = ndm->ndm_family;

        return 0;
}

int sd_rtnl_message_neigh_get_ifindex(const sd_netlink_message *m, int *index) {
        struct ndmsg *ndm;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_neigh(m->hdr->nlmsg_type), -EINVAL);
        assert_return(index, -EINVAL);

        ndm = NLMSG_DATA(m->hdr);

        *index = ndm->ndm_ifindex;

        return 0;
}

int sd_rtnl_message_new_neigh(sd_netlink *rtnl, sd_netlink_message **ret, uint16_t nlmsg_type, int index, int ndm_family) {
        struct ndmsg *ndm;
        int r;

        assert_return(rtnl_message_type_is_neigh(nlmsg_type), -EINVAL);
        assert_return(IN_SET(ndm_family, AF_UNSPEC, AF_INET, AF_INET6, AF_BRIDGE), -EINVAL);
        assert_return(ret, -EINVAL);

        r = message_new(rtnl, ret, nlmsg_type);
        if (r < 0)
                return r;

        if (nlmsg_type == RTM_NEWNEIGH)
                (*ret)->hdr->nlmsg_flags |= NLM_F_CREATE | NLM_F_REPLACE;

        ndm = NLMSG_DATA((*ret)->hdr);

        ndm->ndm_family = ndm_family;
        ndm->ndm_ifindex = index;

        return 0;
}

int sd_rtnl_message_link_set_flags(sd_netlink_message *m, unsigned flags, unsigned change) {
        struct ifinfomsg *ifi;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_link(m->hdr->nlmsg_type), -EINVAL);
        assert_return(change, -EINVAL);

        ifi = NLMSG_DATA(m->hdr);

        ifi->ifi_flags = flags;
        ifi->ifi_change = change;

        return 0;
}

int sd_rtnl_message_link_set_type(sd_netlink_message *m, unsigned type) {
        struct ifinfomsg *ifi;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_link(m->hdr->nlmsg_type), -EINVAL);

        ifi = NLMSG_DATA(m->hdr);

        ifi->ifi_type = type;

        return 0;
}

int sd_rtnl_message_link_set_family(sd_netlink_message *m, unsigned family) {
        struct ifinfomsg *ifi;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_link(m->hdr->nlmsg_type), -EINVAL);

        ifi = NLMSG_DATA(m->hdr);

        ifi->ifi_family = family;

        return 0;
}

int sd_rtnl_message_new_link(sd_netlink *rtnl, sd_netlink_message **ret,
                             uint16_t nlmsg_type, int index) {
        struct ifinfomsg *ifi;
        int r;

        assert_return(rtnl_message_type_is_link(nlmsg_type), -EINVAL);
        assert_return(ret, -EINVAL);

        r = message_new(rtnl, ret, nlmsg_type);
        if (r < 0)
                return r;

        if (nlmsg_type == RTM_NEWLINK)
                (*ret)->hdr->nlmsg_flags |= NLM_F_CREATE | NLM_F_EXCL;
        else if (nlmsg_type == RTM_NEWLINKPROP)
                (*ret)->hdr->nlmsg_flags |= NLM_F_CREATE | NLM_F_EXCL | NLM_F_APPEND;

        ifi = NLMSG_DATA((*ret)->hdr);

        ifi->ifi_family = AF_UNSPEC;
        ifi->ifi_index = index;

        return 0;
}

int sd_rtnl_message_addr_set_prefixlen(sd_netlink_message *m, unsigned char prefixlen) {
        struct ifaddrmsg *ifa;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_addr(m->hdr->nlmsg_type), -EINVAL);

        ifa = NLMSG_DATA(m->hdr);

        if ((ifa->ifa_family == AF_INET && prefixlen > 32) ||
            (ifa->ifa_family == AF_INET6 && prefixlen > 128))
                return -ERANGE;

        ifa->ifa_prefixlen = prefixlen;

        return 0;
}

int sd_rtnl_message_addr_set_flags(sd_netlink_message *m, unsigned char flags) {
        struct ifaddrmsg *ifa;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_addr(m->hdr->nlmsg_type), -EINVAL);

        ifa = NLMSG_DATA(m->hdr);

        ifa->ifa_flags = flags;

        return 0;
}

int sd_rtnl_message_addr_set_scope(sd_netlink_message *m, unsigned char scope) {
        struct ifaddrmsg *ifa;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_addr(m->hdr->nlmsg_type), -EINVAL);

        ifa = NLMSG_DATA(m->hdr);

        ifa->ifa_scope = scope;

        return 0;
}

int sd_rtnl_message_addr_get_family(const sd_netlink_message *m, int *family) {
        struct ifaddrmsg *ifa;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_addr(m->hdr->nlmsg_type), -EINVAL);
        assert_return(family, -EINVAL);

        ifa = NLMSG_DATA(m->hdr);

        *family = ifa->ifa_family;

        return 0;
}

int sd_rtnl_message_addr_get_prefixlen(const sd_netlink_message *m, unsigned char *prefixlen) {
        struct ifaddrmsg *ifa;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_addr(m->hdr->nlmsg_type), -EINVAL);
        assert_return(prefixlen, -EINVAL);

        ifa = NLMSG_DATA(m->hdr);

        *prefixlen = ifa->ifa_prefixlen;

        return 0;
}

int sd_rtnl_message_addr_get_scope(const sd_netlink_message *m, unsigned char *scope) {
        struct ifaddrmsg *ifa;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_addr(m->hdr->nlmsg_type), -EINVAL);
        assert_return(scope, -EINVAL);

        ifa = NLMSG_DATA(m->hdr);

        *scope = ifa->ifa_scope;

        return 0;
}

int sd_rtnl_message_addr_get_flags(const sd_netlink_message *m, unsigned char *flags) {
        struct ifaddrmsg *ifa;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_addr(m->hdr->nlmsg_type), -EINVAL);
        assert_return(flags, -EINVAL);

        ifa = NLMSG_DATA(m->hdr);

        *flags = ifa->ifa_flags;

        return 0;
}

int sd_rtnl_message_addr_get_ifindex(const sd_netlink_message *m, int *ifindex) {
        struct ifaddrmsg *ifa;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_addr(m->hdr->nlmsg_type), -EINVAL);
        assert_return(ifindex, -EINVAL);

        ifa = NLMSG_DATA(m->hdr);

        *ifindex = ifa->ifa_index;

        return 0;
}

int sd_rtnl_message_new_addr(sd_netlink *rtnl, sd_netlink_message **ret,
                             uint16_t nlmsg_type, int index,
                             int family) {
        struct ifaddrmsg *ifa;
        int r;

        assert_return(rtnl_message_type_is_addr(nlmsg_type), -EINVAL);
        assert_return((nlmsg_type == RTM_GETADDR && index == 0) ||
                      index > 0, -EINVAL);
        assert_return((nlmsg_type == RTM_GETADDR && family == AF_UNSPEC) ||
                      IN_SET(family, AF_INET, AF_INET6), -EINVAL);
        assert_return(ret, -EINVAL);

        r = message_new(rtnl, ret, nlmsg_type);
        if (r < 0)
                return r;

        if (nlmsg_type == RTM_GETADDR)
                (*ret)->hdr->nlmsg_flags |= NLM_F_DUMP;

        ifa = NLMSG_DATA((*ret)->hdr);

        ifa->ifa_index = index;
        ifa->ifa_family = family;
        if (family == AF_INET)
                ifa->ifa_prefixlen = 32;
        else if (family == AF_INET6)
                ifa->ifa_prefixlen = 128;

        return 0;
}

int sd_rtnl_message_new_addr_update(sd_netlink *rtnl, sd_netlink_message **ret,
                             int index, int family) {
        int r;

        r = sd_rtnl_message_new_addr(rtnl, ret, RTM_NEWADDR, index, family);
        if (r < 0)
                return r;

        (*ret)->hdr->nlmsg_flags |= NLM_F_REPLACE;

        return 0;
}

int sd_rtnl_message_link_get_ifindex(const sd_netlink_message *m, int *ifindex) {
        struct ifinfomsg *ifi;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_link(m->hdr->nlmsg_type), -EINVAL);
        assert_return(ifindex, -EINVAL);

        ifi = NLMSG_DATA(m->hdr);

        *ifindex = ifi->ifi_index;

        return 0;
}

int sd_rtnl_message_link_get_flags(const sd_netlink_message *m, unsigned *flags) {
        struct ifinfomsg *ifi;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_link(m->hdr->nlmsg_type), -EINVAL);
        assert_return(flags, -EINVAL);

        ifi = NLMSG_DATA(m->hdr);

        *flags = ifi->ifi_flags;

        return 0;
}

int sd_rtnl_message_link_get_type(const sd_netlink_message *m, unsigned short *type) {
        struct ifinfomsg *ifi;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_link(m->hdr->nlmsg_type), -EINVAL);
        assert_return(type, -EINVAL);

        ifi = NLMSG_DATA(m->hdr);

        *type = ifi->ifi_type;

        return 0;
}

int sd_rtnl_message_get_family(const sd_netlink_message *m, int *family) {
        assert_return(m, -EINVAL);
        assert_return(family, -EINVAL);

        assert(m->hdr);

        if (rtnl_message_type_is_link(m->hdr->nlmsg_type)) {
                struct ifinfomsg *ifi;

                ifi = NLMSG_DATA(m->hdr);

                *family = ifi->ifi_family;

                return 0;
        } else if (rtnl_message_type_is_route(m->hdr->nlmsg_type)) {
                struct rtmsg *rtm;

                rtm = NLMSG_DATA(m->hdr);

                *family = rtm->rtm_family;

                return 0;
        } else if (rtnl_message_type_is_neigh(m->hdr->nlmsg_type)) {
                struct ndmsg *ndm;

                ndm = NLMSG_DATA(m->hdr);

                *family = ndm->ndm_family;

                return 0;
        } else if (rtnl_message_type_is_addr(m->hdr->nlmsg_type)) {
                struct ifaddrmsg *ifa;

                ifa = NLMSG_DATA(m->hdr);

                *family = ifa->ifa_family;

                return 0;
        } else if (rtnl_message_type_is_routing_policy_rule(m->hdr->nlmsg_type)) {
                struct rtmsg *rtm;

                rtm = NLMSG_DATA(m->hdr);

                *family = rtm->rtm_family;

                return 0;
        } else if (rtnl_message_type_is_nexthop(m->hdr->nlmsg_type)) {
                struct nhmsg *nhm;

                nhm = NLMSG_DATA(m->hdr);

                *family = nhm->nh_family;

                return 0;
        }

        return -EOPNOTSUPP;
}

int sd_rtnl_message_new_addrlabel(sd_netlink *rtnl, sd_netlink_message **ret, uint16_t nlmsg_type, int ifindex, int ifal_family) {
        struct ifaddrlblmsg *addrlabel;
        int r;

        assert_return(rtnl_message_type_is_addrlabel(nlmsg_type), -EINVAL);
        assert_return(ret, -EINVAL);

        r = message_new(rtnl, ret, nlmsg_type);
        if (r < 0)
                return r;

        if (nlmsg_type == RTM_NEWADDRLABEL)
                (*ret)->hdr->nlmsg_flags |= NLM_F_CREATE | NLM_F_EXCL;

        addrlabel = NLMSG_DATA((*ret)->hdr);

        addrlabel->ifal_family = ifal_family;
        addrlabel->ifal_index = ifindex;

        return 0;
}

int sd_rtnl_message_addrlabel_set_prefixlen(sd_netlink_message *m, unsigned char prefixlen) {
        struct ifaddrlblmsg *addrlabel;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_addrlabel(m->hdr->nlmsg_type), -EINVAL);

        addrlabel = NLMSG_DATA(m->hdr);

        if (prefixlen > 128)
                return -ERANGE;

        addrlabel->ifal_prefixlen = prefixlen;

        return 0;
}

int sd_rtnl_message_addrlabel_get_prefixlen(const sd_netlink_message *m, unsigned char *prefixlen) {
        struct ifaddrlblmsg *addrlabel;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_addrlabel(m->hdr->nlmsg_type), -EINVAL);

        addrlabel = NLMSG_DATA(m->hdr);

        *prefixlen = addrlabel->ifal_prefixlen;

        return 0;
}

int sd_rtnl_message_new_routing_policy_rule(sd_netlink *rtnl, sd_netlink_message **ret, uint16_t nlmsg_type, int ifal_family) {
        struct rtmsg *rtm;
        int r;

        assert_return(rtnl_message_type_is_routing_policy_rule(nlmsg_type), -EINVAL);
        assert_return(ret, -EINVAL);

        r = message_new(rtnl, ret, nlmsg_type);
        if (r < 0)
                return r;

        if (nlmsg_type == RTM_NEWRULE)
                (*ret)->hdr->nlmsg_flags |= NLM_F_CREATE | NLM_F_EXCL;

        rtm = NLMSG_DATA((*ret)->hdr);
        rtm->rtm_family = ifal_family;
        rtm->rtm_protocol = RTPROT_BOOT;
        rtm->rtm_scope = RT_SCOPE_UNIVERSE;
        rtm->rtm_type = RTN_UNICAST;

        return 0;
}

int sd_rtnl_message_routing_policy_rule_set_tos(sd_netlink_message *m, unsigned char tos) {
        struct rtmsg *routing_policy_rule;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_routing_policy_rule(m->hdr->nlmsg_type), -EINVAL);

        routing_policy_rule = NLMSG_DATA(m->hdr);

        routing_policy_rule->rtm_tos = tos;

        return 0;
}

int sd_rtnl_message_routing_policy_rule_get_tos(const sd_netlink_message *m, unsigned char *tos) {
        struct rtmsg *routing_policy_rule;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_routing_policy_rule(m->hdr->nlmsg_type), -EINVAL);

        routing_policy_rule = NLMSG_DATA(m->hdr);

        *tos = routing_policy_rule->rtm_tos;

        return 0;
}

int sd_rtnl_message_routing_policy_rule_set_table(sd_netlink_message *m, unsigned char table) {
        struct rtmsg *routing_policy_rule;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_routing_policy_rule(m->hdr->nlmsg_type), -EINVAL);

        routing_policy_rule = NLMSG_DATA(m->hdr);

        routing_policy_rule->rtm_table = table;

        return 0;
}

int sd_rtnl_message_routing_policy_rule_get_table(const sd_netlink_message *m, unsigned char *table) {
        struct rtmsg *routing_policy_rule;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_routing_policy_rule(m->hdr->nlmsg_type), -EINVAL);

        routing_policy_rule = NLMSG_DATA(m->hdr);

        *table = routing_policy_rule->rtm_table;

        return 0;
}

int sd_rtnl_message_routing_policy_rule_set_flags(sd_netlink_message *m, unsigned flags) {
        struct rtmsg *routing_policy_rule;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_routing_policy_rule(m->hdr->nlmsg_type), -EINVAL);

        routing_policy_rule = NLMSG_DATA(m->hdr);
        routing_policy_rule->rtm_flags |= flags;

        return 0;
}

int sd_rtnl_message_routing_policy_rule_get_flags(const sd_netlink_message *m, unsigned *flags) {
        struct rtmsg *routing_policy_rule;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_routing_policy_rule(m->hdr->nlmsg_type), -EINVAL);

        routing_policy_rule = NLMSG_DATA(m->hdr);
        *flags = routing_policy_rule->rtm_flags;

        return 0;
}

int sd_rtnl_message_routing_policy_rule_set_rtm_type(sd_netlink_message *m, unsigned char type) {
        struct rtmsg *routing_policy_rule;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_routing_policy_rule(m->hdr->nlmsg_type), -EINVAL);

        routing_policy_rule = NLMSG_DATA(m->hdr);

        routing_policy_rule->rtm_type = type;

        return 0;
}

int sd_rtnl_message_routing_policy_rule_get_rtm_type(const sd_netlink_message *m, unsigned char *type) {
        struct rtmsg *routing_policy_rule;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_routing_policy_rule(m->hdr->nlmsg_type), -EINVAL);

        routing_policy_rule = NLMSG_DATA(m->hdr);

        *type =  routing_policy_rule->rtm_type;

        return 0;
}

int sd_rtnl_message_routing_policy_rule_set_rtm_dst_prefixlen(sd_netlink_message *m, unsigned char len) {
        struct rtmsg *routing_policy_rule;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_routing_policy_rule(m->hdr->nlmsg_type), -EINVAL);

        routing_policy_rule = NLMSG_DATA(m->hdr);

        routing_policy_rule->rtm_dst_len = len;

        return 0;
}

int sd_rtnl_message_routing_policy_rule_get_rtm_dst_prefixlen(const sd_netlink_message *m, unsigned char *len) {
        struct rtmsg *routing_policy_rule;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_routing_policy_rule(m->hdr->nlmsg_type), -EINVAL);

        routing_policy_rule = NLMSG_DATA(m->hdr);

        *len = routing_policy_rule->rtm_dst_len;

        return 0;
}

int sd_rtnl_message_routing_policy_rule_set_rtm_src_prefixlen(sd_netlink_message *m, unsigned char len) {
        struct rtmsg *routing_policy_rule;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_routing_policy_rule(m->hdr->nlmsg_type), -EINVAL);

        routing_policy_rule = NLMSG_DATA(m->hdr);

        routing_policy_rule->rtm_src_len = len;

        return 0;
}

int sd_rtnl_message_routing_policy_rule_get_rtm_src_prefixlen(const sd_netlink_message *m, unsigned char *len) {
        struct rtmsg *routing_policy_rule;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(rtnl_message_type_is_routing_policy_rule(m->hdr->nlmsg_type), -EINVAL);

        routing_policy_rule = NLMSG_DATA(m->hdr);

        *len = routing_policy_rule->rtm_src_len;

        return 0;
}

int sd_rtnl_message_new_qdisc(sd_netlink *rtnl, sd_netlink_message **ret, uint16_t nlmsg_type, int tcm_family, int tcm_ifindex) {
        struct tcmsg *tcm;
        int r;

        assert_return(rtnl_message_type_is_qdisc(nlmsg_type), -EINVAL);
        assert_return(ret, -EINVAL);

        r = message_new(rtnl, ret, nlmsg_type);
        if (r < 0)
                return r;

        if (nlmsg_type == RTM_NEWQDISC)
                (*ret)->hdr->nlmsg_flags |= NLM_F_CREATE | NLM_F_EXCL;

        tcm = NLMSG_DATA((*ret)->hdr);
        tcm->tcm_family = tcm_family;
        tcm->tcm_ifindex = tcm_ifindex;

        return 0;
}

int sd_rtnl_message_set_qdisc_parent(sd_netlink_message *m, uint32_t parent) {
        struct tcmsg *tcm;

        assert_return(rtnl_message_type_is_qdisc(m->hdr->nlmsg_type), -EINVAL);

        tcm = NLMSG_DATA(m->hdr);
        tcm->tcm_parent = parent;

        return 0;
}

int sd_rtnl_message_set_qdisc_handle(sd_netlink_message *m, uint32_t handle) {
        struct tcmsg *tcm;

        assert_return(rtnl_message_type_is_qdisc(m->hdr->nlmsg_type), -EINVAL);

        tcm = NLMSG_DATA(m->hdr);
        tcm->tcm_handle = handle;

        return 0;
}

int sd_rtnl_message_new_tclass(sd_netlink *rtnl, sd_netlink_message **ret, uint16_t nlmsg_type, int tcm_family, int tcm_ifindex) {
        struct tcmsg *tcm;
        int r;

        assert_return(rtnl_message_type_is_tclass(nlmsg_type), -EINVAL);
        assert_return(ret, -EINVAL);

        r = message_new(rtnl, ret, nlmsg_type);
        if (r < 0)
                return r;

        if (nlmsg_type == RTM_NEWTCLASS)
                (*ret)->hdr->nlmsg_flags |= NLM_F_CREATE | NLM_F_EXCL;

        tcm = NLMSG_DATA((*ret)->hdr);
        tcm->tcm_family = tcm_family;
        tcm->tcm_ifindex = tcm_ifindex;

        return 0;
}

int sd_rtnl_message_set_tclass_parent(sd_netlink_message *m, uint32_t parent) {
        struct tcmsg *tcm;

        assert_return(rtnl_message_type_is_tclass(m->hdr->nlmsg_type), -EINVAL);

        tcm = NLMSG_DATA(m->hdr);
        tcm->tcm_parent = parent;

        return 0;
}

int sd_rtnl_message_set_tclass_handle(sd_netlink_message *m, uint32_t handle) {
        struct tcmsg *tcm;

        assert_return(rtnl_message_type_is_tclass(m->hdr->nlmsg_type), -EINVAL);

        tcm = NLMSG_DATA(m->hdr);
        tcm->tcm_handle = handle;

        return 0;
}

int sd_rtnl_message_new_mdb(sd_netlink *rtnl, sd_netlink_message **ret, uint16_t nlmsg_type, int mdb_ifindex) {
        struct br_port_msg *bpm;
        int r;

        assert_return(rtnl_message_type_is_mdb(nlmsg_type), -EINVAL);
        assert_return(ret, -EINVAL);

        r = message_new(rtnl, ret, nlmsg_type);
        if (r < 0)
                return r;

        if (nlmsg_type == RTM_NEWMDB)
                (*ret)->hdr->nlmsg_flags |= NLM_F_CREATE | NLM_F_EXCL;

        bpm = NLMSG_DATA((*ret)->hdr);
        bpm->family = AF_BRIDGE;
        bpm->ifindex = mdb_ifindex;

        return 0;
}
