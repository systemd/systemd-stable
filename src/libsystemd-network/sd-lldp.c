/***
    This file is part of systemd.

    Copyright (C) 2014 Tom Gundersen
    Copyright (C) 2014 Susant Sahani

    systemd is free software; you can redistribute it and/or modify it
    under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.

    systemd is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <arpa/inet.h>
#include <linux/sockios.h>

#include "sd-lldp.h"

#include "alloc-util.h"
#include "fd-util.h"
#include "fileio.h"
#include "hashmap.h"
#include "lldp-internal.h"
#include "lldp-port.h"
#include "lldp-tlv.h"
#include "prioq.h"
#include "siphash24.h"
#include "string-util.h"

typedef enum LLDPAgentRXState {
        LLDP_AGENT_RX_WAIT_PORT_OPERATIONAL = 4,
        LLDP_AGENT_RX_DELETE_AGED_INFO,
        LLDP_AGENT_RX_LLDP_INITIALIZE,
        LLDP_AGENT_RX_WAIT_FOR_FRAME,
        LLDP_AGENT_RX_RX_FRAME,
        LLDP_AGENT_RX_DELETE_INFO,
        LLDP_AGENT_RX_UPDATE_INFO,
        _LLDP_AGENT_RX_STATE_MAX,
        _LLDP_AGENT_RX_INVALID = -1,
} LLDPAgentRXState;

/* Section 10.5.2.2 Reception counters */
struct lldp_agent_statistics {
        uint64_t stats_ageouts_total;
        uint64_t stats_frames_discarded_total;
        uint64_t stats_frames_in_errors_total;
        uint64_t stats_frames_in_total;
        uint64_t stats_tlvs_discarded_total;
        uint64_t stats_tlvs_unrecognized_total;
};

struct sd_lldp {
        lldp_port *port;

        Prioq *by_expiry;
        Hashmap *neighbour_mib;

        sd_lldp_cb_t cb;

        void *userdata;

        LLDPAgentRXState rx_state;
        lldp_agent_statistics statistics;
};

static void chassis_id_hash_func(const void *p, struct siphash *state) {
        const lldp_chassis_id *id = p;

        assert(id);
        assert(id->data);

        siphash24_compress(&id->length, sizeof(id->length), state);
        siphash24_compress(id->data, id->length, state);
}

static int chassis_id_compare_func(const void *_a, const void *_b) {
        const lldp_chassis_id *a, *b;

        a = _a;
        b = _b;

        assert(!a->length || a->data);
        assert(!b->length || b->data);

        if (a->type != b->type)
                return -1;

        if (a->length != b->length)
                return a->length < b->length ? -1 : 1;

        return memcmp(a->data, b->data, a->length);
}

static const struct hash_ops chassis_id_hash_ops = {
        .hash = chassis_id_hash_func,
        .compare = chassis_id_compare_func
};

static void lldp_mib_delete_objects(sd_lldp *lldp);
static void lldp_set_state(sd_lldp *lldp, LLDPAgentRXState state);
static void lldp_run_state_machine(sd_lldp *ll);

static int lldp_receive_frame(sd_lldp *lldp, tlv_packet *tlv) {
        int r;

        assert(lldp);
        assert(tlv);

        /* Remove expired packets */
        if (prioq_size(lldp->by_expiry) > 0) {

                lldp_set_state(lldp, LLDP_AGENT_RX_DELETE_INFO);

                lldp_mib_delete_objects(lldp);
        }

        r = lldp_mib_add_objects(lldp->by_expiry, lldp->neighbour_mib, tlv);
        if (r < 0)
                goto out;

        lldp_set_state(lldp, LLDP_AGENT_RX_UPDATE_INFO);

        log_lldp("Packet added. MIB size: %d , PQ size: %d",
                 hashmap_size(lldp->neighbour_mib),
                 prioq_size(lldp->by_expiry));

        lldp->statistics.stats_frames_in_total ++;

 out:
        if (r < 0)
                log_lldp("Receive frame failed: %s", strerror(-r));

        lldp_set_state(lldp, LLDP_AGENT_RX_WAIT_FOR_FRAME);

        return 0;
}

/* 10.3.2 LLDPDU validation: rxProcessFrame() */
int lldp_handle_packet(tlv_packet *tlv, uint16_t length) {
        bool system_description = false, system_name = false, chassis_id = false;
        bool malformed = false, port_id = false, ttl = false, end = false;
        uint16_t type, len, i, l, t;
        lldp_port *port;
        uint8_t *p, *q;
        sd_lldp *lldp;
        int r;

        assert(tlv);
        assert(length > 0);

        port = (lldp_port *) tlv->userdata;
        lldp = (sd_lldp *) port->userdata;

        if (lldp->port->status == LLDP_PORT_STATUS_DISABLED) {
                log_lldp("Port: %s is disabled. Dropping.", lldp->port->ifname);
                goto out;
        }

        lldp_set_state(lldp, LLDP_AGENT_RX_RX_FRAME);

        p = tlv->pdu;
        p += sizeof(struct ether_header);

        for (i = 1, l = 0; l <= length; i++) {

                memcpy(&t, p, sizeof(uint16_t));

                type = ntohs(t) >> 9;
                len = ntohs(t) & 0x01ff;

                if (type == LLDP_TYPE_END) {
                        if (len != 0) {
                                log_lldp("TLV type end must be length 0 (not %d). Dropping.", len);

                                malformed = true;
                                goto out;
                        }

                        end = true;

                        break;
                } else if (type >=_LLDP_TYPE_MAX) {
                        log_lldp("TLV type: %d not recognized. Dropping.", type);

                        malformed = true;
                        goto out;
                }

                /* skip type and length encoding */
                p += 2;
                q = p;

                p += len;
                l += (len + 2);

                if (i <= 3) {
                        if (i != type) {
                                log_lldp("TLV missing or out of order. Dropping.");

                                malformed = true;
                                goto out;
                        }
                }

                switch(type) {
                case LLDP_TYPE_CHASSIS_ID:

                        if (len < 2) {
                                log_lldp("Received malformed Chassis ID TLV length: %d. Dropping.", len);

                                malformed = true;
                                goto out;
                        }

                        if (chassis_id) {
                                log_lldp("Duplicate Chassis ID TLV found. Dropping.");

                                malformed = true;
                                goto out;
                        }

                        /* Look what subtype it has */
                        if (*q == LLDP_CHASSIS_SUBTYPE_RESERVED || *q > LLDP_CHASSIS_SUBTYPE_LOCALLY_ASSIGNED) {
                                log_lldp("Unknown subtype: %d found in Chassis ID TLV. Dropping.", *q);

                                malformed = true;
                                goto out;

                        }

                        chassis_id = true;

                        break;
                case LLDP_TYPE_PORT_ID:

                        if (len < 2) {
                                log_lldp("Received malformed Port ID TLV length: %d. Dropping.", len);

                                malformed = true;
                                goto out;
                        }

                        if (port_id) {
                                log_lldp("Duplicate Port ID TLV found. Dropping.");

                                malformed = true;
                                goto out;
                        }

                        /* Look what subtype it has */
                        if (*q == LLDP_PORT_SUBTYPE_RESERVED || *q > LLDP_PORT_SUBTYPE_LOCALLY_ASSIGNED) {
                                log_lldp("Unknown subtype: %d found in Port ID TLV. Dropping.", *q);

                                malformed = true;
                                goto out;

                        }

                        port_id = true;

                        break;
                case LLDP_TYPE_TTL:

                        if(len != 2) {
                                log_lldp("Received invalid TTL TLV lenth: %d. Dropping.", len);

                                malformed = true;
                                goto out;
                        }

                        if (ttl) {
                                log_lldp("Duplicate TTL TLV found. Dropping.");

                                malformed = true;
                                goto out;
                        }

                        ttl = true;

                        break;
                case LLDP_TYPE_SYSTEM_NAME:

                        /* According to RFC 1035 the length of a FQDN is limited to 255 characters */
                        if (len > 255) {
                                log_lldp("Received invalid system name length: %d. Dropping.", len);
                                malformed = true;
                                goto out;
                        }

                        if (system_name) {
                                log_lldp("Duplicate system name found. Dropping.");
                                malformed = true;
                                goto out;
                        }

                        system_name = true;

                        break;
                case LLDP_TYPE_SYSTEM_DESCRIPTION:

                        /* 0 <= n <= 255 octets */
                        if (len > 255) {
                                log_lldp("Received invalid system description length: %d. Dropping.", len);
                                malformed = true;
                                goto out;
                        }

                        if (system_description) {
                                log_lldp("Duplicate system description found. Dropping.");
                                malformed = true;
                                goto out;
                        }

                        system_description = true;
                        break;
                default:

                        if (len == 0) {
                                log_lldp("TLV type: %d length 0 received. Dropping.", type);

                                malformed = true;
                                goto out;
                        }
                        break;
                }
        }

        if(!chassis_id || !port_id || !ttl || !end) {
                log_lldp("One or more mandatory TLV missing. Dropping.");

                malformed = true;
                goto out;

        }

        r = tlv_packet_parse_pdu(tlv, length);
        if (r < 0) {
                log_lldp("Failed to parse the TLV. Dropping.");

                malformed = true;
                goto out;
        }

        return lldp_receive_frame(lldp, tlv);

 out:
        lldp_set_state(lldp, LLDP_AGENT_RX_WAIT_FOR_FRAME);

        if (malformed) {
                lldp->statistics.stats_frames_discarded_total ++;
                lldp->statistics.stats_frames_in_errors_total ++;
        }

        sd_lldp_packet_unref(tlv);

        return 0;
}

static int ttl_expiry_item_prioq_compare_func(const void *a, const void *b) {
        const lldp_neighbour_port *p = a, *q = b;

        if (p->until < q->until)
                return -1;

        if (p->until > q->until)
                return 1;

        return 0;
}

static void lldp_set_state(sd_lldp *lldp, LLDPAgentRXState state) {

        assert(lldp);
        assert(state < _LLDP_AGENT_RX_STATE_MAX);

        lldp->rx_state = state;

        lldp_run_state_machine(lldp);
}

static void lldp_run_state_machine(sd_lldp *lldp) {
        if (!lldp->cb)
                return;

        switch (lldp->rx_state) {
        case LLDP_AGENT_RX_UPDATE_INFO:
                lldp->cb(lldp, SD_LLDP_EVENT_UPDATE_INFO, lldp->userdata);
                break;
        default:
                break;
        }
}

/* 10.5.5.2.1 mibDeleteObjects ()
 * The mibDeleteObjects () procedure deletes all information in the LLDP remote
 * systems MIB associated with the MSAP identifier if an LLDPDU is received with
 * an rxTTL value of zero (see 10.3.2) or the timing counter rxInfoTTL expires. */

static void lldp_mib_delete_objects(sd_lldp *lldp) {
        lldp_neighbour_port *p;
        usec_t t = 0;

        /* Remove all entries that are past their TTL */
        for (;;) {

                if (prioq_size(lldp->by_expiry) <= 0)
                        break;

                p = prioq_peek(lldp->by_expiry);
                if (!p)
                        break;

                if (t <= 0)
                        t = now(clock_boottime_or_monotonic());

                if (p->until > t)
                        break;

                lldp_neighbour_port_remove_and_free(p);

                lldp->statistics.stats_ageouts_total ++;
        }
}

static void lldp_mib_objects_flush(sd_lldp *lldp) {
        lldp_neighbour_port *p, *q;
        lldp_chassis *c;

        assert(lldp);
        assert(lldp->neighbour_mib);
        assert(lldp->by_expiry);

        /* Drop all packets */
        while ((c = hashmap_steal_first(lldp->neighbour_mib))) {

                LIST_FOREACH_SAFE(port, p, q, c->ports) {
                        lldp_neighbour_port_remove_and_free(p);
                }
        }

        assert(hashmap_size(lldp->neighbour_mib) == 0);
        assert(prioq_size(lldp->by_expiry) == 0);
}

int sd_lldp_save(sd_lldp *lldp, const char *lldp_file) {
        _cleanup_free_ char *temp_path = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        uint8_t *mac, *port_id, type;
        lldp_neighbour_port *p;
        uint16_t data = 0, length = 0;
        char buf[LINE_MAX];
        lldp_chassis *c;
        usec_t time;
        Iterator i;
        int r;

        assert(lldp);
        assert(lldp_file);

        r = fopen_temporary(lldp_file, &f, &temp_path);
        if (r < 0)
                goto fail;

        fchmod(fileno(f), 0644);

        HASHMAP_FOREACH(c, lldp->neighbour_mib, i) {
                LIST_FOREACH(port, p, c->ports) {
                        _cleanup_free_ char *s = NULL;
                        char *k, *t;

                        r = sd_lldp_packet_read_chassis_id(p->packet, &type, &mac, &length);
                        if (r < 0)
                                continue;

                        sprintf(buf, "'_Chassis=%02x:%02x:%02x:%02x:%02x:%02x' '_CType=%d' ",
                                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], type);

                        s = strdup(buf);
                        if (!s) {
                                r = -ENOMEM;
                                goto fail;
                        }

                        r = sd_lldp_packet_read_port_id(p->packet, &type, &port_id, &length);
                        if (r < 0)
                                continue;

                        if (type != LLDP_PORT_SUBTYPE_MAC_ADDRESS) {
                                k = strndup((char *) port_id, length -1);
                                if (!k) {
                                        r = -ENOMEM;
                                        goto fail;
                                }

                                sprintf(buf, "'_Port=%s' '_PType=%d' ", k , type);
                                free(k);
                        } else {
                                mac = port_id;
                                sprintf(buf, "'_Port=%02x:%02x:%02x:%02x:%02x:%02x' '_PType=%d' ",
                                        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], type);
                        }

                        k = strappend(s, buf);
                        if (!k) {
                                r = -ENOMEM;
                                goto fail;
                        }

                        free(s);
                        s = k;

                        time = now(clock_boottime_or_monotonic());

                        /* Don't write expired packets */
                        if (time - p->until <= 0)
                                continue;

                        sprintf(buf, "'_TTL="USEC_FMT"' ", p->until);

                        k = strappend(s, buf);
                        if (!k) {
                                r = -ENOMEM;
                                goto fail;
                        }

                        free(s);
                        s = k;

                        r = sd_lldp_packet_read_system_name(p->packet, &k, &length);
                        if (r < 0)
                                k = strappend(s, "'_NAME=N/A' ");
                        else {
                                t = strndup(k, length);
                                if (!t) {
                                        r = -ENOMEM;
                                        goto fail;
                                }

                                k = strjoin(s, "'_NAME=", t, "' ", NULL);
                                free(t);
                        }

                        if (!k) {
                                r = -ENOMEM;
                                goto fail;
                        }

                        free(s);
                        s = k;

                        (void) sd_lldp_packet_read_system_capability(p->packet, &data);

                        sprintf(buf, "'_CAP=%x'", data);

                        k = strappend(s, buf);
                        if (!k) {
                                r = -ENOMEM;
                                goto fail;
                        }

                        free(s);
                        s = k;

                        fprintf(f, "%s\n", s);
                }
        }

        r = fflush_and_check(f);
        if (r < 0)
                goto fail;

        if (rename(temp_path, lldp_file) < 0) {
                r = -errno;
                goto fail;
        }

        return 0;

 fail:
        if (temp_path)
                (void) unlink(temp_path);

        return log_error_errno(r, "Failed to save lldp data %s: %m", lldp_file);
}

int sd_lldp_start(sd_lldp *lldp) {
        int r;

        assert_return(lldp, -EINVAL);
        assert_return(lldp->port, -EINVAL);

        lldp->port->status = LLDP_PORT_STATUS_ENABLED;

        lldp_set_state(lldp, LLDP_AGENT_RX_LLDP_INITIALIZE);

        r = lldp_port_start(lldp->port);
        if (r < 0) {
                log_lldp("Failed to start Port : %s , %s",
                         lldp->port->ifname,
                         strerror(-r));

                lldp_set_state(lldp, LLDP_AGENT_RX_WAIT_PORT_OPERATIONAL);

                return r;
        }

        lldp_set_state(lldp, LLDP_AGENT_RX_WAIT_FOR_FRAME);

        return 0;
}

int sd_lldp_stop(sd_lldp *lldp) {
        int r;

        assert_return(lldp, -EINVAL);
        assert_return(lldp->port, -EINVAL);

        lldp->port->status = LLDP_PORT_STATUS_DISABLED;

        r = lldp_port_stop(lldp->port);
        if (r < 0)
                return r;

        lldp_mib_objects_flush(lldp);

        return 0;
}

int sd_lldp_attach_event(sd_lldp *lldp, sd_event *event, int priority) {
        int r;

        assert_return(lldp, -EINVAL);
        assert_return(!lldp->port->event, -EBUSY);

        if (event)
                lldp->port->event = sd_event_ref(event);
        else {
                r = sd_event_default(&lldp->port->event);
                if (r < 0)
                        return r;
        }

        lldp->port->event_priority = priority;

        return 0;
}

int sd_lldp_detach_event(sd_lldp *lldp) {

        assert_return(lldp, -EINVAL);

        lldp->port->event = sd_event_unref(lldp->port->event);

        return 0;
}

int sd_lldp_set_callback(sd_lldp *lldp, sd_lldp_cb_t cb, void *userdata) {
        assert_return(lldp, -EINVAL);

        lldp->cb = cb;
        lldp->userdata = userdata;

        return 0;
}

sd_lldp* sd_lldp_unref(sd_lldp *lldp) {

        if (!lldp)
                return NULL;

        /* Drop all packets */
        lldp_mib_objects_flush(lldp);

        lldp_port_free(lldp->port);

        hashmap_free(lldp->neighbour_mib);
        prioq_free(lldp->by_expiry);

        free(lldp);
        return NULL;
}

int sd_lldp_new(int ifindex,
                const char *ifname,
                const struct ether_addr *mac,
                sd_lldp **ret) {
        _cleanup_(sd_lldp_unrefp) sd_lldp *lldp = NULL;
        int r;

        assert_return(ret, -EINVAL);
        assert_return(ifindex > 0, -EINVAL);
        assert_return(ifname, -EINVAL);
        assert_return(mac, -EINVAL);

        lldp = new0(sd_lldp, 1);
        if (!lldp)
                return -ENOMEM;

        r = lldp_port_new(ifindex, ifname, mac, lldp, &lldp->port);
        if (r < 0)
                return r;

        lldp->neighbour_mib = hashmap_new(&chassis_id_hash_ops);
        if (!lldp->neighbour_mib)
                return -ENOMEM;

        r = prioq_ensure_allocated(&lldp->by_expiry,
                                   ttl_expiry_item_prioq_compare_func);
        if (r < 0)
                return r;

        lldp->rx_state = LLDP_AGENT_RX_WAIT_PORT_OPERATIONAL;

        *ret = lldp;
        lldp = NULL;

        return 0;
}

int sd_lldp_get_packets(sd_lldp *lldp, sd_lldp_packet ***tlvs) {
        lldp_neighbour_port *p;
        lldp_chassis *c;
        Iterator iter;
        unsigned count = 0, i;

        assert_return(lldp, -EINVAL);
        assert_return(tlvs, -EINVAL);

        HASHMAP_FOREACH(c, lldp->neighbour_mib, iter) {
                LIST_FOREACH(port, p, c->ports)
                        count++;
        }

        if (!count) {
                *tlvs = NULL;
                return 0;
        }

        *tlvs = new(sd_lldp_packet *, count);
        if (!*tlvs)
                return -ENOMEM;

        i = 0;
        HASHMAP_FOREACH(c, lldp->neighbour_mib, iter) {
                LIST_FOREACH(port, p, c->ports)
                        (*tlvs)[i++] = sd_lldp_packet_ref(p->packet);
        }

        return count;
}
