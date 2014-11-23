/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

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

#include "siphash24.h"
#include "hashmap.h"
#include "event-util.h"

#include "lldp-tlv.h"
#include "lldp-port.h"
#include "sd-lldp.h"
#include "prioq.h"
#include "lldp-internal.h"

/* Section 10.5.2.2 Reception counters */
struct lldp_agent_statitics {
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

        lldp_agent_statitics statitics;
};

static unsigned long chassis_id_hash_func(const void *p,
                                          const uint8_t hash_key[HASH_KEY_SIZE]) {
        uint64_t u;
        const lldp_chassis_id *id = p;

        assert(id);

        siphash24((uint8_t *) &u, id->data, id->length, hash_key);

        return (unsigned long) u;
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

static int lldp_receive_frame(sd_lldp *lldp, tlv_packet *tlv) {
        int r;

        assert(lldp);
        assert(tlv);

        /* Remove expired packets */
        if (prioq_size(lldp->by_expiry) > 0)
                lldp_mib_delete_objects(lldp);

        r = lldp_mib_add_objects(lldp->by_expiry, lldp->neighbour_mib, tlv);
        if (r < 0)
                goto out;

        log_lldp("Packet added. MIB size: %d , PQ size: %d",
                 hashmap_size(lldp->neighbour_mib),
                 prioq_size(lldp->by_expiry));

        lldp->statitics.stats_frames_in_total ++;

        return 0;

 out:
        if (r < 0)
                log_lldp("Receive frame failed: %s", strerror(-r));

        return 0;
}

/* 10.3.2 LLDPDU validation: rxProcessFrame() */
int lldp_handle_packet(tlv_packet *tlv, uint16_t length) {
        uint16_t type, len, i, l, t;
        bool chassis_id = false;
        bool malformed = false;
        bool port_id = false;
        bool ttl = false;
        bool end = false;
        lldp_port *port;
        uint8_t *p, *q;
        sd_lldp *lldp;
        int r;

        assert(tlv);
        assert(length > 0);

        port = (lldp_port *) tlv->userdata;
        lldp = (sd_lldp *) port->userdata;

        if (lldp->port->status == LLDP_PORT_STATUS_DISABLED) {
                log_lldp("Port is disabled : %s . Dropping ...",
                         lldp->port->ifname);
                goto out;
        }

        p = tlv->pdu;
        p += sizeof(struct ether_header);

        for (i = 1, l = 0; l <= length; i++) {

                memcpy(&t, p, sizeof(uint16_t));

                type = ntohs(t) >> 9;
                len = ntohs(t) & 0x01ff;

                if (type == LLDP_TYPE_END) {
                        if (len != 0) {
                                log_lldp("TLV type end is not length 0. Length:%d received . Dropping ...",
                                         len);

                                malformed = true;
                                goto out;
                        }

                        end = true;

                        break;
                } else if (type >=_LLDP_TYPE_MAX) {
                        log_lldp("TLV type not recognized %d . Dropping ...",
                                 type);

                        malformed = true;
                        goto out;
                }

                /* skip type and lengh encoding */
                p += 2;
                q = p;

                p += len;
                l += (len + 2);

                if (i <= 3) {
                        if (i != type) {
                                log_lldp("TLV missing or out of order. Dropping ...");

                                malformed = true;
                                goto out;
                        }
                }

                switch(type) {
                case LLDP_TYPE_CHASSIS_ID:

                        if (len < 2) {
                                log_lldp("Received malformed Chassis ID TLV len = %d. Dropping",
                                         len);

                                malformed = true;
                                goto out;
                        }

                        if (chassis_id) {
                                log_lldp("Duplicate Chassis ID TLV found. Dropping ...");

                                malformed = true;
                                goto out;
                        }

                        /* Look what subtype it has */
                        if (*q == LLDP_CHASSIS_SUBTYPE_RESERVED ||
                            *q > LLDP_CHASSIS_SUBTYPE_LOCALLY_ASSIGNED) {
                                log_lldp("Unknown subtype: %d found in Chassis ID TLV . Dropping ...",
                                         *q);

                                malformed = true;
                                goto out;

                        }

                        chassis_id = true;

                        break;
                case LLDP_TYPE_PORT_ID:

                        if (len < 2) {
                                log_lldp("Received malformed Port ID TLV len = %d. Dropping",
                                         len);

                                malformed = true;
                                goto out;
                        }

                        if (port_id) {
                                log_lldp("Duplicate Port ID TLV found. Dropping ...");

                                malformed = true;
                                goto out;
                        }

                        /* Look what subtype it has */
                        if (*q == LLDP_PORT_SUBTYPE_RESERVED ||
                            *q > LLDP_PORT_SUBTYPE_LOCALLY_ASSIGNED) {
                                log_lldp("Unknown subtype: %d found in Port ID TLV . Dropping ...",
                                         *q);

                                malformed = true;
                                goto out;

                        }

                        port_id = true;

                        break;
                case LLDP_TYPE_TTL:

                        if(len != 2) {
                                log_lldp(
                                         "Received invalid lenth: %d TTL TLV. Dropping ...",
                                         len);

                                malformed = true;
                                goto out;
                        }

                        if (ttl) {
                                log_lldp("Duplicate TTL TLV found. Dropping ...");

                                malformed = true;
                                goto out;
                        }

                        ttl = true;

                        break;
                default:

                        if (len == 0) {
                                log_lldp("TLV type = %d's, length 0 received . Dropping ...",
                                         type);

                                malformed = true;
                                goto out;
                        }
                        break;
                }
        }

        if(!chassis_id || !port_id || !ttl || !end) {
                log_lldp( "One or more mandotory TLV missing . Dropping ...");

                malformed = true;
                goto out;

        }

        r = tlv_packet_parse_pdu(tlv, length);
        if (r < 0) {
                log_lldp( "Failed to parse the TLV. Dropping ...");

                malformed = true;
                goto out;
        }

        return lldp_receive_frame(lldp, tlv);

 out:
        if (malformed) {
                lldp->statitics.stats_frames_discarded_total ++;
                lldp->statitics.stats_frames_in_errors_total ++;
        }

        tlv_packet_free(tlv);

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
                        t = now(CLOCK_BOOTTIME);

                if (p->until > t)
                        break;

                lldp_neighbour_port_remove_and_free(p);

                lldp->statitics.stats_ageouts_total ++;
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

int sd_lldp_start(sd_lldp *lldp) {
        int r;

        assert_return(lldp, -EINVAL);
        assert_return(lldp->port, -EINVAL);

        lldp->port->status = LLDP_PORT_STATUS_ENABLED;

        r = lldp_port_start(lldp->port);
        if (r < 0) {
                log_lldp("Failed to start Port : %s , %s",
                         lldp->port->ifname,
                         strerror(-r));
                return r;
        }

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

void sd_lldp_free(sd_lldp *lldp) {

        if (!lldp)
                return;

        /* Drop all packets */
        lldp_mib_objects_flush(lldp);

        lldp_port_free(lldp->port);

        hashmap_free(lldp->neighbour_mib);
        prioq_free(lldp->by_expiry);

        free(lldp);
}

int sd_lldp_new(int ifindex,
                char *ifname,
                struct ether_addr *mac,
                sd_lldp **ret) {
        _cleanup_sd_lldp_free_ sd_lldp *lldp = NULL;
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

        *ret = lldp;
        lldp = NULL;

        return 0;
}