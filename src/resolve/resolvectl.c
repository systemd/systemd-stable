/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <getopt.h>
#include <locale.h>
#include <net/if.h>

#include "sd-bus.h"
#include "sd-netlink.h"

#include "af-list.h"
#include "alloc-util.h"
#include "bus-common-errors.h"
#include "bus-error.h"
#include "bus-locator.h"
#include "bus-map-properties.h"
#include "bus-message-util.h"
#include "dns-domain.h"
#include "escape.h"
#include "format-table.h"
#include "format-util.h"
#include "gcrypt-util.h"
#include "hostname-util.h"
#include "main-func.h"
#include "missing_network.h"
#include "netlink-util.h"
#include "pager.h"
#include "parse-argument.h"
#include "parse-util.h"
#include "pretty-print.h"
#include "process-util.h"
#include "resolvconf-compat.h"
#include "resolvectl.h"
#include "resolved-def.h"
#include "resolved-dns-packet.h"
#include "resolved-util.h"
#include "socket-netlink.h"
#include "sort-util.h"
#include "stdio-util.h"
#include "string-table.h"
#include "strv.h"
#include "terminal-util.h"
#include "utf8.h"
#include "verbs.h"

static int arg_family = AF_UNSPEC;
static int arg_ifindex = 0;
static char *arg_ifname = NULL;
static uint16_t arg_type = 0;
static uint16_t arg_class = 0;
static bool arg_legend = true;
static uint64_t arg_flags = 0;
static PagerFlags arg_pager_flags = 0;
bool arg_ifindex_permissive = false; /* If true, don't generate an error if the specified interface index doesn't exist */
static const char *arg_service_family = NULL;

typedef enum RawType {
        RAW_NONE,
        RAW_PAYLOAD,
        RAW_PACKET,
} RawType;
static RawType arg_raw = RAW_NONE;

ExecutionMode arg_mode = MODE_RESOLVE_HOST;

char **arg_set_dns = NULL;
char **arg_set_domain = NULL;
static const char *arg_set_llmnr = NULL;
static const char *arg_set_mdns = NULL;
static const char *arg_set_dns_over_tls = NULL;
static const char *arg_set_dnssec = NULL;
static char **arg_set_nta = NULL;

STATIC_DESTRUCTOR_REGISTER(arg_ifname, freep);
STATIC_DESTRUCTOR_REGISTER(arg_set_dns, strv_freep);
STATIC_DESTRUCTOR_REGISTER(arg_set_domain, strv_freep);
STATIC_DESTRUCTOR_REGISTER(arg_set_nta, strv_freep);

typedef enum StatusMode {
        STATUS_ALL,
        STATUS_DNS,
        STATUS_DOMAIN,
        STATUS_DEFAULT_ROUTE,
        STATUS_LLMNR,
        STATUS_MDNS,
        STATUS_PRIVATE,
        STATUS_DNSSEC,
        STATUS_NTA,
} StatusMode;

typedef struct InterfaceInfo {
        int index;
        const char *name;
} InterfaceInfo;

static int interface_info_compare(const InterfaceInfo *a, const InterfaceInfo *b) {
        int r;

        r = CMP(a->index, b->index);
        if (r != 0)
                return r;

        return strcmp_ptr(a->name, b->name);
}

int ifname_mangle(const char *s) {
        _cleanup_free_ char *iface = NULL;
        int ifi;

        assert(s);

        iface = strdup(s);
        if (!iface)
                return log_oom();

        ifi = rtnl_resolve_interface(NULL, iface);
        if (ifi < 0) {
                if (ifi == -ENODEV && arg_ifindex_permissive) {
                        log_debug("Interface '%s' not found, but -f specified, ignoring.", iface);
                        return 0; /* done */
                }

                return log_error_errno(ifi, "Failed to resolve interface \"%s\": %m", iface);
        }

        if (arg_ifindex > 0 && arg_ifindex != ifi)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Specified multiple different interfaces. Refusing.");

        arg_ifindex = ifi;
        free_and_replace(arg_ifname, iface);

        return 1;
}

int ifname_resolvconf_mangle(const char *s) {
        const char *dot;

        assert(s);

        dot = strchr(s, '.');
        if (dot) {
                _cleanup_free_ char *iface = NULL;

                log_debug("Ignoring protocol specifier '%s'.", dot + 1);
                iface = strndup(s, dot - s);
                if (!iface)
                        return log_oom();
                return ifname_mangle(iface);
        } else
                return ifname_mangle(s);
}

static void print_source(uint64_t flags, usec_t rtt) {
        char rtt_str[FORMAT_TIMESTAMP_MAX];

        if (!arg_legend)
                return;

        if (flags == 0)
                return;

        printf("\n%s-- Information acquired via", ansi_grey());

        printf(" protocol%s%s%s%s%s",
               flags & SD_RESOLVED_DNS ? " DNS" :"",
               flags & SD_RESOLVED_LLMNR_IPV4 ? " LLMNR/IPv4" : "",
               flags & SD_RESOLVED_LLMNR_IPV6 ? " LLMNR/IPv6" : "",
               flags & SD_RESOLVED_MDNS_IPV4 ? " mDNS/IPv4" : "",
               flags & SD_RESOLVED_MDNS_IPV6 ? " mDNS/IPv6" : "");

        assert_se(format_timespan(rtt_str, sizeof(rtt_str), rtt, 100));

        printf(" in %s.%s\n"
               "%s-- Data is authenticated: %s; Data was acquired via local or encrypted transport: %s%s\n",
               rtt_str, ansi_normal(),
               ansi_grey(),
               yes_no(flags & SD_RESOLVED_AUTHENTICATED),
               yes_no(flags & SD_RESOLVED_CONFIDENTIAL),
               ansi_normal());

        if ((flags & (SD_RESOLVED_FROM_MASK|SD_RESOLVED_SYNTHETIC)) != 0)
                printf("%s-- Data from:%s%s%s%s%s%s\n",
                       ansi_grey(),
                       FLAGS_SET(flags, SD_RESOLVED_SYNTHETIC) ? " synthetic" : "",
                       FLAGS_SET(flags, SD_RESOLVED_FROM_CACHE) ? " cache" : "",
                       FLAGS_SET(flags, SD_RESOLVED_FROM_ZONE) ? " zone" : "",
                       FLAGS_SET(flags, SD_RESOLVED_FROM_TRUST_ANCHOR) ? " trust-anchor" : "",
                       FLAGS_SET(flags, SD_RESOLVED_FROM_NETWORK) ? " network" : "",
                       ansi_normal());
}

static void print_ifindex_comment(int printed_so_far, int ifindex) {
        char ifname[IF_NAMESIZE + 1];

        if (ifindex <= 0)
                return;

        if (!format_ifname(ifindex, ifname))
                log_warning_errno(errno, "Failed to resolve interface name for index %i, ignoring: %m", ifindex);
        else
                printf("%*s%s-- link: %s%s",
                       60 > printed_so_far ? 60 - printed_so_far : 0, " ", /* Align comment to the 60th column */
                       ansi_grey(), ifname, ansi_normal());
}

static int resolve_host(sd_bus *bus, const char *name) {
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *req = NULL, *reply = NULL;
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        const char *canonical = NULL;
        unsigned c = 0;
        uint64_t flags;
        usec_t ts;
        int r;

        assert(name);

        log_debug("Resolving %s (family %s, interface %s).", name, af_to_name(arg_family) ?: "*", isempty(arg_ifname) ? "*" : arg_ifname);

        r = bus_message_new_method_call(bus, &req, bus_resolve_mgr, "ResolveHostname");
        if (r < 0)
                return bus_log_create_error(r);

        r = sd_bus_message_append(req, "isit", arg_ifindex, name, arg_family, arg_flags);
        if (r < 0)
                return bus_log_create_error(r);

        ts = now(CLOCK_MONOTONIC);

        r = sd_bus_call(bus, req, SD_RESOLVED_QUERY_TIMEOUT_USEC, &error, &reply);
        if (r < 0)
                return log_error_errno(r, "%s: resolve call failed: %s", name, bus_error_message(&error, r));

        ts = now(CLOCK_MONOTONIC) - ts;

        r = sd_bus_message_enter_container(reply, 'a', "(iiay)");
        if (r < 0)
                return bus_log_parse_error(r);

        while ((r = sd_bus_message_enter_container(reply, 'r', "iiay")) > 0) {
                _cleanup_free_ char *pretty = NULL;
                int ifindex, family, k;
                union in_addr_union a;

                assert_cc(sizeof(int) == sizeof(int32_t));

                r = sd_bus_message_read(reply, "i", &ifindex);
                if (r < 0)
                        return bus_log_parse_error(r);

                sd_bus_error_free(&error);
                r = bus_message_read_in_addr_auto(reply, &error, &family, &a);
                if (r < 0 && !sd_bus_error_has_name(&error, SD_BUS_ERROR_INVALID_ARGS))
                        return log_error_errno(r, "%s: systemd-resolved returned invalid result: %s", name, bus_error_message(&error, r));

                r = sd_bus_message_exit_container(reply);
                if (r < 0)
                        return bus_log_parse_error(r);

                if (sd_bus_error_has_name(&error, SD_BUS_ERROR_INVALID_ARGS)) {
                        log_debug_errno(r, "%s: systemd-resolved returned invalid result, ignoring: %s", name, bus_error_message(&error, r));
                        continue;
                }

                r = in_addr_ifindex_to_string(family, &a, ifindex, &pretty);
                if (r < 0)
                        return log_error_errno(r, "Failed to print address for %s: %m", name);

                k = printf("%*s%s %s%s%s",
                           (int) strlen(name), c == 0 ? name : "", c == 0 ? ":" : " ",
                           ansi_highlight(), pretty, ansi_normal());

                print_ifindex_comment(k, ifindex);
                fputc('\n', stdout);

                c++;
        }
        if (r < 0)
                return bus_log_parse_error(r);

        r = sd_bus_message_exit_container(reply);
        if (r < 0)
                return bus_log_parse_error(r);

        r = sd_bus_message_read(reply, "st", &canonical, &flags);
        if (r < 0)
                return bus_log_parse_error(r);

        if (!streq(name, canonical))
                printf("%*s%s (%s)\n",
                       (int) strlen(name), c == 0 ? name : "", c == 0 ? ":" : " ",
                       canonical);

        if (c == 0)
                return log_error_errno(SYNTHETIC_ERRNO(ESRCH),
                                       "%s: no addresses found", name);

        print_source(flags, ts);

        return 0;
}

static int resolve_address(sd_bus *bus, int family, const union in_addr_union *address, int ifindex) {
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *req = NULL, *reply = NULL;
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_free_ char *pretty = NULL;
        uint64_t flags;
        unsigned c = 0;
        usec_t ts;
        int r;

        assert(bus);
        assert(IN_SET(family, AF_INET, AF_INET6));
        assert(address);

        if (ifindex <= 0)
                ifindex = arg_ifindex;

        r = in_addr_ifindex_to_string(family, address, ifindex, &pretty);
        if (r < 0)
                return log_oom();

        log_debug("Resolving %s.", pretty);

        r = bus_message_new_method_call(bus, &req, bus_resolve_mgr, "ResolveAddress");
        if (r < 0)
                return bus_log_create_error(r);

        r = sd_bus_message_append(req, "ii", ifindex, family);
        if (r < 0)
                return bus_log_create_error(r);

        r = sd_bus_message_append_array(req, 'y', address, FAMILY_ADDRESS_SIZE(family));
        if (r < 0)
                return bus_log_create_error(r);

        r = sd_bus_message_append(req, "t", arg_flags);
        if (r < 0)
                return bus_log_create_error(r);

        ts = now(CLOCK_MONOTONIC);

        r = sd_bus_call(bus, req, SD_RESOLVED_QUERY_TIMEOUT_USEC, &error, &reply);
        if (r < 0)
                return log_error_errno(r, "%s: resolve call failed: %s", pretty, bus_error_message(&error, r));

        ts = now(CLOCK_MONOTONIC) - ts;

        r = sd_bus_message_enter_container(reply, 'a', "(is)");
        if (r < 0)
                return bus_log_create_error(r);

        while ((r = sd_bus_message_enter_container(reply, 'r', "is")) > 0) {
                const char *n;
                int k;

                assert_cc(sizeof(int) == sizeof(int32_t));

                r = sd_bus_message_read(reply, "is", &ifindex, &n);
                if (r < 0)
                        return r;

                r = sd_bus_message_exit_container(reply);
                if (r < 0)
                        return r;

                k = printf("%*s%s %s%s%s",
                           (int) strlen(pretty), c == 0 ? pretty : "",
                           c == 0 ? ":" : " ",
                           ansi_highlight(), n, ansi_normal());

                print_ifindex_comment(k, ifindex);
                fputc('\n', stdout);

                c++;
        }
        if (r < 0)
                return bus_log_parse_error(r);

        r = sd_bus_message_exit_container(reply);
        if (r < 0)
                return bus_log_parse_error(r);

        r = sd_bus_message_read(reply, "t", &flags);
        if (r < 0)
                return bus_log_parse_error(r);

        if (c == 0)
                return log_error_errno(SYNTHETIC_ERRNO(ESRCH),
                                       "%s: no names found", pretty);

        print_source(flags, ts);

        return 0;
}

static int output_rr_packet(const void *d, size_t l, int ifindex) {
        _cleanup_(dns_resource_record_unrefp) DnsResourceRecord *rr = NULL;
        _cleanup_(dns_packet_unrefp) DnsPacket *p = NULL;
        int r;

        r = dns_packet_new(&p, DNS_PROTOCOL_DNS, 0, DNS_PACKET_SIZE_MAX);
        if (r < 0)
                return log_oom();

        p->refuse_compression = true;

        r = dns_packet_append_blob(p, d, l, NULL);
        if (r < 0)
                return log_oom();

        r = dns_packet_read_rr(p, &rr, NULL, NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to parse RR: %m");

        if (arg_raw == RAW_PAYLOAD) {
                void *data;
                ssize_t k;

                k = dns_resource_record_payload(rr, &data);
                if (k < 0)
                        return log_error_errno(k, "Cannot dump RR: %m");
                fwrite(data, 1, k, stdout);
        } else {
                const char *s;
                int k;

                s = dns_resource_record_to_string(rr);
                if (!s)
                        return log_oom();

                k = printf("%s", s);
                print_ifindex_comment(k, ifindex);
                fputc('\n', stdout);
        }

        return 0;
}

static int idna_candidate(const char *name, char **ret) {
        _cleanup_free_ char *idnafied = NULL;
        int r;

        assert(name);
        assert(ret);

        r = dns_name_apply_idna(name, &idnafied);
        if (r < 0)
                return log_error_errno(r, "Failed to apply IDNA to name '%s': %m", name);
        if (r > 0 && !streq(name, idnafied)) {
                *ret = TAKE_PTR(idnafied);
                return true;
        }

        *ret = NULL;
        return false;
}

static bool single_label_nonsynthetic(const char *name) {
        _cleanup_free_ char *first_label = NULL;
        int r;

        if (!dns_name_is_single_label(name))
                return false;

        if (is_localhost(name) || is_gateway_hostname(name))
                return false;

        r = resolve_system_hostname(NULL, &first_label);
        if (r < 0) {
                log_warning_errno(r, "Failed to determine the hostname: %m");
                return false;
        }

        return !streq(name, first_label);
}

static int resolve_record(sd_bus *bus, const char *name, uint16_t class, uint16_t type, bool warn_missing) {
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *req = NULL, *reply = NULL;
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_free_ char *idnafied = NULL;
        bool needs_authentication = false;
        unsigned n = 0;
        uint64_t flags;
        usec_t ts;
        int r;

        assert(name);

        log_debug("Resolving %s %s %s (interface %s).", name, dns_class_to_string(class), dns_type_to_string(type), isempty(arg_ifname) ? "*" : arg_ifname);

        if (dns_name_dot_suffixed(name) == 0 && single_label_nonsynthetic(name))
                log_notice("(Note that search domains are not appended when --type= is specified. "
                           "Please specify fully qualified domain names, or remove --type= switch from invocation in order to request regular hostname resolution.)");

        r = idna_candidate(name, &idnafied);
        if (r < 0)
                return r;
        if (r > 0)
                log_notice("(Note that IDNA translation is not applied when --type= is specified. "
                           "Please specify translated domain names — i.e. '%s' — when resolving raw records, or remove --type= switch from invocation in order to request regular hostname resolution.",
                           idnafied);

        r = bus_message_new_method_call(bus, &req, bus_resolve_mgr, "ResolveRecord");
        if (r < 0)
                return bus_log_create_error(r);

        r = sd_bus_message_append(req, "isqqt", arg_ifindex, name, class, type, arg_flags);
        if (r < 0)
                return bus_log_create_error(r);

        ts = now(CLOCK_MONOTONIC);

        r = sd_bus_call(bus, req, SD_RESOLVED_QUERY_TIMEOUT_USEC, &error, &reply);
        if (r < 0) {
                if (warn_missing || r != -ENXIO)
                        log_error("%s: resolve call failed: %s", name, bus_error_message(&error, r));
                return r;
        }

        ts = now(CLOCK_MONOTONIC) - ts;

        r = sd_bus_message_enter_container(reply, 'a', "(iqqay)");
        if (r < 0)
                return bus_log_parse_error(r);

        while ((r = sd_bus_message_enter_container(reply, 'r', "iqqay")) > 0) {
                uint16_t c, t;
                int ifindex;
                const void *d;
                size_t l;

                assert_cc(sizeof(int) == sizeof(int32_t));

                r = sd_bus_message_read(reply, "iqq", &ifindex, &c, &t);
                if (r < 0)
                        return bus_log_parse_error(r);

                r = sd_bus_message_read_array(reply, 'y', &d, &l);
                if (r < 0)
                        return bus_log_parse_error(r);

                r = sd_bus_message_exit_container(reply);
                if (r < 0)
                        return bus_log_parse_error(r);

                if (arg_raw == RAW_PACKET) {
                        uint64_t u64 = htole64(l);

                        fwrite(&u64, sizeof(u64), 1, stdout);
                        fwrite(d, 1, l, stdout);
                } else {
                        r = output_rr_packet(d, l, ifindex);
                        if (r < 0)
                                return r;
                }

                if (dns_type_needs_authentication(t))
                        needs_authentication = true;

                n++;
        }
        if (r < 0)
                return bus_log_parse_error(r);

        r = sd_bus_message_exit_container(reply);
        if (r < 0)
                return bus_log_parse_error(r);

        r = sd_bus_message_read(reply, "t", &flags);
        if (r < 0)
                return bus_log_parse_error(r);

        if (n == 0) {
                if (warn_missing)
                        log_error("%s: no records found", name);
                return -ESRCH;
        }

        print_source(flags, ts);

        if ((flags & SD_RESOLVED_AUTHENTICATED) == 0 && needs_authentication) {
                fflush(stdout);

                fprintf(stderr, "\n%s"
                       "WARNING: The resources shown contain cryptographic key data which could not be\n"
                       "         authenticated. It is not suitable to authenticate any communication.\n"
                       "         This is usually indication that DNSSEC authentication was not enabled\n"
                       "         or is not available for the selected protocol or DNS servers.%s\n",
                       ansi_highlight_red(),
                       ansi_normal());
        }

        return 0;
}

static int resolve_rfc4501(sd_bus *bus, const char *name) {
        uint16_t type = 0, class = 0;
        const char *p, *q, *n;
        int r;

        assert(bus);
        assert(name);
        assert(startswith(name, "dns:"));

        /* Parse RFC 4501 dns: URIs */

        p = name + 4;

        if (p[0] == '/') {
                const char *e;

                if (p[1] != '/')
                        goto invalid;

                e = strchr(p + 2, '/');
                if (!e)
                        goto invalid;

                if (e != p + 2)
                        log_warning("DNS authority specification not supported; ignoring specified authority.");

                p = e + 1;
        }

        q = strchr(p, '?');
        if (q) {
                n = strndupa(p, q - p);
                q++;

                for (;;) {
                        const char *f;

                        f = startswith_no_case(q, "class=");
                        if (f) {
                                _cleanup_free_ char *t = NULL;
                                const char *e;

                                if (class != 0)
                                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                                               "DNS class specified twice.");

                                e = strchrnul(f, ';');
                                t = strndup(f, e - f);
                                if (!t)
                                        return log_oom();

                                r = dns_class_from_string(t);
                                if (r < 0)
                                        return log_error_errno(r, "Unknown DNS class %s.", t);

                                class = r;

                                if (*e == ';') {
                                        q = e + 1;
                                        continue;
                                }

                                break;
                        }

                        f = startswith_no_case(q, "type=");
                        if (f) {
                                _cleanup_free_ char *t = NULL;
                                const char *e;

                                if (type != 0)
                                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                                               "DNS type specified twice.");

                                e = strchrnul(f, ';');
                                t = strndup(f, e - f);
                                if (!t)
                                        return log_oom();

                                r = dns_type_from_string(t);
                                if (r < 0)
                                        return log_error_errno(r, "Unknown DNS type %s: %m", t);

                                type = r;

                                if (*e == ';') {
                                        q = e + 1;
                                        continue;
                                }

                                break;
                        }

                        goto invalid;
                }
        } else
                n = p;

        if (class == 0)
                class = arg_class ?: DNS_CLASS_IN;
        if (type == 0)
                type = arg_type ?: DNS_TYPE_A;

        return resolve_record(bus, n, class, type, true);

invalid:
        return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                               "Invalid DNS URI: %s", name);
}

static int verb_query(int argc, char **argv, void *userdata) {
        sd_bus *bus = userdata;
        char **p;
        int q, r = 0;

        if (arg_type != 0)
                STRV_FOREACH(p, argv + 1) {
                        q = resolve_record(bus, *p, arg_class, arg_type, true);
                        if (q < 0)
                                r = q;
                }

        else
                STRV_FOREACH(p, argv + 1) {
                        if (startswith(*p, "dns:"))
                                q = resolve_rfc4501(bus, *p);
                        else {
                                int family, ifindex;
                                union in_addr_union a;

                                q = in_addr_ifindex_from_string_auto(*p, &family, &a, &ifindex);
                                if (q >= 0)
                                        q = resolve_address(bus, family, &a, ifindex);
                                else
                                        q = resolve_host(bus, *p);
                        }
                        if (q < 0)
                                r = q;
                }

        return r;
}

static int resolve_service(sd_bus *bus, const char *name, const char *type, const char *domain) {
        const char *canonical_name, *canonical_type, *canonical_domain;
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *req = NULL, *reply = NULL;
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        size_t indent, sz;
        uint64_t flags;
        const char *p;
        unsigned c;
        usec_t ts;
        int r;

        assert(bus);
        assert(domain);

        name = empty_to_null(name);
        type = empty_to_null(type);

        if (name)
                log_debug("Resolving service \"%s\" of type %s in %s (family %s, interface %s).", name, type, domain, af_to_name(arg_family) ?: "*", isempty(arg_ifname) ? "*" : arg_ifname);
        else if (type)
                log_debug("Resolving service type %s of %s (family %s, interface %s).", type, domain, af_to_name(arg_family) ?: "*", isempty(arg_ifname) ? "*" : arg_ifname);
        else
                log_debug("Resolving service type %s (family %s, interface %s).", domain, af_to_name(arg_family) ?: "*", isempty(arg_ifname) ? "*" : arg_ifname);

        r = bus_message_new_method_call(bus, &req, bus_resolve_mgr, "ResolveService");
        if (r < 0)
                return bus_log_create_error(r);

        r = sd_bus_message_append(req, "isssit", arg_ifindex, name, type, domain, arg_family, arg_flags);
        if (r < 0)
                return bus_log_create_error(r);

        ts = now(CLOCK_MONOTONIC);

        r = sd_bus_call(bus, req, SD_RESOLVED_QUERY_TIMEOUT_USEC, &error, &reply);
        if (r < 0)
                return log_error_errno(r, "Resolve call failed: %s", bus_error_message(&error, r));

        ts = now(CLOCK_MONOTONIC) - ts;

        r = sd_bus_message_enter_container(reply, 'a', "(qqqsa(iiay)s)");
        if (r < 0)
                return bus_log_parse_error(r);

        indent =
                (name ? strlen(name) + 1 : 0) +
                (type ? strlen(type) + 1 : 0) +
                strlen(domain) + 2;

        c = 0;
        while ((r = sd_bus_message_enter_container(reply, 'r', "qqqsa(iiay)s")) > 0) {
                uint16_t priority, weight, port;
                const char *hostname, *canonical;

                r = sd_bus_message_read(reply, "qqqs", &priority, &weight, &port, &hostname);
                if (r < 0)
                        return bus_log_parse_error(r);

                if (name)
                        printf("%*s%s", (int) strlen(name), c == 0 ? name : "", c == 0 ? "/" : " ");
                if (type)
                        printf("%*s%s", (int) strlen(type), c == 0 ? type : "", c == 0 ? "/" : " ");

                printf("%*s%s %s:%u [priority=%u, weight=%u]\n",
                       (int) strlen(domain), c == 0 ? domain : "",
                       c == 0 ? ":" : " ",
                       hostname, port,
                       priority, weight);

                r = sd_bus_message_enter_container(reply, 'a', "(iiay)");
                if (r < 0)
                        return bus_log_parse_error(r);

                while ((r = sd_bus_message_enter_container(reply, 'r', "iiay")) > 0) {
                        _cleanup_free_ char *pretty = NULL;
                        int ifindex, family, k;
                        union in_addr_union a;;

                        assert_cc(sizeof(int) == sizeof(int32_t));

                        r = sd_bus_message_read(reply, "i", &ifindex);
                        if (r < 0)
                                return bus_log_parse_error(r);

                        sd_bus_error_free(&error);
                        r = bus_message_read_in_addr_auto(reply, &error, &family, &a);
                        if (r < 0 && !sd_bus_error_has_name(&error, SD_BUS_ERROR_INVALID_ARGS))
                                return log_error_errno(r, "%s: systemd-resolved returned invalid result: %s", name, bus_error_message(&error, r));

                        r = sd_bus_message_exit_container(reply);
                        if (r < 0)
                                return bus_log_parse_error(r);

                        if (sd_bus_error_has_name(&error, SD_BUS_ERROR_INVALID_ARGS)) {
                                log_debug_errno(r, "%s: systemd-resolved returned invalid result, ignoring: %s", name, bus_error_message(&error, r));
                                continue;
                        }

                        r = in_addr_ifindex_to_string(family, &a, ifindex, &pretty);
                        if (r < 0)
                                return log_error_errno(r, "Failed to print address for %s: %m", name);

                        k = printf("%*s%s", (int) indent, "", pretty);
                        print_ifindex_comment(k, ifindex);
                        fputc('\n', stdout);
                }
                if (r < 0)
                        return bus_log_parse_error(r);

                r = sd_bus_message_exit_container(reply);
                if (r < 0)
                        return bus_log_parse_error(r);

                r = sd_bus_message_read(reply, "s", &canonical);
                if (r < 0)
                        return bus_log_parse_error(r);

                if (!streq(hostname, canonical))
                        printf("%*s(%s)\n", (int) indent, "", canonical);

                r = sd_bus_message_exit_container(reply);
                if (r < 0)
                        return bus_log_parse_error(r);

                c++;
        }
        if (r < 0)
                return bus_log_parse_error(r);

        r = sd_bus_message_exit_container(reply);
        if (r < 0)
                return bus_log_parse_error(r);

        r = sd_bus_message_enter_container(reply, 'a', "ay");
        if (r < 0)
                return bus_log_parse_error(r);

        while ((r = sd_bus_message_read_array(reply, 'y', (const void**) &p, &sz)) > 0) {
                _cleanup_free_ char *escaped = NULL;

                escaped = cescape_length(p, sz);
                if (!escaped)
                        return log_oom();

                printf("%*s%s\n", (int) indent, "", escaped);
        }
        if (r < 0)
                return bus_log_parse_error(r);

        r = sd_bus_message_exit_container(reply);
        if (r < 0)
                return bus_log_parse_error(r);

        r = sd_bus_message_read(reply, "ssst", &canonical_name, &canonical_type, &canonical_domain, &flags);
        if (r < 0)
                return bus_log_parse_error(r);

        canonical_name = empty_to_null(canonical_name);
        canonical_type = empty_to_null(canonical_type);

        if (!streq_ptr(name, canonical_name) ||
            !streq_ptr(type, canonical_type) ||
            !streq_ptr(domain, canonical_domain)) {

                printf("%*s(", (int) indent, "");

                if (canonical_name)
                        printf("%s/", canonical_name);
                if (canonical_type)
                        printf("%s/", canonical_type);

                printf("%s)\n", canonical_domain);
        }

        print_source(flags, ts);

        return 0;
}

static int verb_service(int argc, char **argv, void *userdata) {
        sd_bus *bus = userdata;

        if (argc == 2)
                return resolve_service(bus, NULL, NULL, argv[1]);
        else if (argc == 3)
                return resolve_service(bus, NULL, argv[1], argv[2]);
        else
                return resolve_service(bus, argv[1], argv[2], argv[3]);
}

static int resolve_openpgp(sd_bus *bus, const char *address) {
        const char *domain, *full;
        int r;
        _cleanup_free_ char *hashed = NULL;

        assert(bus);
        assert(address);

        domain = strrchr(address, '@');
        if (!domain)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "Address does not contain '@': \"%s\"", address);
        if (domain == address || domain[1] == '\0')
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "Address starts or ends with '@': \"%s\"", address);
        domain++;

        r = string_hashsum_sha256(address, domain - 1 - address, &hashed);
        if (r < 0)
                return log_error_errno(r, "Hashing failed: %m");

        strshorten(hashed, 56);

        full = strjoina(hashed, "._openpgpkey.", domain);
        log_debug("Looking up \"%s\".", full);

        r = resolve_record(bus, full,
                           arg_class ?: DNS_CLASS_IN,
                           arg_type ?: DNS_TYPE_OPENPGPKEY, false);

        if (IN_SET(r, -ENXIO, -ESRCH)) { /* NXDOMAIN or NODATA? */
              hashed = mfree(hashed);
              r = string_hashsum_sha224(address, domain - 1 - address, &hashed);
              if (r < 0)
                    return log_error_errno(r, "Hashing failed: %m");

              full = strjoina(hashed, "._openpgpkey.", domain);
              log_debug("Looking up \"%s\".", full);

              return resolve_record(bus, full,
                                    arg_class ?: DNS_CLASS_IN,
                                    arg_type ?: DNS_TYPE_OPENPGPKEY, true);
        }

        return r;
}

static int verb_openpgp(int argc, char **argv, void *userdata) {
        sd_bus *bus = userdata;
        char **p;
        int q, r = 0;

        STRV_FOREACH(p, argv + 1) {
                q = resolve_openpgp(bus, *p);
                if (q < 0)
                        r = q;
        }

        return r;
}

static int resolve_tlsa(sd_bus *bus, const char *family, const char *address) {
        const char *port;
        uint16_t port_num = 443;
        _cleanup_free_ char *full = NULL;
        int r;

        assert(bus);
        assert(address);

        port = strrchr(address, ':');
        if (port) {
                r = parse_ip_port(port + 1, &port_num);
                if (r < 0)
                        return log_error_errno(r, "Invalid port \"%s\".", port + 1);

                address = strndupa(address, port - address);
        }

        r = asprintf(&full, "_%u._%s.%s",
                     port_num,
                     family,
                     address);
        if (r < 0)
                return log_oom();

        log_debug("Looking up \"%s\".", full);

        return resolve_record(bus, full,
                              arg_class ?: DNS_CLASS_IN,
                              arg_type ?: DNS_TYPE_TLSA, true);
}

static bool service_family_is_valid(const char *s) {
        return STR_IN_SET(s, "tcp", "udp", "sctp");
}

static int verb_tlsa(int argc, char **argv, void *userdata) {
        sd_bus *bus = userdata;
        char **p, **args = argv + 1;
        const char *family = "tcp";
        int q, r = 0;

        if (service_family_is_valid(argv[1])) {
                family = argv[1];
                args++;
        }

        STRV_FOREACH(p, args) {
                q = resolve_tlsa(bus, family, *p);
                if (q < 0)
                        r = q;
        }

        return r;
}

static int show_statistics(int argc, char **argv, void *userdata) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
        _cleanup_(table_unrefp) Table *table = NULL;
        sd_bus *bus = userdata;
        uint64_t n_current_transactions, n_total_transactions,
                cache_size, n_cache_hit, n_cache_miss,
                n_dnssec_secure, n_dnssec_insecure, n_dnssec_bogus, n_dnssec_indeterminate;
        int r, dnssec_supported;

        assert(bus);

        r = bus_get_property_trivial(bus, bus_resolve_mgr, "DNSSECSupported", &error, 'b', &dnssec_supported);
        if (r < 0)
                return log_error_errno(r, "Failed to get DNSSEC supported state: %s", bus_error_message(&error, r));

        printf("DNSSEC supported by current servers: %s%s%s\n\n",
               ansi_highlight(),
               yes_no(dnssec_supported),
               ansi_normal());

        r = bus_get_property(bus, bus_resolve_mgr, "TransactionStatistics", &error, &reply, "(tt)");
        if (r < 0)
                return log_error_errno(r, "Failed to get transaction statistics: %s", bus_error_message(&error, r));

        r = sd_bus_message_read(reply, "(tt)",
                                &n_current_transactions,
                                &n_total_transactions);
        if (r < 0)
                return bus_log_parse_error(r);

        reply = sd_bus_message_unref(reply);

        r = bus_get_property(bus, bus_resolve_mgr, "CacheStatistics", &error, &reply, "(ttt)");
        if (r < 0)
                return log_error_errno(r, "Failed to get cache statistics: %s", bus_error_message(&error, r));

        r = sd_bus_message_read(reply, "(ttt)",
                                &cache_size,
                                &n_cache_hit,
                                &n_cache_miss);
        if (r < 0)
                return bus_log_parse_error(r);

        reply = sd_bus_message_unref(reply);

        r = bus_get_property(bus, bus_resolve_mgr, "DNSSECStatistics", &error, &reply, "(tttt)");
        if (r < 0)
                return log_error_errno(r, "Failed to get DNSSEC statistics: %s", bus_error_message(&error, r));

        r = sd_bus_message_read(reply, "(tttt)",
                                &n_dnssec_secure,
                                &n_dnssec_insecure,
                                &n_dnssec_bogus,
                                &n_dnssec_indeterminate);
        if (r < 0)
                return bus_log_parse_error(r);

        table = table_new("key", "value");
        if (!table)
                return log_oom();

        table_set_header(table, false);

        r = table_add_many(table,
                           TABLE_STRING, "Transactions",
                           TABLE_SET_COLOR, ansi_highlight(),
                           TABLE_EMPTY,
                           TABLE_STRING, "Current Transactions:",
                           TABLE_SET_ALIGN_PERCENT, 100,
                           TABLE_UINT64, n_current_transactions,
                           TABLE_STRING, "Total Transactions:",
                           TABLE_UINT64, n_total_transactions,
                           TABLE_EMPTY, TABLE_EMPTY,
                           TABLE_STRING, "Cache",
                           TABLE_SET_COLOR, ansi_highlight(),
                           TABLE_SET_ALIGN_PERCENT, 0,
                           TABLE_EMPTY,
                           TABLE_STRING, "Current Cache Size:",
                           TABLE_SET_ALIGN_PERCENT, 100,
                           TABLE_UINT64, cache_size,
                           TABLE_STRING, "Cache Hits:",
                           TABLE_UINT64, n_cache_hit,
                           TABLE_STRING, "Cache Misses:",
                           TABLE_UINT64, n_cache_miss,
                           TABLE_EMPTY, TABLE_EMPTY,
                           TABLE_STRING, "DNSSEC Verdicts",
                           TABLE_SET_COLOR, ansi_highlight(),
                           TABLE_SET_ALIGN_PERCENT, 0,
                           TABLE_EMPTY,
                           TABLE_STRING, "Secure:",
                           TABLE_SET_ALIGN_PERCENT, 100,
                           TABLE_UINT64, n_dnssec_secure,
                           TABLE_STRING, "Insecure:",
                           TABLE_UINT64, n_dnssec_insecure,
                           TABLE_STRING, "Bogus:",
                           TABLE_UINT64, n_dnssec_bogus,
                           TABLE_STRING, "Indeterminate:",
                           TABLE_UINT64, n_dnssec_indeterminate);
        if (r < 0)
                table_log_add_error(r);

        r = table_print(table, NULL);
        if (r < 0)
                return table_log_print_error(r);

        return 0;
}

static int reset_statistics(int argc, char **argv, void *userdata) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus *bus = userdata;
        int r;

        r = bus_call_method(bus, bus_resolve_mgr, "ResetStatistics", &error, NULL, NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to reset statistics: %s", bus_error_message(&error, r));

        return 0;
}

static int flush_caches(int argc, char **argv, void *userdata) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus *bus = userdata;
        int r;

        r = bus_call_method(bus, bus_resolve_mgr, "FlushCaches", &error, NULL, NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to flush caches: %s", bus_error_message(&error, r));

        return 0;
}

static int reset_server_features(int argc, char **argv, void *userdata) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus *bus = userdata;
        int r;

        r = bus_call_method(bus, bus_resolve_mgr, "ResetServerFeatures", &error, NULL, NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to reset server features: %s", bus_error_message(&error, r));

        return 0;
}

static int read_dns_server_one(
                sd_bus_message *m,
                bool with_ifindex,  /* read "ifindex" reply that also carries an interface index */
                bool extended,      /* read "extended" reply, i.e. with port number and server name */
                bool only_global,   /* suppress entries with an (non-loopback) ifindex set (i.e. which are specific to some interface) */
                char **ret) {

        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_free_ char *pretty = NULL;
        union in_addr_union a;
        const char *name = NULL;
        int32_t ifindex = 0;
        int family, r, k;
        uint16_t port = 0;

        assert(m);
        assert(ret);

        r = sd_bus_message_enter_container(m, 'r', with_ifindex ? (extended ? "iiayqs" : "iiay") : (extended ? "iayqs" : "iay"));
        if (r <= 0)
                return r;

        if (with_ifindex) {
                r = sd_bus_message_read(m, "i", &ifindex);
                if (r < 0)
                        return r;
        }

        k = bus_message_read_in_addr_auto(m, &error, &family, &a);
        if (k < 0 && !sd_bus_error_has_name(&error, SD_BUS_ERROR_INVALID_ARGS))
                return k;

        if (extended) {
                r = sd_bus_message_read(m, "q", &port);
                if (r < 0)
                        return r;

                r = sd_bus_message_read(m, "s", &name);
                if (r < 0)
                        return r;
        }

        r = sd_bus_message_exit_container(m);
        if (r < 0)
                return r;

        if (k < 0) {
                log_debug("Invalid DNS server, ignoring: %s", bus_error_message(&error, k));
                *ret = NULL;
                return 1;
        }

        if (only_global && ifindex > 0 && ifindex != LOOPBACK_IFINDEX) {
                /* This one has an (non-loopback) ifindex set, and we were told to suppress those. Hence do so. */
                *ret = NULL;
                return 1;
        }

        r = in_addr_port_ifindex_name_to_string(family, &a, port, ifindex, name, &pretty);
        if (r < 0)
                return r;

        *ret = TAKE_PTR(pretty);

        return 1;
}

static int map_link_dns_servers_internal(sd_bus *bus, const char *member, sd_bus_message *m, sd_bus_error *error, void *userdata, bool extended) {
        char ***l = userdata;
        int r;

        assert(bus);
        assert(member);
        assert(m);
        assert(l);

        r = sd_bus_message_enter_container(m, 'a', extended ? "(iayqs)" : "(iay)");
        if (r < 0)
                return r;

        for (;;) {
                _cleanup_free_ char *pretty = NULL;

                r = read_dns_server_one(m, /* with_ifindex= */ false, extended, /* only_global= */ false, &pretty);
                if (r < 0)
                        return r;
                if (r == 0)
                        break;

                if (isempty(pretty))
                        continue;

                r = strv_consume(l, TAKE_PTR(pretty));
                if (r < 0)
                        return r;
        }

        r = sd_bus_message_exit_container(m);
        if (r < 0)
                return r;

        return 0;
}

static int map_link_dns_servers(sd_bus *bus, const char *member, sd_bus_message *m, sd_bus_error *error, void *userdata) {
        return map_link_dns_servers_internal(bus, member, m, error, userdata, false);
}

static int map_link_dns_servers_ex(sd_bus *bus, const char *member, sd_bus_message *m, sd_bus_error *error, void *userdata) {
        return map_link_dns_servers_internal(bus, member, m, error, userdata, true);
}

static int map_link_current_dns_server(sd_bus *bus, const char *member, sd_bus_message *m, sd_bus_error *error, void *userdata) {
        assert(m);
        assert(userdata);

        return read_dns_server_one(m, /* with_ifindex= */ false, /* extended= */ false, /* only_global= */ false, userdata);
}

static int map_link_current_dns_server_ex(sd_bus *bus, const char *member, sd_bus_message *m, sd_bus_error *error, void *userdata) {
        assert(m);
        assert(userdata);

        return read_dns_server_one(m, /* with_ifindex= */ false, /* extended= */ true, /* only_global= */ false, userdata);
}

static int read_domain_one(sd_bus_message *m, bool with_ifindex, char **ret) {
        _cleanup_free_ char *str = NULL;
        int ifindex, route_only, r;
        const char *domain;

        assert(m);
        assert(ret);

        if (with_ifindex)
                r = sd_bus_message_read(m, "(isb)", &ifindex, &domain, &route_only);
        else
                r = sd_bus_message_read(m, "(sb)", &domain, &route_only);
        if (r <= 0)
                return r;

        if (with_ifindex && ifindex != 0) {
                /* only show the global ones here */
                *ret = NULL;
                return 1;
        }

        if (route_only)
                str = strjoin("~", domain);
        else
                str = strdup(domain);
        if (!str)
                return -ENOMEM;

        *ret = TAKE_PTR(str);

        return 1;
}

static int map_link_domains(sd_bus *bus, const char *member, sd_bus_message *m, sd_bus_error *error, void *userdata) {
        char ***l = userdata;
        int r;

        assert(bus);
        assert(member);
        assert(m);
        assert(l);

        r = sd_bus_message_enter_container(m, 'a', "(sb)");
        if (r < 0)
                return r;

        for (;;) {
                _cleanup_free_ char *pretty = NULL;

                r = read_domain_one(m, false, &pretty);
                if (r < 0)
                        return r;
                if (r == 0)
                        break;

                if (isempty(pretty))
                        continue;

                r = strv_consume(l, TAKE_PTR(pretty));
                if (r < 0)
                        return r;
        }

        r = sd_bus_message_exit_container(m);
        if (r < 0)
                return r;

        strv_sort(*l);

        return 0;
}

static int status_print_strv_ifindex(int ifindex, const char *ifname, char **p) {
        const unsigned indent = strlen("Global: "); /* Use the same indentation everywhere to make things nice */
        int pos1, pos2;

        if (ifname)
                printf("%s%nLink %i (%s)%n%s:", ansi_highlight(), &pos1, ifindex, ifname, &pos2, ansi_normal());
        else
                printf("%s%nGlobal%n%s:", ansi_highlight(), &pos1, &pos2, ansi_normal());

        size_t cols = columns(), position = pos2 - pos1 + 2;
        char **i;

        STRV_FOREACH(i, p) {
                size_t our_len = utf8_console_width(*i); /* This returns -1 on invalid utf-8 (which shouldn't happen).
                                                          * If that happens, we'll just print one item per line. */

                if (position <= indent || size_add(size_add(position, 1), our_len) < cols) {
                        printf(" %s", *i);
                        position = size_add(size_add(position, 1), our_len);
                } else {
                        printf("\n%*s%s", indent, "", *i);
                        position = size_add(our_len, indent);
                }
        }

        printf("\n");

        return 0;
}

static int status_print_strv_global(char **p) {
        return status_print_strv_ifindex(0, NULL, p);
}

typedef struct LinkInfo {
        uint64_t scopes_mask;
        const char *llmnr;
        const char *mdns;
        const char *dns_over_tls;
        const char *dnssec;
        char *current_dns;
        char *current_dns_ex;
        char **dns;
        char **dns_ex;
        char **domains;
        char **ntas;
        bool dnssec_supported;
        bool default_route;
} LinkInfo;

typedef struct GlobalInfo {
        char *current_dns;
        char *current_dns_ex;
        char **dns;
        char **dns_ex;
        char **fallback_dns;
        char **fallback_dns_ex;
        char **domains;
        char **ntas;
        const char *llmnr;
        const char *mdns;
        const char *dns_over_tls;
        const char *dnssec;
        const char *resolv_conf_mode;
        bool dnssec_supported;
} GlobalInfo;

static void link_info_clear(LinkInfo *p) {
        free(p->current_dns);
        free(p->current_dns_ex);
        strv_free(p->dns);
        strv_free(p->dns_ex);
        strv_free(p->domains);
        strv_free(p->ntas);
}

static void global_info_clear(GlobalInfo *p) {
        free(p->current_dns);
        free(p->current_dns_ex);
        strv_free(p->dns);
        strv_free(p->dns_ex);
        strv_free(p->fallback_dns);
        strv_free(p->fallback_dns_ex);
        strv_free(p->domains);
        strv_free(p->ntas);
}

static int dump_list(Table *table, const char *prefix, char * const *l) {
        int r;

        if (strv_isempty(l))
                return 0;

        r = table_add_many(table,
                           TABLE_STRING, prefix,
                           TABLE_STRV_WRAPPED, l);
        if (r < 0)
                return table_log_add_error(r);

        return 0;
}

static int strv_extend_extended_bool(char ***strv, const char *name, const char *value) {
        int r;

        if (value) {
                r = parse_boolean(value);
                if (r >= 0)
                        return strv_extendf(strv, "%s%s", plus_minus(r), name);
        }

        return strv_extendf(strv, "%s=%s", name, value ?: "???");
}

static char** link_protocol_status(const LinkInfo *info) {
        _cleanup_strv_free_ char **s = NULL;

        if (strv_extendf(&s, "%sDefaultRoute", plus_minus(info->default_route)) < 0)
                return NULL;

        if (strv_extend_extended_bool(&s, "LLMNR", info->llmnr) < 0)
                return NULL;

        if (strv_extend_extended_bool(&s, "mDNS", info->mdns) < 0)
                return NULL;

        if (strv_extend_extended_bool(&s, "DNSOverTLS", info->dns_over_tls) < 0)
                return NULL;

        if (strv_extendf(&s, "DNSSEC=%s/%s",
                         info->dnssec ?: "???",
                         info->dnssec_supported ? "supported" : "unsupported") < 0)
                return NULL;

        return TAKE_PTR(s);
}

static char** global_protocol_status(const GlobalInfo *info) {
        _cleanup_strv_free_ char **s = NULL;

        if (strv_extend_extended_bool(&s, "LLMNR", info->llmnr) < 0)
                return NULL;

        if (strv_extend_extended_bool(&s, "mDNS", info->mdns) < 0)
                return NULL;

        if (strv_extend_extended_bool(&s, "DNSOverTLS", info->dns_over_tls) < 0)
                return NULL;

        if (strv_extendf(&s, "DNSSEC=%s/%s",
                         info->dnssec ?: "???",
                         info->dnssec_supported ? "supported" : "unsupported") < 0)
                return NULL;

        return TAKE_PTR(s);
}

static int status_ifindex(sd_bus *bus, int ifindex, const char *name, StatusMode mode, bool *empty_line) {
        static const struct bus_properties_map property_map[] = {
                { "ScopesMask",                 "t",        NULL,                           offsetof(LinkInfo, scopes_mask)      },
                { "DNS",                        "a(iay)",   map_link_dns_servers,           offsetof(LinkInfo, dns)              },
                { "DNSEx",                      "a(iayqs)", map_link_dns_servers_ex,        offsetof(LinkInfo, dns_ex)           },
                { "CurrentDNSServer",           "(iay)",    map_link_current_dns_server,    offsetof(LinkInfo, current_dns)      },
                { "CurrentDNSServerEx",         "(iayqs)",  map_link_current_dns_server_ex, offsetof(LinkInfo, current_dns_ex)   },
                { "Domains",                    "a(sb)",    map_link_domains,               offsetof(LinkInfo, domains)          },
                { "DefaultRoute",               "b",        NULL,                           offsetof(LinkInfo, default_route)    },
                { "LLMNR",                      "s",        NULL,                           offsetof(LinkInfo, llmnr)            },
                { "MulticastDNS",               "s",        NULL,                           offsetof(LinkInfo, mdns)             },
                { "DNSOverTLS",                 "s",        NULL,                           offsetof(LinkInfo, dns_over_tls)     },
                { "DNSSEC",                     "s",        NULL,                           offsetof(LinkInfo, dnssec)           },
                { "DNSSECNegativeTrustAnchors", "as",       bus_map_strv_sort,              offsetof(LinkInfo, ntas)             },
                { "DNSSECSupported",            "b",        NULL,                           offsetof(LinkInfo, dnssec_supported) },
                {}
        };
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *m = NULL;
        _cleanup_(link_info_clear) LinkInfo link_info = {};
        _cleanup_(table_unrefp) Table *table = NULL;
        _cleanup_free_ char *p = NULL;
        char ifi[DECIMAL_STR_MAX(int)], ifname[IF_NAMESIZE + 1] = "";
        int r;

        assert(bus);
        assert(ifindex > 0);

        if (!name) {
                if (!format_ifname(ifindex, ifname))
                        return log_error_errno(errno, "Failed to resolve interface name for %i: %m", ifindex);

                name = ifname;
        }

        xsprintf(ifi, "%i", ifindex);
        r = sd_bus_path_encode("/org/freedesktop/resolve1/link", ifi, &p);
        if (r < 0)
                return log_oom();

        r = bus_map_all_properties(bus,
                                   "org.freedesktop.resolve1",
                                   p,
                                   property_map,
                                   BUS_MAP_BOOLEAN_AS_BOOL,
                                   &error,
                                   &m,
                                   &link_info);
        if (r < 0)
                return log_error_errno(r, "Failed to get link data for %i: %s", ifindex, bus_error_message(&error, r));

        (void) pager_open(arg_pager_flags);

        if (mode == STATUS_DNS)
                return status_print_strv_ifindex(ifindex, name, link_info.dns_ex ?: link_info.dns);

        if (mode == STATUS_DOMAIN)
                return status_print_strv_ifindex(ifindex, name, link_info.domains);

        if (mode == STATUS_NTA)
                return status_print_strv_ifindex(ifindex, name, link_info.ntas);

        if (mode == STATUS_DEFAULT_ROUTE) {
                printf("%sLink %i (%s)%s: %s\n",
                       ansi_highlight(), ifindex, name, ansi_normal(),
                       yes_no(link_info.default_route));

                return 0;
        }

        if (mode == STATUS_LLMNR) {
                printf("%sLink %i (%s)%s: %s\n",
                       ansi_highlight(), ifindex, name, ansi_normal(),
                       strna(link_info.llmnr));

                return 0;
        }

        if (mode == STATUS_MDNS) {
                printf("%sLink %i (%s)%s: %s\n",
                       ansi_highlight(), ifindex, name, ansi_normal(),
                       strna(link_info.mdns));

                return 0;
        }

        if (mode == STATUS_PRIVATE) {
                printf("%sLink %i (%s)%s: %s\n",
                       ansi_highlight(), ifindex, name, ansi_normal(),
                       strna(link_info.dns_over_tls));

                return 0;
        }

        if (mode == STATUS_DNSSEC) {
                printf("%sLink %i (%s)%s: %s\n",
                       ansi_highlight(), ifindex, name, ansi_normal(),
                       strna(link_info.dnssec));

                return 0;
        }

        if (empty_line && *empty_line)
                fputc('\n', stdout);

        printf("%sLink %i (%s)%s\n",
               ansi_highlight(), ifindex, name, ansi_normal());

        table = table_new("key", "value");
        if (!table)
                return log_oom();

        table_set_header(table, false);

        r = table_add_many(table,
                           TABLE_STRING, "Current Scopes:",
                           TABLE_SET_ALIGN_PERCENT, 100);
        if (r < 0)
                return table_log_add_error(r);

        if (link_info.scopes_mask == 0)
                r = table_add_cell(table, NULL, TABLE_STRING, "none");
        else {
                _cleanup_free_ char *buf = NULL;
                size_t len;

                if (asprintf(&buf, "%s%s%s%s%s",
                             link_info.scopes_mask & SD_RESOLVED_DNS ? "DNS " : "",
                             link_info.scopes_mask & SD_RESOLVED_LLMNR_IPV4 ? "LLMNR/IPv4 " : "",
                             link_info.scopes_mask & SD_RESOLVED_LLMNR_IPV6 ? "LLMNR/IPv6 " : "",
                             link_info.scopes_mask & SD_RESOLVED_MDNS_IPV4 ? "mDNS/IPv4 " : "",
                             link_info.scopes_mask & SD_RESOLVED_MDNS_IPV6 ? "mDNS/IPv6 " : "") < 0)
                        return log_oom();

                len = strlen(buf);
                assert(len > 0);
                buf[len - 1] = '\0';

                r = table_add_cell(table, NULL, TABLE_STRING, buf);
        }
        if (r < 0)
                return table_log_add_error(r);

        _cleanup_strv_free_ char **pstatus = link_protocol_status(&link_info);
        if (!pstatus)
                return log_oom();

        r = table_add_many(table,
                           TABLE_STRING,       "Protocols:",
                           TABLE_STRV_WRAPPED, pstatus);
        if (r < 0)
                return table_log_add_error(r);

        if (link_info.current_dns) {
                r = table_add_many(table,
                                   TABLE_STRING, "Current DNS Server:",
                                   TABLE_STRING, link_info.current_dns_ex ?: link_info.current_dns);
                if (r < 0)
                        return table_log_add_error(r);
        }

        r = dump_list(table, "DNS Servers:", link_info.dns_ex ?: link_info.dns);
        if (r < 0)
                return r;

        r = dump_list(table, "DNS Domain:", link_info.domains);
        if (r < 0)
                return r;

        r = table_print(table, NULL);
        if (r < 0)
                return table_log_print_error(r);

        if (empty_line)
                *empty_line = true;

        return 0;
}

static int map_global_dns_servers_internal(sd_bus *bus, const char *member, sd_bus_message *m, sd_bus_error *error, void *userdata, bool extended) {
        char ***l = userdata;
        int r;

        assert(bus);
        assert(member);
        assert(m);
        assert(l);

        r = sd_bus_message_enter_container(m, 'a', extended ? "(iiayqs)" : "(iiay)");
        if (r < 0)
                return r;

        for (;;) {
                _cleanup_free_ char *pretty = NULL;

                r = read_dns_server_one(m, /* with_ifindex= */ true, extended, /* only_global= */ true, &pretty);
                if (r < 0)
                        return r;
                if (r == 0)
                        break;

                if (isempty(pretty))
                        continue;

                r = strv_consume(l, TAKE_PTR(pretty));
                if (r < 0)
                        return r;
        }

        r = sd_bus_message_exit_container(m);
        if (r < 0)
                return r;

        return 0;
}

static int map_global_dns_servers(sd_bus *bus, const char *member, sd_bus_message *m, sd_bus_error *error, void *userdata) {
        return map_global_dns_servers_internal(bus, member, m, error, userdata, false);
}

static int map_global_dns_servers_ex(sd_bus *bus, const char *member, sd_bus_message *m, sd_bus_error *error, void *userdata) {
        return map_global_dns_servers_internal(bus, member, m, error, userdata, true);
}

static int map_global_current_dns_server(sd_bus *bus, const char *member, sd_bus_message *m, sd_bus_error *error, void *userdata) {
        return read_dns_server_one(m, /* with_ifindex= */ true, /* extended= */ false, /* only_global= */ true, userdata);
}

static int map_global_current_dns_server_ex(sd_bus *bus, const char *member, sd_bus_message *m, sd_bus_error *error, void *userdata) {
        return read_dns_server_one(m, /* with_ifindex= */ true, /* extended= */ true, /* only_global= */ true, userdata);
}

static int map_global_domains(sd_bus *bus, const char *member, sd_bus_message *m, sd_bus_error *error, void *userdata) {
        char ***l = userdata;
        int r;

        assert(bus);
        assert(member);
        assert(m);
        assert(l);

        r = sd_bus_message_enter_container(m, 'a', "(isb)");
        if (r < 0)
                return r;

        for (;;) {
                _cleanup_free_ char *pretty = NULL;

                r = read_domain_one(m, true, &pretty);
                if (r < 0)
                        return r;
                if (r == 0)
                        break;

                if (isempty(pretty))
                        continue;

                r = strv_consume(l, TAKE_PTR(pretty));
                if (r < 0)
                        return r;
        }

        r = sd_bus_message_exit_container(m);
        if (r < 0)
                return r;

        strv_sort(*l);

        return 0;
}

static int status_global(sd_bus *bus, StatusMode mode, bool *empty_line) {
        static const struct bus_properties_map property_map[] = {
                { "DNS",                        "a(iiay)",   map_global_dns_servers,           offsetof(GlobalInfo, dns)              },
                { "DNSEx",                      "a(iiayqs)", map_global_dns_servers_ex,        offsetof(GlobalInfo, dns_ex)           },
                { "FallbackDNS",                "a(iiay)",   map_global_dns_servers,           offsetof(GlobalInfo, fallback_dns)     },
                { "FallbackDNSEx",              "a(iiayqs)", map_global_dns_servers_ex,        offsetof(GlobalInfo, fallback_dns_ex)  },
                { "CurrentDNSServer",           "(iiay)",    map_global_current_dns_server,    offsetof(GlobalInfo, current_dns)      },
                { "CurrentDNSServerEx",         "(iiayqs)",  map_global_current_dns_server_ex, offsetof(GlobalInfo, current_dns_ex)   },
                { "Domains",                    "a(isb)",    map_global_domains,               offsetof(GlobalInfo, domains)          },
                { "DNSSECNegativeTrustAnchors", "as",        bus_map_strv_sort,                offsetof(GlobalInfo, ntas)             },
                { "LLMNR",                      "s",         NULL,                             offsetof(GlobalInfo, llmnr)            },
                { "MulticastDNS",               "s",         NULL,                             offsetof(GlobalInfo, mdns)             },
                { "DNSOverTLS",                 "s",         NULL,                             offsetof(GlobalInfo, dns_over_tls)     },
                { "DNSSEC",                     "s",         NULL,                             offsetof(GlobalInfo, dnssec)           },
                { "DNSSECSupported",            "b",         NULL,                             offsetof(GlobalInfo, dnssec_supported) },
                { "ResolvConfMode",             "s",         NULL,                             offsetof(GlobalInfo, resolv_conf_mode) },
                {}
        };
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *m = NULL;
        _cleanup_(global_info_clear) GlobalInfo global_info = {};
        _cleanup_(table_unrefp) Table *table = NULL;
        int r;

        assert(bus);
        assert(empty_line);

        r = bus_map_all_properties(bus,
                                   "org.freedesktop.resolve1",
                                   "/org/freedesktop/resolve1",
                                   property_map,
                                   BUS_MAP_BOOLEAN_AS_BOOL,
                                   &error,
                                   &m,
                                   &global_info);
        if (r < 0)
                return log_error_errno(r, "Failed to get global data: %s", bus_error_message(&error, r));

        (void) pager_open(arg_pager_flags);

        if (mode == STATUS_DNS)
                return status_print_strv_global(global_info.dns_ex ?: global_info.dns);

        if (mode == STATUS_DOMAIN)
                return status_print_strv_global(global_info.domains);

        if (mode == STATUS_NTA)
                return status_print_strv_global(global_info.ntas);

        if (mode == STATUS_LLMNR) {
                printf("%sGlobal%s: %s\n", ansi_highlight(), ansi_normal(),
                       strna(global_info.llmnr));

                return 0;
        }

        if (mode == STATUS_MDNS) {
                printf("%sGlobal%s: %s\n", ansi_highlight(), ansi_normal(),
                       strna(global_info.mdns));

                return 0;
        }

        if (mode == STATUS_PRIVATE) {
                printf("%sGlobal%s: %s\n", ansi_highlight(), ansi_normal(),
                       strna(global_info.dns_over_tls));

                return 0;
        }

        if (mode == STATUS_DNSSEC) {
                printf("%sGlobal%s: %s\n", ansi_highlight(), ansi_normal(),
                       strna(global_info.dnssec));

                return 0;
        }

        printf("%sGlobal%s\n", ansi_highlight(), ansi_normal());

        table = table_new("key", "value");
        if (!table)
                return log_oom();

        table_set_header(table, false);

        _cleanup_strv_free_ char **pstatus = global_protocol_status(&global_info);
        if (!pstatus)
                return log_oom();

        r = table_add_many(table,
                           TABLE_STRING,            "Protocols:",
                           TABLE_SET_ALIGN_PERCENT, 100,
                           TABLE_STRV_WRAPPED,      pstatus);
        if (r < 0)
                return table_log_add_error(r);

        if (global_info.resolv_conf_mode) {
                r = table_add_many(table,
                                   TABLE_STRING, "resolv.conf mode:",
                                   TABLE_STRING, global_info.resolv_conf_mode);
                if (r < 0)
                        return table_log_add_error(r);
        }

        if (global_info.current_dns) {
                r = table_add_many(table,
                                   TABLE_STRING, "Current DNS Server:",
                                   TABLE_STRING, global_info.current_dns_ex ?: global_info.current_dns);
                if (r < 0)
                        return table_log_add_error(r);
        }

        r = dump_list(table, "DNS Servers", global_info.dns_ex ?: global_info.dns);
        if (r < 0)
                return r;

        r = dump_list(table, "Fallback DNS Servers", global_info.fallback_dns_ex ?: global_info.fallback_dns);
        if (r < 0)
                return r;

        r = dump_list(table, "DNS Domain", global_info.domains);
        if (r < 0)
                return r;

        r = table_print(table, NULL);
        if (r < 0)
                return table_log_print_error(r);

        *empty_line = true;

        return 0;
}

static int status_all(sd_bus *bus, StatusMode mode) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *req = NULL, *reply = NULL;
        _cleanup_(sd_netlink_unrefp) sd_netlink *rtnl = NULL;
        bool empty_line = false;
        int r;

        assert(bus);

        r = status_global(bus, mode, &empty_line);
        if (r < 0)
                return r;

        r = sd_netlink_open(&rtnl);
        if (r < 0)
                return log_error_errno(r, "Failed to connect to netlink: %m");

        r = sd_rtnl_message_new_link(rtnl, &req, RTM_GETLINK, 0);
        if (r < 0)
                return rtnl_log_create_error(r);

        r = sd_netlink_message_request_dump(req, true);
        if (r < 0)
                return rtnl_log_create_error(r);

        r = sd_netlink_call(rtnl, req, 0, &reply);
        if (r < 0)
                return log_error_errno(r, "Failed to enumerate links: %m");

        _cleanup_free_ InterfaceInfo *infos = NULL;
        size_t n_infos = 0;

        for (sd_netlink_message *i = reply; i; i = sd_netlink_message_next(i)) {
                const char *name;
                int ifindex;
                uint16_t type;

                r = sd_netlink_message_get_type(i, &type);
                if (r < 0)
                        return rtnl_log_parse_error(r);

                if (type != RTM_NEWLINK)
                        continue;

                r = sd_rtnl_message_link_get_ifindex(i, &ifindex);
                if (r < 0)
                        return rtnl_log_parse_error(r);

                if (ifindex == LOOPBACK_IFINDEX)
                        continue;

                r = sd_netlink_message_read_string(i, IFLA_IFNAME, &name);
                if (r < 0)
                        return rtnl_log_parse_error(r);

                if (!GREEDY_REALLOC(infos, n_infos + 1))
                        return log_oom();

                infos[n_infos++] = (InterfaceInfo) { ifindex, name };
        }

        typesafe_qsort(infos, n_infos, interface_info_compare);

        r = 0;
        for (size_t i = 0; i < n_infos; i++) {
                int q = status_ifindex(bus, infos[i].index, infos[i].name, mode, &empty_line);
                if (q < 0 && r >= 0)
                        r = q;
        }

        return r;
}

static int verb_status(int argc, char **argv, void *userdata) {
        sd_bus *bus = userdata;
        _cleanup_(sd_netlink_unrefp) sd_netlink *rtnl = NULL;
        int r = 0;

        if (argc > 1) {
                char **ifname;
                bool empty_line = false;

                STRV_FOREACH(ifname, argv + 1) {
                        int ifindex, q;

                        ifindex = rtnl_resolve_interface(&rtnl, *ifname);
                        if (ifindex < 0) {
                                log_warning_errno(ifindex, "Failed to resolve interface \"%s\", ignoring: %m", *ifname);
                                continue;
                        }

                        q = status_ifindex(bus, ifindex, NULL, STATUS_ALL, &empty_line);
                        if (q < 0)
                                r = q;
                }
        } else
                r = status_all(bus, STATUS_ALL);

        return r;
}

static int call_dns(sd_bus *bus, char **dns, const BusLocator *locator, sd_bus_error *error, bool extended) {
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *req = NULL;
        char **p;
        int r;

        r = bus_message_new_method_call(bus, &req, locator, extended ? "SetLinkDNSEx" : "SetLinkDNS");
        if (r < 0)
                return bus_log_create_error(r);

        r = sd_bus_message_append(req, "i", arg_ifindex);
        if (r < 0)
                return bus_log_create_error(r);

        r = sd_bus_message_open_container(req, 'a', extended ? "(iayqs)" : "(iay)");
        if (r < 0)
                return bus_log_create_error(r);

        /* If only argument is the empty string, then call SetLinkDNS() with an
         * empty list, which will clear the list of domains for an interface. */
        if (!strv_equal(dns, STRV_MAKE("")))
                STRV_FOREACH(p, dns) {
                        _cleanup_free_ char *name = NULL;
                        struct in_addr_data data;
                        uint16_t port;
                        int ifindex;

                        r = in_addr_port_ifindex_name_from_string_auto(*p, &data.family, &data.address, &port, &ifindex, &name);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse DNS server address: %s", *p);

                        if (ifindex != 0 && ifindex != arg_ifindex)
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Invalid ifindex: %i", ifindex);

                        r = sd_bus_message_open_container(req, 'r', extended ? "iayqs" : "iay");
                        if (r < 0)
                                return bus_log_create_error(r);

                        r = sd_bus_message_append(req, "i", data.family);
                        if (r < 0)
                                return bus_log_create_error(r);

                        r = sd_bus_message_append_array(req, 'y', &data.address, FAMILY_ADDRESS_SIZE(data.family));
                        if (r < 0)
                                return bus_log_create_error(r);

                        if (extended) {
                                r = sd_bus_message_append(req, "q", port);
                                if (r < 0)
                                        return bus_log_create_error(r);

                                r = sd_bus_message_append(req, "s", name);
                                if (r < 0)
                                        return bus_log_create_error(r);
                        }

                        r = sd_bus_message_close_container(req);
                        if (r < 0)
                                return bus_log_create_error(r);
                }

        r = sd_bus_message_close_container(req);
        if (r < 0)
                return bus_log_create_error(r);

        r = sd_bus_call(bus, req, 0, error, NULL);
        if (r < 0 && extended && sd_bus_error_has_name(error, SD_BUS_ERROR_UNKNOWN_METHOD)) {
                sd_bus_error_free(error);
                return call_dns(bus, dns, locator, error, false);
        }
        return r;
}

static int verb_dns(int argc, char **argv, void *userdata) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus *bus = userdata;
        int r;

        assert(bus);

        if (argc >= 2) {
                r = ifname_mangle(argv[1]);
                if (r < 0)
                        return r;
        }

        if (arg_ifindex <= 0)
                return status_all(bus, STATUS_DNS);

        if (argc < 3)
                return status_ifindex(bus, arg_ifindex, NULL, STATUS_DNS, NULL);

        r = call_dns(bus, argv + 2, bus_resolve_mgr, &error, true);
        if (r < 0 && sd_bus_error_has_name(&error, BUS_ERROR_LINK_BUSY)) {
                sd_bus_error_free(&error);

                r = call_dns(bus, argv + 2, bus_network_mgr, &error, true);
        }
        if (r < 0) {
                if (arg_ifindex_permissive &&
                    sd_bus_error_has_name(&error, BUS_ERROR_NO_SUCH_LINK))
                        return 0;

                return log_error_errno(r, "Failed to set DNS configuration: %s", bus_error_message(&error, r));
        }

        return 0;
}

static int call_domain(sd_bus *bus, char **domain, const BusLocator *locator, sd_bus_error *error) {
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *req = NULL;
        char **p;
        int r;

        r = bus_message_new_method_call(bus, &req, locator, "SetLinkDomains");
        if (r < 0)
                return bus_log_create_error(r);

        r = sd_bus_message_append(req, "i", arg_ifindex);
        if (r < 0)
                return bus_log_create_error(r);

        r = sd_bus_message_open_container(req, 'a', "(sb)");
        if (r < 0)
                return bus_log_create_error(r);

        /* If only argument is the empty string, then call SetLinkDomains() with an
         * empty list, which will clear the list of domains for an interface. */
        if (!strv_equal(domain, STRV_MAKE("")))
                STRV_FOREACH(p, domain) {
                        const char *n;

                        n = **p == '~' ? *p + 1 : *p;

                        r = dns_name_is_valid(n);
                        if (r < 0)
                                return log_error_errno(r, "Failed to validate specified domain %s: %m", n);
                        if (r == 0)
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                                       "Domain not valid: %s",
                                                       n);

                        r = sd_bus_message_append(req, "(sb)", n, **p == '~');
                        if (r < 0)
                                return bus_log_create_error(r);
                }

        r = sd_bus_message_close_container(req);
        if (r < 0)
                return bus_log_create_error(r);

        return sd_bus_call(bus, req, 0, error, NULL);
}

static int verb_domain(int argc, char **argv, void *userdata) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus *bus = userdata;
        int r;

        assert(bus);

        if (argc >= 2) {
                r = ifname_mangle(argv[1]);
                if (r < 0)
                        return r;
        }

        if (arg_ifindex <= 0)
                return status_all(bus, STATUS_DOMAIN);

        if (argc < 3)
                return status_ifindex(bus, arg_ifindex, NULL, STATUS_DOMAIN, NULL);

        r = call_domain(bus, argv + 2, bus_resolve_mgr, &error);
        if (r < 0 && sd_bus_error_has_name(&error, BUS_ERROR_LINK_BUSY)) {
                sd_bus_error_free(&error);

                r = call_domain(bus, argv + 2, bus_network_mgr, &error);
        }
        if (r < 0) {
                if (arg_ifindex_permissive &&
                    sd_bus_error_has_name(&error, BUS_ERROR_NO_SUCH_LINK))
                        return 0;

                return log_error_errno(r, "Failed to set domain configuration: %s", bus_error_message(&error, r));
        }

        return 0;
}

static int verb_default_route(int argc, char **argv, void *userdata) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus *bus = userdata;
        int r, b;

        assert(bus);

        if (argc >= 2) {
                r = ifname_mangle(argv[1]);
                if (r < 0)
                        return r;
        }

        if (arg_ifindex <= 0)
                return status_all(bus, STATUS_DEFAULT_ROUTE);

        if (argc < 3)
                return status_ifindex(bus, arg_ifindex, NULL, STATUS_DEFAULT_ROUTE, NULL);

        b = parse_boolean(argv[2]);
        if (b < 0)
                return log_error_errno(b, "Failed to parse boolean argument: %s", argv[2]);

        r = bus_call_method(bus, bus_resolve_mgr, "SetLinkDefaultRoute", &error, NULL, "ib", arg_ifindex, b);
        if (r < 0 && sd_bus_error_has_name(&error, BUS_ERROR_LINK_BUSY)) {
                sd_bus_error_free(&error);

                r = bus_call_method(bus, bus_network_mgr, "SetLinkDefaultRoute", &error, NULL, "ib", arg_ifindex, b);
        }
        if (r < 0) {
                if (arg_ifindex_permissive &&
                    sd_bus_error_has_name(&error, BUS_ERROR_NO_SUCH_LINK))
                        return 0;

                return log_error_errno(r, "Failed to set default route configuration: %s", bus_error_message(&error, r));
        }

        return 0;
}

static int verb_llmnr(int argc, char **argv, void *userdata) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus *bus = userdata;
        int r;

        assert(bus);

        if (argc >= 2) {
                r = ifname_mangle(argv[1]);
                if (r < 0)
                        return r;
        }

        if (arg_ifindex <= 0)
                return status_all(bus, STATUS_LLMNR);

        if (argc < 3)
                return status_ifindex(bus, arg_ifindex, NULL, STATUS_LLMNR, NULL);

        r = bus_call_method(bus, bus_resolve_mgr, "SetLinkLLMNR", &error, NULL, "is", arg_ifindex, argv[2]);
        if (r < 0 && sd_bus_error_has_name(&error, BUS_ERROR_LINK_BUSY)) {
                sd_bus_error_free(&error);

                r = bus_call_method(bus, bus_network_mgr, "SetLinkLLMNR", &error, NULL, "is", arg_ifindex, argv[2]);
        }
        if (r < 0) {
                if (arg_ifindex_permissive &&
                    sd_bus_error_has_name(&error, BUS_ERROR_NO_SUCH_LINK))
                        return 0;

                return log_error_errno(r, "Failed to set LLMNR configuration: %s", bus_error_message(&error, r));
        }

        return 0;
}

static int verb_mdns(int argc, char **argv, void *userdata) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus *bus = userdata;
        int r;

        assert(bus);

        if (argc >= 2) {
                r = ifname_mangle(argv[1]);
                if (r < 0)
                        return r;
        }

        if (arg_ifindex <= 0)
                return status_all(bus, STATUS_MDNS);

        if (argc < 3)
                return status_ifindex(bus, arg_ifindex, NULL, STATUS_MDNS, NULL);

        r = bus_call_method(bus, bus_resolve_mgr, "SetLinkMulticastDNS", &error, NULL, "is", arg_ifindex, argv[2]);
        if (r < 0 && sd_bus_error_has_name(&error, BUS_ERROR_LINK_BUSY)) {
                sd_bus_error_free(&error);

                r = bus_call_method(
                                bus,
                                bus_network_mgr,
                                "SetLinkMulticastDNS",
                                &error,
                                NULL,
                                "is", arg_ifindex, argv[2]);
        }
        if (r < 0) {
                if (arg_ifindex_permissive &&
                    sd_bus_error_has_name(&error, BUS_ERROR_NO_SUCH_LINK))
                        return 0;

                return log_error_errno(r, "Failed to set MulticastDNS configuration: %s", bus_error_message(&error, r));
        }

        return 0;
}

static int verb_dns_over_tls(int argc, char **argv, void *userdata) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus *bus = userdata;
        int r;

        assert(bus);

        if (argc >= 2) {
                r = ifname_mangle(argv[1]);
                if (r < 0)
                        return r;
        }

        if (arg_ifindex <= 0)
                return status_all(bus, STATUS_PRIVATE);

        if (argc < 3)
                return status_ifindex(bus, arg_ifindex, NULL, STATUS_PRIVATE, NULL);

        r = bus_call_method(bus, bus_resolve_mgr, "SetLinkDNSOverTLS", &error, NULL, "is", arg_ifindex, argv[2]);
        if (r < 0 && sd_bus_error_has_name(&error, BUS_ERROR_LINK_BUSY)) {
                sd_bus_error_free(&error);

                r = bus_call_method(
                                bus,
                                bus_network_mgr,
                                "SetLinkDNSOverTLS",
                                &error,
                                NULL,
                                "is", arg_ifindex, argv[2]);
        }
        if (r < 0) {
                if (arg_ifindex_permissive &&
                    sd_bus_error_has_name(&error, BUS_ERROR_NO_SUCH_LINK))
                        return 0;

                return log_error_errno(r, "Failed to set DNSOverTLS configuration: %s", bus_error_message(&error, r));
        }

        return 0;
}

static int verb_dnssec(int argc, char **argv, void *userdata) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus *bus = userdata;
        int r;

        assert(bus);

        if (argc >= 2) {
                r = ifname_mangle(argv[1]);
                if (r < 0)
                        return r;
        }

        if (arg_ifindex <= 0)
                return status_all(bus, STATUS_DNSSEC);

        if (argc < 3)
                return status_ifindex(bus, arg_ifindex, NULL, STATUS_DNSSEC, NULL);

        r = bus_call_method(bus, bus_resolve_mgr, "SetLinkDNSSEC", &error, NULL, "is", arg_ifindex, argv[2]);
        if (r < 0 && sd_bus_error_has_name(&error, BUS_ERROR_LINK_BUSY)) {
                sd_bus_error_free(&error);

                r = bus_call_method(bus, bus_network_mgr, "SetLinkDNSSEC", &error, NULL, "is", arg_ifindex, argv[2]);
        }
        if (r < 0) {
                if (arg_ifindex_permissive &&
                    sd_bus_error_has_name(&error, BUS_ERROR_NO_SUCH_LINK))
                        return 0;

                return log_error_errno(r, "Failed to set DNSSEC configuration: %s", bus_error_message(&error, r));
        }

        return 0;
}

static int call_nta(sd_bus *bus, char **nta, const BusLocator *locator,  sd_bus_error *error) {
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *req = NULL;
        int r;

        r = bus_message_new_method_call(bus, &req, locator, "SetLinkDNSSECNegativeTrustAnchors");
        if (r < 0)
                return bus_log_create_error(r);

        r = sd_bus_message_append(req, "i", arg_ifindex);
        if (r < 0)
                return bus_log_create_error(r);

        r = sd_bus_message_append_strv(req, nta);
        if (r < 0)
                return bus_log_create_error(r);

        return sd_bus_call(bus, req, 0, error, NULL);
}

static int verb_nta(int argc, char **argv, void *userdata) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus *bus = userdata;
        char **p;
        int r;
        bool clear;

        assert(bus);

        if (argc >= 2) {
                r = ifname_mangle(argv[1]);
                if (r < 0)
                        return r;
        }

        if (arg_ifindex <= 0)
                return status_all(bus, STATUS_NTA);

        if (argc < 3)
                return status_ifindex(bus, arg_ifindex, NULL, STATUS_NTA, NULL);

        /* If only argument is the empty string, then call SetLinkDNSSECNegativeTrustAnchors()
         * with an empty list, which will clear the list of domains for an interface. */
        clear = strv_equal(argv + 2, STRV_MAKE(""));

        if (!clear)
                STRV_FOREACH(p, argv + 2) {
                        r = dns_name_is_valid(*p);
                        if (r < 0)
                                return log_error_errno(r, "Failed to validate specified domain %s: %m", *p);
                        if (r == 0)
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                                       "Domain not valid: %s",
                                                       *p);
                }

        r = call_nta(bus, clear ? NULL : argv + 2, bus_resolve_mgr, &error);
        if (r < 0 && sd_bus_error_has_name(&error, BUS_ERROR_LINK_BUSY)) {
                sd_bus_error_free(&error);

                r = call_nta(bus, clear ? NULL : argv + 2, bus_network_mgr, &error);
        }
        if (r < 0) {
                if (arg_ifindex_permissive &&
                    sd_bus_error_has_name(&error, BUS_ERROR_NO_SUCH_LINK))
                        return 0;

                return log_error_errno(r, "Failed to set DNSSEC NTA configuration: %s", bus_error_message(&error, r));
        }

        return 0;
}

static int verb_revert_link(int argc, char **argv, void *userdata) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus *bus = userdata;
        int r;

        assert(bus);

        if (argc >= 2) {
                r = ifname_mangle(argv[1]);
                if (r < 0)
                        return r;
        }

        if (arg_ifindex <= 0)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Interface argument required.");

        r = bus_call_method(bus, bus_resolve_mgr, "RevertLink", &error, NULL, "i", arg_ifindex);
        if (r < 0 && sd_bus_error_has_name(&error, BUS_ERROR_LINK_BUSY)) {
                sd_bus_error_free(&error);

                r = bus_call_method(bus, bus_network_mgr, "RevertLinkDNS", &error, NULL, "i", arg_ifindex);
        }
        if (r < 0) {
                if (arg_ifindex_permissive &&
                    sd_bus_error_has_name(&error, BUS_ERROR_NO_SUCH_LINK))
                        return 0;

                return log_error_errno(r, "Failed to revert interface configuration: %s", bus_error_message(&error, r));
        }

        return 0;
}

static int verb_log_level(int argc, char *argv[], void *userdata) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus *bus = userdata;
        int r;

        assert(bus);

        if (argc == 1) {
                _cleanup_free_ char *level = NULL;

                r = sd_bus_get_property_string(
                                bus,
                                "org.freedesktop.resolve1",
                                "/org/freedesktop/LogControl1",
                                "org.freedesktop.LogControl1",
                                "LogLevel",
                                &error,
                                &level);
                if (r < 0)
                        return log_error_errno(r, "Failed to get log level: %s", bus_error_message(&error, r));

                puts(level);

        } else {
                assert(argc == 2);

                r = sd_bus_set_property(
                                bus,
                                "org.freedesktop.resolve1",
                                "/org/freedesktop/LogControl1",
                                "org.freedesktop.LogControl1",
                                "LogLevel",
                                &error,
                                "s",
                                argv[1]);
                if (r < 0)
                        return log_error_errno(r, "Failed to set log level: %s", bus_error_message(&error, r));
        }

        return 0;
}

static void help_protocol_types(void) {
        if (arg_legend)
                puts("Known protocol types:");
        puts("dns\nllmnr\nllmnr-ipv4\nllmnr-ipv6\nmdns\nmdns-ipv4\nmdns-ipv6");
}

static void help_dns_types(void) {
        if (arg_legend)
                puts("Known DNS RR types:");

        DUMP_STRING_TABLE(dns_type, int, _DNS_TYPE_MAX);
}

static void help_dns_classes(void) {
        if (arg_legend)
                puts("Known DNS RR classes:");

        DUMP_STRING_TABLE(dns_class, int, _DNS_CLASS_MAX);
}

static int compat_help(void) {
        _cleanup_free_ char *link = NULL;
        int r;

        r = terminal_urlify_man("resolvectl", "1", &link);
        if (r < 0)
                return log_oom();

        printf("%1$s [OPTIONS...] HOSTNAME|ADDRESS...\n"
               "%1$s [OPTIONS...] --service [[NAME] TYPE] DOMAIN\n"
               "%1$s [OPTIONS...] --openpgp EMAIL@DOMAIN...\n"
               "%1$s [OPTIONS...] --statistics\n"
               "%1$s [OPTIONS...] --reset-statistics\n"
               "\n"
               "%2$sResolve domain names, IPv4 and IPv6 addresses, DNS records, and services.%3$s\n\n"
               "  -h --help                 Show this help\n"
               "     --version              Show package version\n"
               "     --no-pager             Do not pipe output into a pager\n"
               "  -4                        Resolve IPv4 addresses\n"
               "  -6                        Resolve IPv6 addresses\n"
               "  -i --interface=INTERFACE  Look on interface\n"
               "  -p --protocol=PROTO|help  Look via protocol\n"
               "  -t --type=TYPE|help       Query RR with DNS type\n"
               "  -c --class=CLASS|help     Query RR with DNS class\n"
               "     --service              Resolve service (SRV)\n"
               "     --service-address=BOOL Resolve address for services (default: yes)\n"
               "     --service-txt=BOOL     Resolve TXT records for services (default: yes)\n"
               "     --openpgp              Query OpenPGP public key\n"
               "     --tlsa                 Query TLS public key\n"
               "     --cname=BOOL           Follow CNAME redirects (default: yes)\n"
               "     --search=BOOL          Use search domains for single-label names\n"
               "                                                              (default: yes)\n"
               "     --raw[=payload|packet] Dump the answer as binary data\n"
               "     --legend=BOOL          Print headers and additional info (default: yes)\n"
               "     --statistics           Show resolver statistics\n"
               "     --reset-statistics     Reset resolver statistics\n"
               "     --status               Show link and server status\n"
               "     --flush-caches         Flush all local DNS caches\n"
               "     --reset-server-features\n"
               "                            Forget learnt DNS server feature levels\n"
               "     --set-dns=SERVER       Set per-interface DNS server address\n"
               "     --set-domain=DOMAIN    Set per-interface search domain\n"
               "     --set-llmnr=MODE       Set per-interface LLMNR mode\n"
               "     --set-mdns=MODE        Set per-interface MulticastDNS mode\n"
               "     --set-dnsovertls=MODE  Set per-interface DNS-over-TLS mode\n"
               "     --set-dnssec=MODE      Set per-interface DNSSEC mode\n"
               "     --set-nta=DOMAIN       Set per-interface DNSSEC NTA\n"
               "     --revert               Revert per-interface configuration\n"
               "\nSee the %4$s for details.\n",
               program_invocation_short_name,
               ansi_highlight(),
               ansi_normal(),
               link);

        return 0;
}

static int native_help(void) {
        _cleanup_free_ char *link = NULL;
        int r;

        r = terminal_urlify_man("resolvectl", "1", &link);
        if (r < 0)
                return log_oom();

        printf("%s [OPTIONS...] COMMAND ...\n"
               "\n"
               "%sSend control commands to the network name resolution manager, or%s\n"
               "%sresolve domain names, IPv4 and IPv6 addresses, DNS records, and services.%s\n"
               "\nCommands:\n"
               "  query HOSTNAME|ADDRESS...    Resolve domain names, IPv4 and IPv6 addresses\n"
               "  service [[NAME] TYPE] DOMAIN Resolve service (SRV)\n"
               "  openpgp EMAIL@DOMAIN...      Query OpenPGP public key\n"
               "  tlsa DOMAIN[:PORT]...        Query TLS public key\n"
               "  status [LINK...]             Show link and server status\n"
               "  statistics                   Show resolver statistics\n"
               "  reset-statistics             Reset resolver statistics\n"
               "  flush-caches                 Flush all local DNS caches\n"
               "  reset-server-features        Forget learnt DNS server feature levels\n"
               "  dns [LINK [SERVER...]]       Get/set per-interface DNS server address\n"
               "  domain [LINK [DOMAIN...]]    Get/set per-interface search domain\n"
               "  default-route [LINK [BOOL]]  Get/set per-interface default route flag\n"
               "  llmnr [LINK [MODE]]          Get/set per-interface LLMNR mode\n"
               "  mdns [LINK [MODE]]           Get/set per-interface MulticastDNS mode\n"
               "  dnsovertls [LINK [MODE]]     Get/set per-interface DNS-over-TLS mode\n"
               "  dnssec [LINK [MODE]]         Get/set per-interface DNSSEC mode\n"
               "  nta [LINK [DOMAIN...]]       Get/set per-interface DNSSEC NTA\n"
               "  revert LINK                  Revert per-interface configuration\n"
               "  log-level [LEVEL]            Get/set logging threshold for systemd-resolved\n"
               "\nOptions:\n"
               "  -h --help                    Show this help\n"
               "     --version                 Show package version\n"
               "     --no-pager                Do not pipe output into a pager\n"
               "  -4                           Resolve IPv4 addresses\n"
               "  -6                           Resolve IPv6 addresses\n"
               "  -i --interface=INTERFACE     Look on interface\n"
               "  -p --protocol=PROTO|help     Look via protocol\n"
               "  -t --type=TYPE|help          Query RR with DNS type\n"
               "  -c --class=CLASS|help        Query RR with DNS class\n"
               "     --service-address=BOOL    Resolve address for services (default: yes)\n"
               "     --service-txt=BOOL        Resolve TXT records for services (default: yes)\n"
               "     --cname=BOOL              Follow CNAME redirects (default: yes)\n"
               "     --validate=BOOL           Allow DNSSEC validation (default: yes)\n"
               "     --synthesize=BOOL         Allow synthetic response (default: yes)\n"
               "     --cache=BOOL              Allow response from cache (default: yes)\n"
               "     --zone=BOOL               Allow response from locally registered mDNS/LLMNR\n"
               "                               records (default: yes)\n"
               "     --trust-anchor=BOOL       Allow response from local trust anchor (default: yes)\n"
               "     --network=BOOL            Allow response from network (default: yes)\n"
               "     --search=BOOL             Use search domains for single-label names (default: yes)\n"
               "     --raw[=payload|packet]    Dump the answer as binary data\n"
               "     --legend=BOOL             Print headers and additional info (default: yes)\n"
               "\nSee the %s for details.\n",
               program_invocation_short_name,
               ansi_highlight(),
               ansi_normal(),
               ansi_highlight(),
               ansi_normal(),
               link);

        return 0;
}

static int verb_help(int argc, char **argv, void *userdata) {
        return native_help();
}

static int compat_parse_argv(int argc, char *argv[]) {
        enum {
                ARG_VERSION = 0x100,
                ARG_LEGEND,
                ARG_SERVICE,
                ARG_CNAME,
                ARG_SERVICE_ADDRESS,
                ARG_SERVICE_TXT,
                ARG_OPENPGP,
                ARG_TLSA,
                ARG_RAW,
                ARG_SEARCH,
                ARG_STATISTICS,
                ARG_RESET_STATISTICS,
                ARG_STATUS,
                ARG_FLUSH_CACHES,
                ARG_RESET_SERVER_FEATURES,
                ARG_NO_PAGER,
                ARG_SET_DNS,
                ARG_SET_DOMAIN,
                ARG_SET_LLMNR,
                ARG_SET_MDNS,
                ARG_SET_PRIVATE,
                ARG_SET_DNSSEC,
                ARG_SET_NTA,
                ARG_REVERT_LINK,
        };

        static const struct option options[] = {
                { "help",                  no_argument,       NULL, 'h'                       },
                { "version",               no_argument,       NULL, ARG_VERSION               },
                { "type",                  required_argument, NULL, 't'                       },
                { "class",                 required_argument, NULL, 'c'                       },
                { "legend",                required_argument, NULL, ARG_LEGEND                },
                { "interface",             required_argument, NULL, 'i'                       },
                { "protocol",              required_argument, NULL, 'p'                       },
                { "cname",                 required_argument, NULL, ARG_CNAME                 },
                { "service",               no_argument,       NULL, ARG_SERVICE               },
                { "service-address",       required_argument, NULL, ARG_SERVICE_ADDRESS       },
                { "service-txt",           required_argument, NULL, ARG_SERVICE_TXT           },
                { "openpgp",               no_argument,       NULL, ARG_OPENPGP               },
                { "tlsa",                  optional_argument, NULL, ARG_TLSA                  },
                { "raw",                   optional_argument, NULL, ARG_RAW                   },
                { "search",                required_argument, NULL, ARG_SEARCH                },
                { "statistics",            no_argument,       NULL, ARG_STATISTICS,           },
                { "reset-statistics",      no_argument,       NULL, ARG_RESET_STATISTICS      },
                { "status",                no_argument,       NULL, ARG_STATUS                },
                { "flush-caches",          no_argument,       NULL, ARG_FLUSH_CACHES          },
                { "reset-server-features", no_argument,       NULL, ARG_RESET_SERVER_FEATURES },
                { "no-pager",              no_argument,       NULL, ARG_NO_PAGER              },
                { "set-dns",               required_argument, NULL, ARG_SET_DNS               },
                { "set-domain",            required_argument, NULL, ARG_SET_DOMAIN            },
                { "set-llmnr",             required_argument, NULL, ARG_SET_LLMNR             },
                { "set-mdns",              required_argument, NULL, ARG_SET_MDNS              },
                { "set-dnsovertls",        required_argument, NULL, ARG_SET_PRIVATE           },
                { "set-dnssec",            required_argument, NULL, ARG_SET_DNSSEC            },
                { "set-nta",               required_argument, NULL, ARG_SET_NTA               },
                { "revert",                no_argument,       NULL, ARG_REVERT_LINK           },
                {}
        };

        int c, r;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "h46i:t:c:p:", options, NULL)) >= 0)
                switch(c) {

                case 'h':
                        return compat_help();

                case ARG_VERSION:
                        return version();

                case '4':
                        arg_family = AF_INET;
                        break;

                case '6':
                        arg_family = AF_INET6;
                        break;

                case 'i':
                        r = ifname_mangle(optarg);
                        if (r < 0)
                                return r;
                        break;

                case 't':
                        if (streq(optarg, "help")) {
                                help_dns_types();
                                return 0;
                        }

                        r = dns_type_from_string(optarg);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse RR record type %s: %m", optarg);

                        arg_type = (uint16_t) r;
                        assert((int) arg_type == r);

                        arg_mode = MODE_RESOLVE_RECORD;
                        break;

                case 'c':
                        if (streq(optarg, "help")) {
                                help_dns_classes();
                                return 0;
                        }

                        r = dns_class_from_string(optarg);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse RR record class %s: %m", optarg);

                        arg_class = (uint16_t) r;
                        assert((int) arg_class == r);

                        break;

                case ARG_LEGEND:
                        r = parse_boolean_argument("--legend=", optarg, &arg_legend);
                        if (r < 0)
                                return r;
                        break;

                case 'p':
                        if (streq(optarg, "help")) {
                                help_protocol_types();
                                return 0;
                        } else if (streq(optarg, "dns"))
                                arg_flags |= SD_RESOLVED_DNS;
                        else if (streq(optarg, "llmnr"))
                                arg_flags |= SD_RESOLVED_LLMNR;
                        else if (streq(optarg, "llmnr-ipv4"))
                                arg_flags |= SD_RESOLVED_LLMNR_IPV4;
                        else if (streq(optarg, "llmnr-ipv6"))
                                arg_flags |= SD_RESOLVED_LLMNR_IPV6;
                        else if (streq(optarg, "mdns"))
                                arg_flags |= SD_RESOLVED_MDNS;
                        else if (streq(optarg, "mdns-ipv4"))
                                arg_flags |= SD_RESOLVED_MDNS_IPV4;
                        else if (streq(optarg, "mdns-ipv6"))
                                arg_flags |= SD_RESOLVED_MDNS_IPV6;
                        else
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                                       "Unknown protocol specifier: %s", optarg);

                        break;

                case ARG_SERVICE:
                        arg_mode = MODE_RESOLVE_SERVICE;
                        break;

                case ARG_OPENPGP:
                        arg_mode = MODE_RESOLVE_OPENPGP;
                        break;

                case ARG_TLSA:
                        arg_mode = MODE_RESOLVE_TLSA;
                        if (!optarg || service_family_is_valid(optarg))
                                arg_service_family = optarg;
                        else
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                                       "Unknown service family \"%s\".", optarg);
                        break;

                case ARG_RAW:
                        if (on_tty())
                                return log_error_errno(SYNTHETIC_ERRNO(ENOTTY),
                                                       "Refusing to write binary data to tty.");

                        if (optarg == NULL || streq(optarg, "payload"))
                                arg_raw = RAW_PAYLOAD;
                        else if (streq(optarg, "packet"))
                                arg_raw = RAW_PACKET;
                        else
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                                       "Unknown --raw specifier \"%s\".",
                                                       optarg);

                        arg_legend = false;
                        break;

                case ARG_CNAME:
                        r = parse_boolean_argument("--cname=", optarg, NULL);
                        if (r < 0)
                                return r;
                        SET_FLAG(arg_flags, SD_RESOLVED_NO_CNAME, r == 0);
                        break;

                case ARG_SERVICE_ADDRESS:
                        r = parse_boolean_argument("--service-address=", optarg, NULL);
                        if (r < 0)
                                return r;
                        SET_FLAG(arg_flags, SD_RESOLVED_NO_ADDRESS, r == 0);
                        break;

                case ARG_SERVICE_TXT:
                        r = parse_boolean_argument("--service-txt=", optarg, NULL);
                        if (r < 0)
                                return r;
                        SET_FLAG(arg_flags, SD_RESOLVED_NO_TXT, r == 0);
                        break;

                case ARG_SEARCH:
                        r = parse_boolean_argument("--search=", optarg, NULL);
                        if (r < 0)
                                return r;
                        SET_FLAG(arg_flags, SD_RESOLVED_NO_SEARCH, r == 0);
                        break;

                case ARG_STATISTICS:
                        arg_mode = MODE_STATISTICS;
                        break;

                case ARG_RESET_STATISTICS:
                        arg_mode = MODE_RESET_STATISTICS;
                        break;

                case ARG_FLUSH_CACHES:
                        arg_mode = MODE_FLUSH_CACHES;
                        break;

                case ARG_RESET_SERVER_FEATURES:
                        arg_mode = MODE_RESET_SERVER_FEATURES;
                        break;

                case ARG_STATUS:
                        arg_mode = MODE_STATUS;
                        break;

                case ARG_NO_PAGER:
                        arg_pager_flags |= PAGER_DISABLE;
                        break;

                case ARG_SET_DNS:
                        r = strv_extend(&arg_set_dns, optarg);
                        if (r < 0)
                                return log_oom();

                        arg_mode = MODE_SET_LINK;
                        break;

                case ARG_SET_DOMAIN:
                        r = strv_extend(&arg_set_domain, optarg);
                        if (r < 0)
                                return log_oom();

                        arg_mode = MODE_SET_LINK;
                        break;

                case ARG_SET_LLMNR:
                        arg_set_llmnr = optarg;
                        arg_mode = MODE_SET_LINK;
                        break;

                case ARG_SET_MDNS:
                        arg_set_mdns = optarg;
                        arg_mode = MODE_SET_LINK;
                        break;

                case ARG_SET_PRIVATE:
                        arg_set_dns_over_tls = optarg;
                        arg_mode = MODE_SET_LINK;
                        break;

                case ARG_SET_DNSSEC:
                        arg_set_dnssec = optarg;
                        arg_mode = MODE_SET_LINK;
                        break;

                case ARG_SET_NTA:
                        r = strv_extend(&arg_set_nta, optarg);
                        if (r < 0)
                                return log_oom();

                        arg_mode = MODE_SET_LINK;
                        break;

                case ARG_REVERT_LINK:
                        arg_mode = MODE_REVERT_LINK;
                        break;

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached("Unhandled option");
                }

        if (arg_type == 0 && arg_class != 0)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "--class= may only be used in conjunction with --type=.");

        if (arg_type != 0 && arg_mode == MODE_RESOLVE_SERVICE)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "--service and --type= may not be combined.");

        if (arg_type != 0 && arg_class == 0)
                arg_class = DNS_CLASS_IN;

        if (arg_class != 0 && arg_type == 0)
                arg_type = DNS_TYPE_A;

        if (IN_SET(arg_mode, MODE_SET_LINK, MODE_REVERT_LINK)) {

                if (arg_ifindex <= 0)
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                               "--set-dns=, --set-domain=, --set-llmnr=, --set-mdns=, --set-dnsovertls=, --set-dnssec=, --set-nta= and --revert require --interface=.");
        }

        return 1 /* work to do */;
}

static int native_parse_argv(int argc, char *argv[]) {
        enum {
                ARG_VERSION = 0x100,
                ARG_LEGEND,
                ARG_CNAME,
                ARG_VALIDATE,
                ARG_SYNTHESIZE,
                ARG_CACHE,
                ARG_ZONE,
                ARG_TRUST_ANCHOR,
                ARG_NETWORK,
                ARG_SERVICE_ADDRESS,
                ARG_SERVICE_TXT,
                ARG_RAW,
                ARG_SEARCH,
                ARG_NO_PAGER,
        };

        static const struct option options[] = {
                { "help",                  no_argument,       NULL, 'h'                       },
                { "version",               no_argument,       NULL, ARG_VERSION               },
                { "type",                  required_argument, NULL, 't'                       },
                { "class",                 required_argument, NULL, 'c'                       },
                { "legend",                required_argument, NULL, ARG_LEGEND                },
                { "interface",             required_argument, NULL, 'i'                       },
                { "protocol",              required_argument, NULL, 'p'                       },
                { "cname",                 required_argument, NULL, ARG_CNAME                 },
                { "validate",              required_argument, NULL, ARG_VALIDATE              },
                { "synthesize",            required_argument, NULL, ARG_SYNTHESIZE            },
                { "cache",                 required_argument, NULL, ARG_CACHE                 },
                { "zone",                  required_argument, NULL, ARG_ZONE                  },
                { "trust-anchor",          required_argument, NULL, ARG_TRUST_ANCHOR          },
                { "network",               required_argument, NULL, ARG_NETWORK               },
                { "service-address",       required_argument, NULL, ARG_SERVICE_ADDRESS       },
                { "service-txt",           required_argument, NULL, ARG_SERVICE_TXT           },
                { "raw",                   optional_argument, NULL, ARG_RAW                   },
                { "search",                required_argument, NULL, ARG_SEARCH                },
                { "no-pager",              no_argument,       NULL, ARG_NO_PAGER              },
                {}
        };

        int c, r;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "h46i:t:c:p:", options, NULL)) >= 0)
                switch(c) {

                case 'h':
                        return native_help();

                case ARG_VERSION:
                        return version();

                case '4':
                        arg_family = AF_INET;
                        break;

                case '6':
                        arg_family = AF_INET6;
                        break;

                case 'i':
                        r = ifname_mangle(optarg);
                        if (r < 0)
                                return r;
                        break;

                case 't':
                        if (streq(optarg, "help")) {
                                help_dns_types();
                                return 0;
                        }

                        r = dns_type_from_string(optarg);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse RR record type %s: %m", optarg);

                        arg_type = (uint16_t) r;
                        assert((int) arg_type == r);

                        break;

                case 'c':
                        if (streq(optarg, "help")) {
                                help_dns_classes();
                                return 0;
                        }

                        r = dns_class_from_string(optarg);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse RR record class %s: %m", optarg);

                        arg_class = (uint16_t) r;
                        assert((int) arg_class == r);

                        break;

                case ARG_LEGEND:
                        r = parse_boolean_argument("--legend=", optarg, &arg_legend);
                        if (r < 0)
                                return r;
                        break;

                case 'p':
                        if (streq(optarg, "help")) {
                                help_protocol_types();
                                return 0;
                        } else if (streq(optarg, "dns"))
                                arg_flags |= SD_RESOLVED_DNS;
                        else if (streq(optarg, "llmnr"))
                                arg_flags |= SD_RESOLVED_LLMNR;
                        else if (streq(optarg, "llmnr-ipv4"))
                                arg_flags |= SD_RESOLVED_LLMNR_IPV4;
                        else if (streq(optarg, "llmnr-ipv6"))
                                arg_flags |= SD_RESOLVED_LLMNR_IPV6;
                        else if (streq(optarg, "mdns"))
                                arg_flags |= SD_RESOLVED_MDNS;
                        else if (streq(optarg, "mdns-ipv4"))
                                arg_flags |= SD_RESOLVED_MDNS_IPV4;
                        else if (streq(optarg, "mdns-ipv6"))
                                arg_flags |= SD_RESOLVED_MDNS_IPV6;
                        else
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                                       "Unknown protocol specifier: %s",
                                                       optarg);

                        break;

                case ARG_RAW:
                        if (on_tty())
                                return log_error_errno(SYNTHETIC_ERRNO(ENOTTY),
                                                       "Refusing to write binary data to tty.");

                        if (optarg == NULL || streq(optarg, "payload"))
                                arg_raw = RAW_PAYLOAD;
                        else if (streq(optarg, "packet"))
                                arg_raw = RAW_PACKET;
                        else
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                                       "Unknown --raw specifier \"%s\".",
                                                       optarg);

                        arg_legend = false;
                        break;

                case ARG_CNAME:
                        r = parse_boolean_argument("--cname=", optarg, NULL);
                        if (r < 0)
                                return r;
                        SET_FLAG(arg_flags, SD_RESOLVED_NO_CNAME, r == 0);
                        break;

                case ARG_VALIDATE:
                        r = parse_boolean_argument("--validate=", optarg, NULL);
                        if (r < 0)
                                return r;
                        SET_FLAG(arg_flags, SD_RESOLVED_NO_VALIDATE, r == 0);
                        break;

                case ARG_SYNTHESIZE:
                        r = parse_boolean_argument("--synthesize=", optarg, NULL);
                        if (r < 0)
                                return r;
                        SET_FLAG(arg_flags, SD_RESOLVED_NO_SYNTHESIZE, r == 0);
                        break;

                case ARG_CACHE:
                        r = parse_boolean_argument("--cache=", optarg, NULL);
                        if (r < 0)
                                return r;
                        SET_FLAG(arg_flags, SD_RESOLVED_NO_CACHE, r == 0);
                        break;

                case ARG_ZONE:
                        r = parse_boolean_argument("--zone=", optarg, NULL);
                        if (r < 0)
                                return r;
                        SET_FLAG(arg_flags, SD_RESOLVED_NO_ZONE, r == 0);
                        break;

                case ARG_TRUST_ANCHOR:
                        r = parse_boolean_argument("--trust-anchor=", optarg, NULL);
                        if (r < 0)
                                return r;
                        SET_FLAG(arg_flags, SD_RESOLVED_NO_TRUST_ANCHOR, r == 0);
                        break;

                case ARG_NETWORK:
                        r = parse_boolean_argument("--network=", optarg, NULL);
                        if (r < 0)
                                return r;
                        SET_FLAG(arg_flags, SD_RESOLVED_NO_NETWORK, r == 0);
                        break;

                case ARG_SERVICE_ADDRESS:
                        r = parse_boolean_argument("--service-address=", optarg, NULL);
                        if (r < 0)
                                return r;
                        SET_FLAG(arg_flags, SD_RESOLVED_NO_ADDRESS, r == 0);
                        break;

                case ARG_SERVICE_TXT:
                        r = parse_boolean_argument("--service-txt=", optarg, NULL);
                        if (r < 0)
                                return r;
                        SET_FLAG(arg_flags, SD_RESOLVED_NO_TXT, r == 0);
                        break;

                case ARG_SEARCH:
                        r = parse_boolean_argument("--search=", optarg, NULL);
                        if (r < 0)
                                return r;
                        SET_FLAG(arg_flags, SD_RESOLVED_NO_SEARCH, r == 0);
                        break;

                case ARG_NO_PAGER:
                        arg_pager_flags |= PAGER_DISABLE;
                        break;

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached("Unhandled option");
                }

        if (arg_type == 0 && arg_class != 0)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "--class= may only be used in conjunction with --type=.");

        if (arg_type != 0 && arg_class == 0)
                arg_class = DNS_CLASS_IN;

        if (arg_class != 0 && arg_type == 0)
                arg_type = DNS_TYPE_A;

        return 1 /* work to do */;
}

static int native_main(int argc, char *argv[], sd_bus *bus) {

        static const Verb verbs[] = {
                { "help",                  VERB_ANY, VERB_ANY, 0,            verb_help             },
                { "status",                VERB_ANY, VERB_ANY, VERB_DEFAULT, verb_status           },
                { "query",                 2,        VERB_ANY, 0,            verb_query            },
                { "service",               2,        4,        0,            verb_service          },
                { "openpgp",               2,        VERB_ANY, 0,            verb_openpgp          },
                { "tlsa",                  2,        VERB_ANY, 0,            verb_tlsa             },
                { "statistics",            VERB_ANY, 1,        0,            show_statistics       },
                { "reset-statistics",      VERB_ANY, 1,        0,            reset_statistics      },
                { "flush-caches",          VERB_ANY, 1,        0,            flush_caches          },
                { "reset-server-features", VERB_ANY, 1,        0,            reset_server_features },
                { "dns",                   VERB_ANY, VERB_ANY, 0,            verb_dns              },
                { "domain",                VERB_ANY, VERB_ANY, 0,            verb_domain           },
                { "default-route",         VERB_ANY, 3,        0,            verb_default_route    },
                { "llmnr",                 VERB_ANY, 3,        0,            verb_llmnr            },
                { "mdns",                  VERB_ANY, 3,        0,            verb_mdns             },
                { "dnsovertls",            VERB_ANY, 3,        0,            verb_dns_over_tls     },
                { "dnssec",                VERB_ANY, 3,        0,            verb_dnssec           },
                { "nta",                   VERB_ANY, VERB_ANY, 0,            verb_nta              },
                { "revert",                VERB_ANY, 2,        0,            verb_revert_link      },
                { "log-level",             VERB_ANY, 2,        0,            verb_log_level        },
                {}
        };

        return dispatch_verb(argc, argv, verbs, bus);
}

static int translate(const char *verb, const char *single_arg, size_t num_args, char **args, sd_bus *bus) {
        char **fake, **p;
        size_t num;

        assert(verb);
        assert(num_args == 0 || args);

        num = !!single_arg + num_args + 1;

        p = fake = newa0(char *, num + 1);
        *p++ = (char *) verb;
        if (single_arg)
                *p++ = (char *) single_arg;
        for (size_t i = 0; i < num_args; i++)
                *p++ = args[i];

        optind = 0;
        return native_main((int) num, fake, bus);
}

static int compat_main(int argc, char *argv[], sd_bus *bus) {
        int r = 0;

        switch (arg_mode) {
        case MODE_RESOLVE_HOST:
        case MODE_RESOLVE_RECORD:
                return translate("query", NULL, argc - optind, argv + optind, bus);

        case MODE_RESOLVE_SERVICE:
                return translate("service", NULL, argc - optind, argv + optind, bus);

        case MODE_RESOLVE_OPENPGP:
                return translate("openpgp", NULL, argc - optind, argv + optind, bus);

        case MODE_RESOLVE_TLSA:
                return translate("tlsa", arg_service_family, argc - optind, argv + optind, bus);

        case MODE_STATISTICS:
                return translate("statistics", NULL, 0, NULL, bus);

        case MODE_RESET_STATISTICS:
                return translate("reset-statistics", NULL, 0, NULL, bus);

        case MODE_FLUSH_CACHES:
                return translate("flush-caches", NULL, 0, NULL, bus);

        case MODE_RESET_SERVER_FEATURES:
                return translate("reset-server-features", NULL, 0, NULL, bus);

        case MODE_STATUS:
                return translate("status", NULL, argc - optind, argv + optind, bus);

        case MODE_SET_LINK:
                assert(arg_ifname);

                if (arg_set_dns) {
                        r = translate("dns", arg_ifname, strv_length(arg_set_dns), arg_set_dns, bus);
                        if (r < 0)
                                return r;
                }

                if (arg_set_domain) {
                        r = translate("domain", arg_ifname, strv_length(arg_set_domain), arg_set_domain, bus);
                        if (r < 0)
                                return r;
                }

                if (arg_set_nta) {
                        r = translate("nta", arg_ifname, strv_length(arg_set_nta), arg_set_nta, bus);
                        if (r < 0)
                                return r;
                }

                if (arg_set_llmnr) {
                        r = translate("llmnr", arg_ifname, 1, (char **) &arg_set_llmnr, bus);
                        if (r < 0)
                                return r;
                }

                if (arg_set_mdns) {
                        r = translate("mdns", arg_ifname, 1, (char **) &arg_set_mdns, bus);
                        if (r < 0)
                                return r;
                }

                if (arg_set_dns_over_tls) {
                        r = translate("dnsovertls", arg_ifname, 1, (char **) &arg_set_dns_over_tls, bus);
                        if (r < 0)
                                return r;
                }

                if (arg_set_dnssec) {
                        r = translate("dnssec", arg_ifname, 1, (char **) &arg_set_dnssec, bus);
                        if (r < 0)
                                return r;
                }

                return r;

        case MODE_REVERT_LINK:
                assert(arg_ifname);

                return translate("revert", arg_ifname, 0, NULL, bus);

        case _MODE_INVALID:
                assert_not_reached("invalid mode");
        }

        return 0;
}

static int run(int argc, char **argv) {
        _cleanup_(sd_bus_flush_close_unrefp) sd_bus *bus = NULL;
        int r;

        setlocale(LC_ALL, "");
        log_setup();

        if (invoked_as(argv, "resolvconf"))
                r = resolvconf_parse_argv(argc, argv);
        else if (invoked_as(argv, "systemd-resolve"))
                r = compat_parse_argv(argc, argv);
        else
                r = native_parse_argv(argc, argv);
        if (r <= 0)
                return r;

        r = sd_bus_open_system(&bus);
        if (r < 0)
                return log_error_errno(r, "sd_bus_open_system: %m");

        if (STR_IN_SET(program_invocation_short_name, "systemd-resolve", "resolvconf"))
                return compat_main(argc, argv, bus);

        return native_main(argc, argv, bus);
}

DEFINE_MAIN_FUNCTION(run);
