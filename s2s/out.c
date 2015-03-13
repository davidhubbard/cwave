/*
 * jabberd - Jabber Open Source Server
 * Copyright (c) 2002 Jeremie Miller, Thomas Muldowney,
 *                    Ryan Eatmon, Robert Norris
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA02111-1307USA
 */

#include "s2s.h"

#include <idna.h>

/*
 * we handle packets going from the router to the world, and stuff
 * that comes in on connections we initiated.
 *
 * action points:
 *
 *   out_packet(s2s, nad) - send this packet out
 *     - extract to domain
 *     - get dbconn for this domain using out_route
 *       - if dbconn not available bounce packet
 *       - DONE
 *     - if conn in progress (tcp)
 *       - add packet to queue for this domain
 *       - DONE
 *     - if dbconn state valid for this domain, or packet is dialback
 *       - send packet
 *       - DONE
 *     - if dbconn state invalid for this domain
 *       - bounce packet (502)
 *       - DONE
 *     - add packet to queue for this domain
 *     - if dbconn state inprogress for this domain
 *       - DONE
 *     - out_dialback(dbconn, from, to)
 *
 *   out_route(s2s, route, out, allow_bad)
 *     - if dbconn not found
 *       - check internal resolver cache for domain
 *       - if not found
 *         - ask resolver for name
 *         - DONE
 *       - if outgoing ip/port is to be reused
 *         - get dbconn for any valid ip/port
 *         - if dbconn not found
 *            - create new dbconn
 *            - initiate connect to ip/port
 *            - DONE
 *       - create new dbconn
 *       - initiate connect to ip/port
 *       - DONE
 *
 *   out_dialback(dbconn, from, to) - initiate dialback
 *     - generate dbkey: sha1(secret+remote+stream id)
 *     - send auth request: <result to='them' from='us'>dbkey</result>
 *     - set dbconn state for this domain to inprogress
 *     - DONE
 *
 *   out_resolve(s2s, query) - responses from resolver
 *     - store ip/port/ttl in resolver cache
 *     - flush domain queue -> out_packet(s2s, domain)
 *     - DONE
 *
 *   event_STREAM - ip/port open
 *     - get dbconn for this sx
 *     - for each route handled by this conn, out_dialback(dbconn, from, to)
 *     - DONE
 *
 *   event_PACKET: <result from='them' to='us' type='xxx'/> - response to our auth request
 *     - get dbconn for this sx
 *     - if type valid
 *       - set dbconn state for this domain to valid
 *       - flush dbconn queue for this domain -> out_packet(s2s, pkt)
 *       - DONE
 *     - set dbconn state for this domain to invalid
 *     - bounce dbconn queue for this domain (502)
 *     - DONE
 *
 *   event_PACKET: <verify from='them' to='us' id='123' type='xxx'/> - incoming stream authenticated
 *     - get dbconn for given id
 *     - if type is valid
 *       - set dbconn state for this domain to valid
 *     - send result: <result to='them' from='us' type='xxx'/>
 *     - DONE
 */

/* forward decls */
static int _out_mio_callback(mio_t m, mio_action_t a, mio_fd_t fd, void *data, void *arg);
static int _out_sx_callback(sx_t s, sx_event_t e, void *data, void *arg);
static void _out_result(conn_t out, nad_t nad);
static void _out_verify(conn_t out, nad_t nad);
static void _dns_result_aaaa(void *data, int err, struct ub_result *result);
static void _dns_result_a(void *data, int err, struct ub_result *result);

/** queue the packet */
static void _out_packet_queue(s2s_t s2s, pkt_t pkt) {
    char *rkey = s2s_route_key(NULL, pkt->from->domain, pkt->to->domain);
    jqueue_t q = (jqueue_t) xhash_get(s2s->outq, rkey);

    if(q == NULL) {
        log_debug(ZONE, "creating new out packet queue for '%s'", rkey);
        q = jqueue_new();
        q->key = rkey;
        xhash_put(s2s->outq, q->key, (void *) q);
    } else {
        free(rkey);
    }

    log_debug(ZONE, "queueing packet for '%s'", q->key);

    jqueue_push(q, (void *) pkt, 0);
}

static void _out_dialback(conn_t out, char *rkey) {
    char *c, *dbkey;
    nad_t nad;
    int ns;
    time_t now;

    now = time(NULL);

    c = strchr(rkey, '/');
    *c = '\0';
    c++;
    
    /* kick off the dialback */
    dbkey = s2s_db_key(NULL, out->s2s->local_secret, c, out->s->id);

    nad = nad_new();

    /* request auth */
    ns = nad_add_namespace(nad, uri_DIALBACK, "db");
    nad_append_elem(nad, ns, "result", 0);
    nad_append_attr(nad, -1, "from", rkey);
    nad_append_attr(nad, -1, "to", c);
    nad_append_cdata(nad, dbkey, strlen(dbkey), 1);

    c--;
    *c = '/';

    log_debug(ZONE, "sending auth request for %s (key %s)", rkey, dbkey);
    log_write(out->s2s->log, LOG_NOTICE, "[%d] [%s, port=%d] sending dialback auth request for route '%s'", out->fd->fd, out->ip, out->port, rkey);

    /* off it goes */
    sx_nad_write(out->s, nad);

    free(dbkey);
            
    /* we're in progress now */
    xhash_put(out->states, pstrdup(xhash_pool(out->states), rkey), (void *) conn_INPROGRESS);

    /* record the time that we set conn_INPROGRESS state */
    xhash_put(out->states_time, pstrdup(xhash_pool(out->states_time), rkey), (void *) now);
}

int dns_select(s2s_t s2s, char *ip, int *port, time_t now, dnscache_t dns, int allow_bad) {
    /* list of results */
    dnsres_t l_reuse[DNS_MAX_RESULTS];
    dnsres_t l_aaaa[DNS_MAX_RESULTS];
    dnsres_t l_a[DNS_MAX_RESULTS];
    dnsres_t l_bad[DNS_MAX_RESULTS];
    /* running weight sums of results */
    int rw_reuse[DNS_MAX_RESULTS];
    int rw_aaaa[DNS_MAX_RESULTS];
    int rw_a[DNS_MAX_RESULTS];
    int s_reuse = 0, s_aaaa = 0, s_a = 0, s_bad = 0; /* count */
    int p_reuse = 0, p_aaaa = 0, p_a = 0; /* list prio */
    int wt_reuse = 0, wt_aaaa = 0, wt_a = 0; /* weight total */
    int c_expired_good = 0;
    union xhashv xhv;
    dnsres_t res;
    char *c;

    /* for all results:
     * - if not expired
     *   - put highest priority reuseable addrs into list1
     *   - put highest priority ipv6 addrs into list2
     *   - put highest priority ipv4 addrs into list3
     *   - put bad addrs into list4
     * - pick weighted random entry from first non-empty list
     */

    if (dns->results == NULL) {
        log_debug(ZONE, "negative cache entry for '%s'", dns->name);
        return -1;
    }
    log_debug(ZONE, "selecting DNS result for '%s'", dns->name);

    xhv.dnsres_val = &res;
    if (xhash_iter_first(dns->results)) {
        dnsres_t bad = NULL;
        do {
            char *ipport;
            int ipport_len;
            xhash_iter_get(dns->results, (const char **) &ipport, &ipport_len, xhv.val);

            if (s2s->dns_bad_timeout > 0)
                bad = xhash_getx(s2s->dns_bad, ipport, ipport_len);

            if (now > res->expiry) {
                /* good host? */
                if (bad == NULL)
                    c_expired_good++;

                log_debug(ZONE, "host '%s' expired", res->key);
                continue;
            } else if (bad != NULL && !(now > bad->expiry)) {
                /* bad host (connection failure) */
                l_bad[s_bad++] = res;

                log_debug(ZONE, "host '%s' bad", res->key);
            } else if (s2s->out_reuse && xhash_getx(s2s->out_host, ipport, ipport_len) != NULL) {
                /* existing connection */
                log_debug(ZONE, "host '%s' exists", res->key);
                if (s_reuse == 0 || p_reuse > res->prio) {
                    p_reuse = res->prio;
                    s_reuse = 0;
                    wt_reuse = 0;

                    log_debug(ZONE, "reset prio list, using prio %d", res->prio);
                }
                if (res->prio <= p_reuse) {
                    l_reuse[s_reuse] = res;
                    wt_reuse += res->weight;
                    rw_reuse[s_reuse] = wt_reuse;
                    s_reuse++;

                    log_debug(ZONE, "added host with weight %d (%d), running weight %d",
                        (res->weight >> 8), res->weight, wt_reuse);
                } else {
                    log_debug(ZONE, "ignored host with prio %d", res->prio);
                }
            } else if (memchr(ipport, ':', ipport_len) != NULL) {
                /* ipv6 */
                log_debug(ZONE, "host '%s' IPv6", res->key);
                if (s_aaaa == 0 || p_aaaa > res->prio) {
                    p_aaaa = res->prio;
                    s_aaaa = 0;
                    wt_aaaa = 0;

                    log_debug(ZONE, "reset prio list, using prio %d", res->prio);
                }
                if (res->prio <= p_aaaa) {
                    l_aaaa[s_aaaa] = res;
                    wt_aaaa += res->weight;
                    rw_aaaa[s_aaaa] = wt_aaaa;
                    s_aaaa++;

                    log_debug(ZONE, "added host with weight %d (%d), running weight %d",
                        (res->weight >> 8), res->weight, wt_aaaa);
                } else {
                    log_debug(ZONE, "ignored host with prio %d", res->prio);
                }
            } else {
                /* ipv4 */
                log_debug(ZONE, "host '%s' IPv4", res->key);
                if (s_a == 0 || p_a > res->prio) {
                    p_a = res->prio;
                    s_a = 0;
                    wt_a = 0;

                    log_debug(ZONE, "reset prio list, using prio %d", res->prio);
                }
                if (res->prio <= p_a) {
                    l_a[s_a] = res;
                    wt_a += res->weight;
                    rw_a[s_a] = wt_a;
                    s_a++;

                    log_debug(ZONE, "added host with weight %d (%d), running weight %d",
                        (res->weight >> 8), res->weight, wt_a);
                } else {
                    log_debug(ZONE, "ignored host with prio %d", res->prio);
                }
            }
        } while(xhash_iter_next(dns->results));
    }

    /* pick a result at weighted random (RFC 2782)
     * all weights are guaranteed to be >= 16 && <= 16776960
     * (assuming max 50 hosts, the total/running sums won't exceed 2^31)
     */
    char * ipport = NULL;
    if (s_reuse > 0) {
        int i, r;

        log_debug(ZONE, "using existing hosts, total weight %d", wt_reuse);
        assert((wt_reuse + 1) > 0);

        r = rand() % (wt_reuse + 1);
        log_debug(ZONE, "random number %d", r);

        for (i = 0; i < s_reuse; i++)
            if (rw_reuse[i] >= r) {
                log_debug(ZONE, "selected host '%s', running weight %d",
                    l_reuse[i]->key, rw_reuse[i]);

                ipport = l_reuse[i]->key;
                break;
            }
    } else if (s_aaaa > 0 && (s_a == 0 || p_aaaa <= p_a)) {
        int i, r;

        log_debug(ZONE, "using IPv6 hosts, total weight %d", wt_aaaa);
        assert((wt_aaaa + 1) > 0);

        r = rand() % (wt_aaaa + 1);
        log_debug(ZONE, "random number %d", r);

        for (i = 0; i < s_aaaa; i++)
            if (rw_aaaa[i] >= r) {
                log_debug(ZONE, "selected host '%s', running weight %d",
                    l_aaaa[i]->key, rw_aaaa[i]);

                ipport = l_aaaa[i]->key;
                break;
            }
    } else if (s_a > 0) {
        int i, r;

        log_debug(ZONE, "using IPv4 hosts, total weight %d", wt_a);
        assert((wt_a + 1) > 0);

        r = rand() % (wt_a + 1);
        log_debug(ZONE, "random number %d", r);

        for (i = 0; i < s_a; i++)
            if (rw_a[i] >= r) {
                log_debug(ZONE, "selected host '%s', running weight %d",
                    l_a[i]->key, rw_a[i]);

                ipport = l_a[i]->key;
                break;
            }
    } else if (s_bad > 0) {
        ipport = l_bad[rand() % s_bad]->key;

        log_debug(ZONE, "using bad hosts, allow_bad=%d", allow_bad);

        /* there are expired good hosts, expire cache immediately */
        if (c_expired_good > 0) {
            log_debug(ZONE, "expiring this DNS cache entry, %d expired hosts",
                c_expired_good);

            dns->expiry = 0;
        }

        if (!allow_bad)
            return -1;
    }

    /* results cannot all expire before the collection does */
    assert(ipport != NULL);

    /* copy the ip and port to the packet */
    c = strchr(ipport, '/');
    *c = '\0';
    c++;

    strcpy(ip, ipport);
    *port = atoi(c);

    c--;
    *c = '/';

    return 0;
}

/** find/make a connection for a route */
int out_route(s2s_t s2s, char *route, conn_t *out, int allow_bad) {
    dnscache_t dns;
    char ipport[INET6_ADDRSTRLEN + 16], *dkey, *c;
    time_t now;
    int reuse = 0;
    char ip[INET6_ADDRSTRLEN];
    int port;

    c = strchr(route, '/');
    dkey = strdup(c+1);

    log_debug(ZONE, "trying to find connection for '%s'", dkey);
    *out = (conn_t) xhash_get(s2s->out_dest, dkey);
    if(*out == NULL) {
        log_debug(ZONE, "connection for '%s' not found", dkey);

        /* check resolver cache for ip/port */
        dns = xhash_get(s2s->dnscache, dkey);
        if(dns == NULL) {
            /* new resolution */
            log_debug(ZONE, "no dns for %s, preparing for resolution", dkey);

            dns = (dnscache_t) calloc(1, sizeof(struct dnscache_st));

            strcpy(dns->name, dkey);

            xhash_put(s2s->dnscache, dns->name, (void *) dns);
        }

        /* resolution in progress */
        if(dns->pending) {
            log_debug(ZONE, "pending resolution");
            free(dkey);
            return 0;
        }

        /* has it expired (this is 0 for new cache objects, so they're always expired */
        now = time(NULL); /* each entry must be expired no earlier than the collection */
        if(now > dns->expiry) {
            /* resolution required */
            log_debug(ZONE, "requesting resolution for %s", dkey);

            dns->init_time = time(NULL);
            dns->pending = 1;

            dns_resolve_domain(s2s, dns);
            free(dkey);
            return 0;
        }

        /* dns is valid */
        if (dns_select(s2s, ip, &port, now, dns, allow_bad)) {
            /* failed to find anything acceptable */
            free(dkey);
            return -1;
        }

        /* re-request resolution if dns_select expired the data */
        if (now > dns->expiry) {
            /* resolution required */
            log_debug(ZONE, "requesting resolution for %s", dkey);

            dns->init_time = time(NULL);
            dns->pending = 1;

            dns_resolve_domain(s2s, dns);

            free(dkey);
            return 0;
        }

        /* generate the ip/port pair, this is the hash key for the conn */
        snprintf(ipport, INET6_ADDRSTRLEN + 16, "%s/%d", ip, port);

        /* try to re-use an existing connection */
        if (s2s->out_reuse)
            *out = (conn_t) xhash_get(s2s->out_host, ipport);

        if (*out != NULL) {
            log_write(s2s->log, LOG_NOTICE, "[%d] [%s, port=%d] using connection for '%s'", (*out)->fd->fd, (*out)->ip, (*out)->port, dkey);

            /* associate existing connection with domain */
            xhash_put(s2s->out_dest, s2s->out_reuse ? pstrdup(xhash_pool((*out)->routes), dkey) : dkey, (void *) *out);

            reuse = 1;
        } else{
            /* no conn, create one */
            *out = (conn_t) calloc(1, sizeof(struct conn_st));

            (*out)->s2s = s2s;

            (*out)->key = strdup(ipport);
            if (s2s->out_reuse)
                (*out)->dkey = NULL;
            else
                (*out)->dkey = dkey;

            strcpy((*out)->ip, ip);
            (*out)->port = port;

            (*out)->states = xhash_new(101);
            (*out)->states_time = xhash_new(101);

            (*out)->routes = xhash_new(101);

            (*out)->init_time = time(NULL);

            if (s2s->out_reuse)
                xhash_put(s2s->out_host, (*out)->key, (void *) *out);
            xhash_put(s2s->out_dest, s2s->out_reuse ? pstrdup(xhash_pool((*out)->routes), dkey) : dkey, (void *) *out);

            xhash_put((*out)->routes, pstrdup(xhash_pool((*out)->routes), route), (void *) 1);

            /* connect */
            log_debug(ZONE, "initiating connection to %s", ipport);

            (*out)->fd = mio_connect(s2s->mio, port, ip, s2s->origin_ip, _out_mio_callback, (void *) *out);

            if ((*out)->fd == NULL) {
                dnsres_t bad;

                log_write(s2s->log, LOG_NOTICE, "[%d] [%s, port=%d] mio_connect error: %s (%d)", -1, (*out)->ip, (*out)->port, MIO_STRERROR(MIO_ERROR), MIO_ERROR);

                if (s2s->dns_bad_timeout > 0) {
                    /* mark this host as bad */
                    bad = xhash_get(s2s->dns_bad, ipport);
                    if (bad == NULL) {
                        bad = (dnsres_t) calloc(1, sizeof(struct dnsres_st));
                        bad->key = strdup(ipport);
                        xhash_put(s2s->dns_bad, bad->key, bad);
                    }
                    bad->expiry = time(NULL) + s2s->dns_bad_timeout;
                }

                if (s2s->out_reuse)
                   xhash_zap(s2s->out_host, (*out)->key);
                xhash_zap(s2s->out_dest, dkey);

                xhash_free((*out)->states);
                xhash_free((*out)->states_time);

                xhash_free((*out)->routes);

                free((*out)->key);
                free((*out)->dkey);
                free(*out);
                *out = NULL;

                /* try again without allowing bad hosts */
                return out_route(s2s, route, out, 0);
            } else {
                log_write(s2s->log, LOG_NOTICE, "[%d] [%s, port=%d] outgoing connection for '%s'", (*out)->fd->fd, (*out)->ip, (*out)->port, dkey);

                (*out)->s = sx_new(s2s->sx_env, (*out)->fd->fd, _out_sx_callback, (void *) *out);

#ifdef HAVE_SSL
                /* Send a stream version of 1.0 if we can do STARTTLS */
                if(s2s->sx_ssl != NULL) {
                    *c = '\0';
                    sx_client_init((*out)->s, S2S_DB_HEADER, uri_SERVER, dkey, route, "1.0");
                    *c = '/';
                } else {
                    sx_client_init((*out)->s, S2S_DB_HEADER, uri_SERVER, NULL, NULL, NULL);
                }
#else
                sx_client_init((*out)->s, S2S_DB_HEADER, uri_SERVER, NULL, NULL, NULL);
#endif
                /* dkey is now used by the hash table */
                return 0;
            }
        }
    } else {
        log_debug(ZONE, "connection for '%s' found (%d %s/%d)", dkey, (*out)->fd->fd, (*out)->ip, (*out)->port);
    }

    /* connection in progress, or re-using connection: add to routes list */
    if (!(*out)->online || reuse) {
        if (xhash_get((*out)->routes, route) == NULL)
            xhash_put((*out)->routes, pstrdup(xhash_pool((*out)->routes), route), (void *) 1);
    }

    free(dkey);
    return 0;
}

void out_pkt_free(pkt_t pkt)
{
    if (pkt->nad) nad_free(pkt->nad);
    jid_free(pkt->from);
    jid_free(pkt->to);
    free(pkt);
}

/** send a packet out */
int out_packet(s2s_t s2s, pkt_t pkt) {
    char *rkey;
    conn_t out;
    conn_state_t state;
    int ret;

    /* new route key */
    rkey = s2s_route_key(NULL, pkt->from->domain, pkt->to->domain);

    /* get a connection */
    ret = out_route(s2s, rkey, &out, 1);

    if (out == NULL) {
        /* connection not available, queue packet */
        _out_packet_queue(s2s, pkt);

        /* check if out_route was successful in attempting a connection */
        if (ret) {
            /* bounce queue */
            out_bounce_route_queue(s2s, rkey, stanza_err_SERVICE_UNAVAILABLE);

            free(rkey);
            return -1;
        }

        free(rkey);
        return 0;
    }

    /* connection in progress */
    if(!out->online) {
        log_debug(ZONE, "connection in progress, queueing packet");

        _out_packet_queue(s2s, pkt);

        free(rkey);
        return 0;
    }

    /* connection state */
    state = (conn_state_t) xhash_get(out->states, rkey);

    /* valid conns or dialback packets */
    if(state == conn_VALID || pkt->db) {
        log_debug(ZONE, "writing packet for %s to outgoing conn %d", rkey, out->fd->fd);

        /* send it straight out */
        if(pkt->db) {
            /* if this is a db:verify packet, increment counter and set timestamp */
            if(NAD_ENAME_L(pkt->nad, 0) == 6 && strncmp("verify", NAD_ENAME(pkt->nad, 0), 6) == 0) {
                out->verify++;
                out->last_verify = time(NULL);
            }

            /* dialback packet */
            sx_nad_write(out->s, pkt->nad);
        } else {
            /* if the outgoing stanza has a jabber:client namespace, remove it so that the stream jabber:server namespaces will apply (XMPP 11.2.2) */
            int ns = nad_find_namespace(pkt->nad, 1, uri_CLIENT, NULL);
            if(ns >= 0) { 
               /* clear the namespaces of elem 0 (internal route element) and elem 1 (message|iq|presence) */
               pkt->nad->elems[0].ns = -1;
               pkt->nad->elems[0].my_ns = -1;
               pkt->nad->elems[1].ns = -1;
               pkt->nad->elems[1].my_ns = -1;
            }

            /* send it out */
            sx_nad_write_elem(out->s, pkt->nad, 1);
        }

        /* update timestamp */
        out->last_packet = time(NULL);

        jid_free(pkt->from);
        jid_free(pkt->to);
        free(pkt);

        free(rkey);
        return 0;
    }

    /* can't be handled yet, queue */
    _out_packet_queue(s2s, pkt);

    /* if dialback is in progress, then we're done for now */
    if(state == conn_INPROGRESS) {
        free(rkey);
        return 0;
    }

    /* this is a new route - send dialback auth request to piggyback on the existing connection */
    _out_dialback(out, rkey);

    free(rkey);
    return 0;
}

char *dns_make_ipport(char *host, int port) {
    char *c;
    assert(port > 0 && port < 65536);

    c = (char *) malloc(strlen(host) + 7);
    sprintf(c, "%s/%d", host, port);
    return c;
}

static void _dns_add_result(dnsquery_t query, char *ip, int port, int prio, int weight, unsigned int ttl) {
    char *ipport = dns_make_ipport(ip, port);
    dnsres_t res = xhash_get(query->results, ipport);

    if (res != NULL) {
        if (prio < res->prio)
            res->prio = prio;

        if (prio < res->prio) {
            /* duplicate host at lower prio - reset weight */
            res->weight = weight;
        } else if (prio == res->prio) {
            /* duplicate host at same prio - add to weight */
            res->weight += weight;
            if (res->weight > (65535 << 8))
                res->weight = (65535 << 8);
        }

        if (ttl > res->expiry)
            res->expiry = ttl;

        if (ttl > query->expiry)
            query->expiry = ttl;

        log_debug(ZONE, "dns result updated for %s@%p: %s (%d/%d/%d)", query->name, query, ipport,
            res->prio, (res->weight >> 8), res->expiry);
    } else if (xhash_count(query->results) < DNS_MAX_RESULTS) {
        res = pmalloc(xhash_pool(query->results), sizeof(struct dnsres_st));
        res->key = pstrdup(xhash_pool(query->results), ipport);
        res->prio = prio;
        res->weight = weight;
        res->expiry = ttl;

        if (ttl > query->expiry)
            query->expiry = ttl;

        xhash_put(query->results, res->key, res);

        log_debug(ZONE, "dns result added for %s@%p: %s (%d/%d/%d)", query->name, query, ipport,
            res->prio, (res->weight >> 8), res->expiry);
    } else {
        log_debug(ZONE, "dns result ignored for %s@%p: %s (%d/%d/%d)", query->name, query, ipport,
            prio, (weight >> 8), ttl);
    }

    free(ipport);
}

static void _dns_add_host(dnsquery_t query, char *ip, int port, int prio, int weight, unsigned int ttl) {
    char *ipport = dns_make_ipport(ip, port);
    dnsres_t res = xhash_get(query->hosts, ipport);

    /* update host weights:
     *  RFC 2482 "In the presence of records containing weights greater
     *  than 0, records with weight 0 should have a very small chance of
     *  being selected."
     * 0       -> 16
     * 1-65535 -> 256-16776960
     */
    if (weight == 0)
        weight = 1 << 4;
    else
        weight <<= 8;

    if (res != NULL) {
        if (prio < res->prio)
            res->prio = prio;

        if (prio < res->prio) {
            /* duplicate host at lower prio - reset weight */
            res->weight = weight;
        } else if (prio == res->prio) {
            /* duplicate host at same prio - add to weight */
            res->weight += weight;
            if (res->weight > (65535 << 8))
                res->weight = (65535 << 8);
        }

        if (ttl > res->expiry)
            res->expiry = ttl;

        log_debug(ZONE, "dns host updated for %s@%p: %s (%d/%d/%d)", query->name, query, ipport,
            res->prio, (res->weight >> 8), res->expiry);
    } else if (xhash_count(query->hosts) < DNS_MAX_RESULTS) {
        res = pmalloc(xhash_pool(query->hosts), sizeof(struct dnsres_st));
        res->key = pstrdup(xhash_pool(query->hosts), ipport);
        res->prio = prio;
        res->weight = weight;
        res->expiry = ttl;

        xhash_put(query->hosts, res->key, res);

        log_debug(ZONE, "dns host added for %s@%p: %s (%d/%d/%d)", query->name, query, ipport,
            res->prio, (res->weight >> 8), res->expiry);
    } else {
        log_debug(ZONE, "dns host ignored for %s@%p: %s (%d/%d/%d)", query->name, query, ipport,
            prio, (weight >> 8), ttl);
    }

    free(ipport);
}

static void _dns_start_aaaa(dnsquery_t query)
{
    int err;
    log_debug(ZONE, "dns request for %s@%p: AAAA %s", query->name, query, query->cur_host ? query->cur_host : query->name);

    err = ub_resolve_async(query->s2s->ub_ctx, query->cur_host ? query->cur_host : query->name, LDNS_RR_TYPE_AAAA /*rrtype*/, LDNS_RR_CLASS_IN /*rrclass*/,
        query, _dns_result_aaaa, &query->async_id);
    query->have_async_id = 1;

    /* if submit failed, call ourselves with the error */
    if (err) _dns_result_aaaa(query, err, NULL);
}

static void _dns_start_a(dnsquery_t query)
{
    int err;
    log_debug(ZONE, "dns request for %s@%p: A %s", query->name, query, query->cur_host ? query->cur_host : query->name);

    err = ub_resolve_async(query->s2s->ub_ctx, query->cur_host ? query->cur_host : query->name, LDNS_RR_TYPE_A /*rrtype*/, LDNS_RR_CLASS_IN /*rrclass*/,
        query, _dns_result_a, &query->async_id);
    query->have_async_id = 1;

    /* if submit failed, call ourselves with the error */
    if (err) _dns_result_a(query, err, NULL);
}

/* this function is called with 0 err and a NULL result to start the SRV process */
static void _dns_result_srv(void *data, int err, struct ub_result *result) {
    dnsquery_t query = data;
    assert(query != NULL);
    query->have_async_id = 0;

    ldns_pkt *pkt = 0;
    ldns_buffer *buf = ldns_buffer_new(1024);
    if (!buf) {
        log_write(query->s2s->log, LOG_ERR, "ldns_buffer(1024) failed");
    } else if (err == 0 && result != NULL && !result->nxdomain && !result->bogus && result->havedata &&
            ldns_wire2pkt(&pkt, result->answer_packet, result->answer_len) != LDNS_STATUS_OK) {
        log_write(query->s2s->log, LOG_ERR, "ldns_wire2pkt failed to parse DNS answer");
    } else if (err == 0 && result != NULL && !result->nxdomain && !result->bogus && result->havedata) {
        unsigned i;
        ldns_rr_list *rrs = ldns_pkt_answer(pkt);
        log_debug(ZONE, "dns response for %s@%p: SRV", query->name, query);

        for (i = 0; i < ldns_rr_list_rr_count(rrs); i++) {
            ldns_rr *rr = ldns_rr_list_rr(rrs, i);
            if (ldns_rr_get_class(rr) != LDNS_RR_CLASS_IN) continue;
            if (ldns_rr_get_type(rr) != LDNS_RR_TYPE_SRV) continue;
            if (ldns_rr_rd_count(rr) != 4) {
                log_write(query->s2s->log, LOG_ERR, "dns response for %s: SRV with %zu fields (should be 4) - ignoring broken DNS server", query->name, ldns_rr_rd_count(rr));
                continue;
            }
            int ttl = ldns_rr_ttl(rr);
            if (query->cur_expiry > 0 && ttl > query->cur_expiry) ttl = query->cur_expiry;
            uint16_t priority = ldns_rdf2native_int16(ldns_rr_rdf(rr, 0));
            uint16_t weight = ldns_rdf2native_int16(ldns_rr_rdf(rr, 1));
            uint16_t port = ldns_rdf2native_int16(ldns_rr_rdf(rr, 2));
            ldns_buffer_rewind(buf);
            if (ldns_rdf2buffer_str_dname(buf, ldns_rr_rdf(rr, 3)) != LDNS_STATUS_OK) {
                log_write(query->s2s->log, LOG_ERR, "dns response for %s: SRV name invalid - ignoring DNS server", query->name);
                continue;
            } else if (ldns_buffer_position(buf) < 2) {
                log_write(query->s2s->log, LOG_ERR, "dns response for %s: SRV empty name - ignoring DNS server", query->name);
                continue;
            }
            if (!ldns_buffer_reserve(buf, 1)) {
                log_write(query->s2s->log, LOG_ERR, "dns response for %s: SRV name exceeded buffer capacity - ignoring", query->name);
                continue;
            }
            char nullterminator = 0;
            ldns_buffer_write(buf, &nullterminator, sizeof(nullterminator));

            log_debug(ZONE, "dns response for %s@%p: SRV %s[%d] %s/%d (%d/%d)", query->name, query,
                query->name, i, ldns_buffer_at(buf, 0), port, priority, weight);
            _dns_add_host(query, ldns_buffer_at(buf, 0), port, priority, weight, ttl);
        }
    } else if (err != 0) {
        log_write(query->s2s->log, LOG_NOTICE, "dns failure for %s@%p: SRV %s (%s)\n", query->name, query,
            query->s2s->lookup_srv[query->srv_i], ub_strerror(err));
    } else if (result != NULL) {
        const char * msg = "attempted dnssec with bogus key, response discarded";
        if (!result->bogus) {
            if (result->nxdomain) msg = "NXDOMAIN";
            else if (!result->havedata) msg = "empty response (no SRV records)";
        }
        log_write(query->s2s->log, LOG_NOTICE, "dns %s for %s@%p: SRV %s\n", msg, query->name, query,
            query->s2s->lookup_srv[query->srv_i]);
    }
    if (pkt) ldns_pkt_free(pkt);
    if (buf) ldns_buffer_free(buf);
    if (result) ub_resolve_free(result);

    /* check next SRV service name */
    query->srv_i++;
    if (query->srv_i < query->s2s->lookup_nsrv) {
        int err;
        log_debug(ZONE, "dns request for %s@%p: SRV %s", query->name, query, query->s2s->lookup_srv[query->srv_i]);

        err = ub_resolve_async(query->s2s->ub_ctx, query->name, LDNS_RR_TYPE_SRV /*rrtype*/, LDNS_RR_CLASS_IN /*rrclass*/,
            query, _dns_result_srv, &query->async_id);
        query->have_async_id = 1;

        /* if submit failed, call ourselves with the error */
        if (err) _dns_result_srv(query, err, NULL);
    } else {
        /* no more SRV records to check, resolve hosts */
        if (xhash_count(query->hosts) > 0) {
            query->cur_host = 0;
            _dns_start_a(query);

        } else {
            /* no SRV records returned, resolve hostname */
            query->cur_host = strdup(query->name);
            query->cur_port = 5269;
            query->cur_prio = 0;
            query->cur_weight = 0;
            query->cur_expiry = 0;
            if (query->s2s->resolve_aaaa) _dns_start_aaaa(query); else _dns_start_a(query);
        }
    }
}

static void _dns_result_aaaa(void *data, int err, struct ub_result *result) {
    dnsquery_t query = data;
    char ip[INET6_ADDRSTRLEN];
    assert(query != NULL);
    query->have_async_id = 0;
    const char * name = query->cur_host ? query->cur_host : query->name;

    if (err == 0 && result != NULL && !result->nxdomain && !result->bogus && result->havedata && result->data != NULL && *result->data != NULL) {
        int i;
        for (i = 0; result->data[i]; i++) {
            if (inet_ntop(AF_INET6, result->data[i], ip, INET6_ADDRSTRLEN) != NULL) {
                log_debug(ZONE, "dns response for %s@%p: AAAA %s[%d] %s/%d", query->name, query, query->name, i, ip, query->cur_port);

                int ttl = result->ttl; /* getting ttl from libunbound requires a patch to libunbound to pass the ttl */
                if (query->cur_expiry > 0 && ttl > query->cur_expiry) ttl = query->cur_expiry;
                _dns_add_result(query, ip, query->cur_port, query->cur_prio, query->cur_weight, ttl);
            }
        }

        ub_resolve_free(result);
    } else if (err != 0) {
        log_write(query->s2s->log, LOG_NOTICE, "dns failure for %s@%p: AAAA %s (%s)\n", query->name, query, name, ub_strerror(err));
        if (result) ub_resolve_free(result);
    } else if (result != NULL) {
        const char * msg = "attempted dnssec with bogus key, response discarded";
        if (!result->bogus) {
            if (result->nxdomain) msg = "NXDOMAIN";
            else if (!result->havedata) msg = "empty response (broken DNS server)";
        }
        log_write(query->s2s->log, LOG_NOTICE, "dns %s for %s@%p: AAAA %s\n", msg, query->name, query, name);
        ub_resolve_free(result);
    }

    if (query->cur_host == NULL)
        /* uh-oh */
        log_write(query->s2s->log, LOG_ERR, "dns result for %s@%p: AAAA host vanished...\n", query->name, query);
    _dns_start_a(query);
}

/* this function is called with 0 err and a NULL result to start the A/AAAA process */
static void _dns_result_a(void *data, int err, struct ub_result *result) {
    dnsquery_t query = data;
    assert(query != NULL);
    query->have_async_id = 0;
    const char * name = query->cur_host ? query->cur_host : query->name;

    if (err == 0 && result != NULL && !result->nxdomain && !result->bogus && result->havedata && result->data != NULL && *result->data != NULL) {
        char ip[INET_ADDRSTRLEN];
        int i;
        for (i = 0; result->data[i]; i++) {
            if (inet_ntop(AF_INET, result->data[i], ip, INET_ADDRSTRLEN) != NULL) {
                log_debug(ZONE, "dns response for %s@%p: A %s[%d] %s/%d", query->name, query, query->name, i, ip, query->cur_port);

                int ttl = result->ttl; /* getting ttl from libunbound requires a patch to libunbound to pass the ttl */
                if (query->cur_expiry > 0 && ttl > query->cur_expiry) ttl = query->cur_expiry;
                _dns_add_result(query, ip, query->cur_port, query->cur_prio, query->cur_weight, ttl);
            }
        }

        ub_resolve_free(result);
    } else if (err != 0) {
        log_write(query->s2s->log, LOG_NOTICE, "dns failure for %s@%p: A %s (%s)\n", query->name, query, name, ub_strerror(err));
        if (result) ub_resolve_free(result);
    } else if (result != NULL) {
        const char * msg = "attempted dnssec with bogus key, response discarded";
        if (!result->bogus) {
            if (result->nxdomain) msg = "NXDOMAIN";
            else if (!result->havedata) msg = "empty response (broken DNS server)";
        }
        log_write(query->s2s->log, LOG_NOTICE, "dns %s for %s@%p: A %s\n", msg, query->name, query, name);
        ub_resolve_free(result);
    }

    /* resolve the next host in the list */
    if (xhash_iter_first(query->hosts)) {
        char *ipport, *c;
        int ipport_len;
        dnsres_t res;
        union xhashv xhv;

        xhv.dnsres_val = &res;

        /* get the first entry */
        xhash_iter_get(query->hosts, (const char **) &ipport, &ipport_len, xhv.val);

        /* remove the host from the list */
        xhash_iter_zap(query->hosts);

        c = memchr(ipport, '/', ipport_len);
        assert(c);

        /* resolve hostname */
        free(query->cur_host);
        query->cur_host = malloc(c - ipport + 1);
        memcpy(query->cur_host, ipport, c - ipport);
        query->cur_host[c - ipport] = 0;
        char * port_str = (char *) malloc(ipport_len - (c - ipport));
        c++;
        memcpy(port_str, c, ipport_len - (c - ipport));
        port_str[c - ipport] = 0;
        query->cur_port = atoi(port_str);
        free(port_str);
        query->cur_prio = res->prio;
        query->cur_weight = res->weight;
        query->cur_expiry = res->expiry;
        log_debug(ZONE, "dns ttl for %s@%p limited to %lld", query->name, query, (long long) query->cur_expiry);

        if (query->s2s->resolve_aaaa) _dns_start_aaaa(query); else _dns_start_a(query);

    /* finished */
    } else {
        time_t now = time(NULL);
        char *domain;

        free(query->cur_host);
        query->cur_host = NULL;

        log_debug(ZONE, "dns requests for %s@%p complete: %d (ttl %lld)", query->name,
            query, xhash_count(query->results), (long long) query->expiry);

        /* update query TTL */
        if (query->expiry > query->s2s->dns_max_ttl)
            query->expiry = query->s2s->dns_max_ttl;

        if (query->expiry < query->s2s->dns_min_ttl)
            query->expiry = query->s2s->dns_min_ttl;

        query->expiry += now;

        /* update result TTLs - the query expiry MUST NOT be longer than all result expiries */
        if (xhash_iter_first(query->results)) {
            union xhashv xhv;
            dnsres_t res;

            xhv.dnsres_val = &res;

            do {
                xhash_iter_get(query->results, NULL, NULL, xhv.val);

                if (res->expiry > query->s2s->dns_max_ttl)
                    res->expiry = query->s2s->dns_max_ttl;

                if (res->expiry < query->s2s->dns_min_ttl)
                    res->expiry = query->s2s->dns_min_ttl;

                res->expiry += now;
            } while(xhash_iter_next(query->results));
        }

        xhash_free(query->hosts);
        query->hosts = NULL;
        if (idna_to_unicode_8z8z(query->name, &domain, 0) != IDNA_SUCCESS)
        {
            log_write(query->s2s->log, LOG_ERR, "idna dns decode for %s failed", query->name);
            /* TODO: Is it better to shortcut resolution failure here? */
            domain = strdup(query->name);
        }
        out_resolve(query->s2s, domain, query->results, query->expiry);
        free(domain);
        free(query->name);
        free(query);
    }
}

void dns_resolve_domain(s2s_t s2s, dnscache_t dns) {
    dnsquery_t query = (dnsquery_t) calloc(1, sizeof(struct dnsquery_st));

    query->s2s = s2s;
    if (idna_to_ascii_8z(dns->name, &query->name, 0) != IDNA_SUCCESS)
    {
        log_write(s2s->log, LOG_ERR, "idna dns encode for %s failed", dns->name);
        /* TODO: Is it better to shortcut resolution failure here? */
        query->name = strdup(dns->name);
    }
    query->srv_i = -1;
    query->hosts = xhash_new(71);
    query->results = xhash_new(71);
    query->expiry = 0;
    query->cur_host = NULL;
    query->cur_port = 0;
    query->cur_expiry = 0;
    query->have_async_id = 0;
    dns->query = query;

    log_debug(ZONE, "dns resolve for %s@%p started", query->name, query);

    /* - resolve all SRV records to host/port
     * - if no results, include domain/5269
     * - resolve all host/port combinations
     * - query is stored in dns->query and runs asynchronously, calls out_resolve() with results
     */
    _dns_result_srv(query, 0, NULL);
}

/** responses from the resolver */
void out_resolve(s2s_t s2s, char *domain, xht results, time_t expiry) {
    dnscache_t dns;

    /* no results, resolve failed */
    if(xhash_count(results) == 0) {
        dns = xhash_get(s2s->dnscache, domain);
        if (dns != NULL) {
            /* store negative DNS cache */
            xhash_free(dns->results);
            dns->query = NULL;
            dns->results = NULL;
            dns->expiry = expiry;
            dns->pending = 0;
        }

        log_write(s2s->log, LOG_NOTICE, "dns lookup for %s failed", domain);

        /* bounce queue */
        out_bounce_domain_queues(s2s, domain, stanza_err_REMOTE_SERVER_NOT_FOUND);

        xhash_free(results);
        return;
    }

    log_write(s2s->log, LOG_NOTICE, "dns lookup for %s returned %d result%s (ttl %d)",
        domain, xhash_count(results), xhash_count(results)!=1?"s":"", expiry - time(NULL));

    /* get the cache entry */
    dns = xhash_get(s2s->dnscache, domain);
    if(dns == NULL) {
        log_debug(ZONE, "weird, we never requested this");

        xhash_free(results);
        return;
    }
    
    /* fill it out */
    xhash_free(dns->results);
    dns->query = NULL;
    dns->results = results;
    dns->expiry = expiry;
    dns->pending = 0;

    out_flush_domain_queues(s2s, domain);

    /* delete the cache entry if caching is disabled */
    if (!s2s->dns_cache_enabled && !dns->pending) {
        xhash_free(dns->results);
        xhash_zap(s2s->dnscache, domain);
        free(dns);
    }
}

/** mio callback for outgoing conns */
static int _out_mio_callback(mio_t m, mio_action_t a, mio_fd_t fd, void *data, void *arg) {
    conn_t out = (conn_t) arg;
    char ipport[INET6_ADDRSTRLEN + 17];
    int nbytes;

    switch(a) {
        case action_READ:
            log_debug(ZONE, "read action on fd %d", fd->fd);

            /* they did something */
            out->last_activity = time(NULL);

            ioctl(fd->fd, FIONREAD, &nbytes);
            if(nbytes == 0) {
                sx_kill(out->s);
                return 0;
            }

            return sx_can_read(out->s);

        case action_WRITE:
            log_debug(ZONE, "write action on fd %d", fd->fd);

            /* update activity timestamp */
            out->last_activity = time(NULL);

            return sx_can_write(out->s);

        case action_CLOSE:
            log_debug(ZONE, "close action on fd %d", fd->fd);

            jqueue_push(out->s2s->dead, (void *) out->s, 0);

            log_write(out->s2s->log, LOG_NOTICE, "[%d] [%s, port=%d] disconnect, packets: %i", fd->fd, out->ip, out->port, out->packet_count);


            if (out->s2s->out_reuse) {
                /* generate the ip/port pair */
                snprintf(ipport, INET6_ADDRSTRLEN + 16, "%s/%d", out->ip, out->port);

                xhash_zap(out->s2s->out_host, ipport);
            }

            if (xhash_iter_first(out->routes)) {
                char *rkey;
                int rkey_len;
                char *c;

                /* remove all the out_dest entries */
                do {
                    xhash_iter_get(out->routes, (const char **) &rkey, &rkey_len, NULL);
                    c = memchr(rkey, '/', rkey_len);
                    c++;
                    int c_len = rkey_len - (c - rkey);

                    log_debug(ZONE, "route '%.*s'", rkey_len, rkey);
                    if (xhash_getx(out->s2s->out_dest, c, c_len) != NULL) {
                        log_debug(ZONE, "removing dest entry for '%.*s'", c_len, c);
                        xhash_zapx(out->s2s->out_dest, c, c_len);
                    }
                } while(xhash_iter_next(out->routes));
            }

            if (xhash_iter_first(out->routes)) {
                char *rkey;
                int rkey_len;
                jqueue_t q;
                int npkt;

                /* retry all the routes */
                do {
                    xhash_iter_get(out->routes, (const char **) &rkey, &rkey_len, NULL);

                    q = xhash_getx(out->s2s->outq, rkey, rkey_len);
                    if (out->s2s->retry_limit > 0 && q != NULL && jqueue_age(q) > out->s2s->retry_limit) {
                        log_debug(ZONE, "retry limit reached for '%.*s' queue", rkey_len, rkey);
                        q = NULL;
                    }

                    char * local_rkey = (char *) malloc(rkey_len + 1);
                    memcpy(local_rkey, rkey, rkey_len);
                    local_rkey[rkey_len] = 0;
                    if (q != NULL && (npkt = jqueue_size(q)) > 0) {
                        conn_t retry;

                        log_debug(ZONE, "retrying connection for '%s' queue", local_rkey);
                        if (!out_route(out->s2s, local_rkey, &retry, 0)) {
                            log_debug(ZONE, "retry successful");

                            if (retry != NULL) {
                                /* flush queue */
                                out_flush_route_queue(out->s2s, local_rkey);
                            }
                        } else {
                            log_debug(ZONE, "retry failed");

                            /* bounce queue */
                            out_bounce_route_queue(out->s2s, local_rkey, stanza_err_SERVICE_UNAVAILABLE);
                        }
                    } else {
                        /* bounce queue */
                        out_bounce_route_queue(out->s2s, rkey, stanza_err_SERVICE_UNAVAILABLE);
                    }
                    free(local_rkey);
                } while(xhash_iter_next(out->routes));
            }

            jqueue_push(out->s2s->dead_conn, (void *) out, 0);

        case action_ACCEPT:
            break;
    }

    return 0;
}

void send_dialbacks(conn_t out)
{
    if (out->s2s->dns_bad_timeout > 0) {
        dnsres_t bad = xhash_get(out->s2s->dns_bad, out->key);

        if (bad != NULL) {
            log_debug(ZONE, "removing bad host entry for '%s'", out->key);
            xhash_zap(out->s2s->dns_bad, out->key);
            free(bad->key);
            free(bad);
        }
    }

    if (xhash_iter_first(out->routes)) {
        log_debug(ZONE, "sending dialback packets for %s", out->key);
        do {
            char *rkey;
            int rkey_len;
            xhash_iter_get(out->routes, (const char **) &rkey, &rkey_len, NULL);
            char * local_rkey = (char *) malloc(rkey_len + 1);
            memcpy(local_rkey, rkey, rkey_len);
            local_rkey[rkey_len] = 0;
            _out_dialback(out, local_rkey);
            free(local_rkey);
        } while(xhash_iter_next(out->routes));
    }

    return;
}

static int _out_sx_callback(sx_t s, sx_event_t e, void *data, void *arg) {
    conn_t out = (conn_t) arg;
    sx_buf_t buf = (sx_buf_t) data;
    int len, ns, elem, starttls = 0;
    sx_error_t *sxe;
    nad_t nad;

    switch(e) {
        case event_WANT_READ:
            log_debug(ZONE, "want read");
            mio_read(out->s2s->mio, out->fd);
            break;

        case event_WANT_WRITE:
            log_debug(ZONE, "want write");
            mio_write(out->s2s->mio, out->fd);
            break;

        case event_READ:
            log_debug(ZONE, "reading from %d", out->fd->fd);

            /* do the read */
            len = recv(out->fd->fd, buf->data, buf->len, 0);

            if(len < 0) {
                if(MIO_WOULDBLOCK) {
                    buf->len = 0;
                    return 0;
                }

                log_write(out->s2s->log, LOG_NOTICE, "[%d] [%s, port=%d] read error: %s (%d)", out->fd->fd, out->ip, out->port, MIO_STRERROR(MIO_ERROR), MIO_ERROR);

                if (!out->online) {
                    dnsres_t bad;
                    char *ipport;

                    if (out->s2s->dns_bad_timeout > 0) {
                        /* mark this host as bad */
                        ipport = dns_make_ipport(out->ip, out->port);
                        bad = xhash_get(out->s2s->dns_bad, ipport);
                        if (bad == NULL) {
                            bad = (dnsres_t) calloc(1, sizeof(struct dnsres_st));
                            bad->key = ipport;
                            xhash_put(out->s2s->dns_bad, ipport, bad);
                        } else {
                            free(ipport);
                        }
                        bad->expiry = time(NULL) + out->s2s->dns_bad_timeout;
                    }
                }

                sx_kill(s);
                
                return -1;
            }

            else if(len == 0) {
                /* they went away */
                sx_kill(s);

                return -1;
            }

            log_debug(ZONE, "read %d bytes", len);

            buf->len = len;

            return len;

        case event_WRITE:
            log_debug(ZONE, "writing to %d", out->fd->fd);

            len = send(out->fd->fd, buf->data, buf->len, 0);
            if(len >= 0) {
                log_debug(ZONE, "%d bytes written", len);
                return len;
            }

            if(MIO_WOULDBLOCK)
                return 0;

            log_write(out->s2s->log, LOG_NOTICE, "[%d] [%s, port=%d] write error: %s (%d)", out->fd->fd, out->ip, out->port, MIO_STRERROR(MIO_ERROR), MIO_ERROR);

            if (!out->online) {
                dnsres_t bad;
                char *ipport;

                if (out->s2s->dns_bad_timeout > 0) {
                    /* mark this host as bad */
                    ipport = dns_make_ipport(out->ip, out->port);
                    bad = xhash_get(out->s2s->dns_bad, ipport);
                    if (bad == NULL) {
                        bad = (dnsres_t) calloc(1, sizeof(struct dnsres_st));
                        bad->key = ipport;
                        xhash_put(out->s2s->dns_bad, ipport, bad);
                    } else {
                        free(ipport);
                    }
                    bad->expiry = time(NULL) + out->s2s->dns_bad_timeout;
                }
            }

            sx_kill(s);

            return -1;

        case event_ERROR:
            sxe = (sx_error_t *) data;
            log_write(out->s2s->log, LOG_NOTICE, "[%d] [%s, port=%d] error: %s (%s)", out->fd->fd, out->ip, out->port, sxe->generic, sxe->specific);

            /* mark as bad if we did not manage to connect or there is unrecoverable stream error */
            if (!out->online ||
                    (sxe->code == SX_ERR_STREAM &&
                        (strstr(sxe->specific, "host-gone") ||        /* it's not there now */
                         strstr(sxe->specific, "host-unknown") ||     /* they do not service the host */
                         strstr(sxe->specific, "not-authorized") ||   /* they do not want us there */
                         strstr(sxe->specific, "see-other-host") ||   /* we do not support redirections yet */
                         strstr(sxe->specific, "system-shutdown") ||  /* they are going down */
                         strstr(sxe->specific, "policy-violation") || /* they do not want us there */
                         strstr(sxe->specific, "remote-connection-failed") ||  /* the required remote entity is gone */
                         strstr(sxe->specific, "unsupported-encoding") ||      /* they do not like our encoding */
                         strstr(sxe->specific, "undefined-condition") ||       /* something bad happend */
                         strstr(sxe->specific, "internal-server-error") ||     /* that server is broken */
                         strstr(sxe->specific, "unsupported-version")          /* they do not support our stream version */
                        )))
            {
                dnsres_t bad;
                char *ipport;

                if (out->s2s->dns_bad_timeout > 0) {
                    /* mark this host as bad */
                    ipport = dns_make_ipport(out->ip, out->port);
                    bad = xhash_get(out->s2s->dns_bad, ipport);
                    if (bad == NULL) {
                        bad = (dnsres_t) calloc(1, sizeof(struct dnsres_st));
                        bad->key = ipport;
                        xhash_put(out->s2s->dns_bad, ipport, bad);
                    } else {
                        free(ipport);
                    }
                    bad->expiry = time(NULL) + out->s2s->dns_bad_timeout;
                }
            }

            sx_kill(s);

            return -1;

        case event_OPEN:
            log_debug(ZONE, "OPEN event for %s", out->key);
            break;

        case event_STREAM:
            /* check stream version - NULl = pre-xmpp (some jabber1 servers) */
            log_debug(ZONE, "STREAM event for %s stream version is %s", out->key, out->s->res_version);

            /* first time, bring them online */
            if(!out->online) {
                log_debug(ZONE, "outgoing conn to %s is online", out->key);

                /* if no stream version from either side, kick off dialback for each route, */
                /* otherwise wait for stream features */
                if ((out->s->res_version==NULL) || (out->s2s->sx_ssl == NULL)) {
                     log_debug(ZONE, "no stream version, sending dialbacks for %s immediately", out->key);
                     out->online = 1;
                     send_dialbacks(out);
                } else
                     log_debug(ZONE, "outgoing conn to %s - waiting for STREAM features", out->key);
            }

            break;

        case event_PACKET:
            /* we're counting packets */
            out->packet_count++;
            out->s2s->packet_count++;

            nad = (nad_t) data;

            /* watch for the features packet - STARTTLS and/or SASL*/
            if ((out->s->res_version!=NULL) 
                 && NAD_NURI_L(nad, NAD_ENS(nad, 0)) == strlen(uri_STREAMS)   
                 && strncmp(uri_STREAMS, NAD_NURI(nad, NAD_ENS(nad, 0)), strlen(uri_STREAMS)) == 0
                 && NAD_ENAME_L(nad, 0) == 8 && strncmp("features", NAD_ENAME(nad, 0), 8) == 0) {
                log_debug(ZONE, "got the stream features packet");

#ifdef HAVE_SSL
                /* starttls if we can */
                if(out->s2s->sx_ssl != NULL && s->ssf == 0) {
                    ns = nad_find_scoped_namespace(nad, uri_TLS, NULL);
                    if(ns >= 0) {
                        elem = nad_find_elem(nad, 0, ns, "starttls", 1);
                        if(elem >= 0) {
                            log_debug(ZONE, "got STARTTLS in stream features");
                            if(sx_ssl_client_starttls(out->s2s->sx_ssl, s, out->s2s->local_pemfile) == 0) {
                                starttls = 1;
                                nad_free(nad);
                                return 0;
                            }
                            log_write(out->s2s->log, LOG_ERR, "unable to establish encrypted session with peer");
                        }
                    }
                }

                /* If we're not establishing a starttls connection, send dialbacks */
                if (!starttls) {
                     log_debug(ZONE, "No STARTTLS, sending dialbacks for %s", out->key);
                     out->online = 1;
                     send_dialbacks(out);
                }
#else
                out->online = 1;
                send_dialbacks(out);
#endif
            }


            /* we only accept dialback packets */
            if(NAD_ENS(nad, 0) < 0 || NAD_NURI_L(nad, NAD_ENS(nad, 0)) != uri_DIALBACK_L || strncmp(uri_DIALBACK, NAD_NURI(nad, NAD_ENS(nad, 0)), uri_DIALBACK_L) != 0) {
                log_debug(ZONE, "got a non-dialback packet on an outgoing conn, dropping it");
                nad_free(nad);
                return 0;
            }

            /* and then only result and verify */
            if(NAD_ENAME_L(nad, 0) == 6) {
                if(strncmp("result", NAD_ENAME(nad, 0), 6) == 0) {
                    _out_result(out, nad);
                    return 0;
                }

                if(strncmp("verify", NAD_ENAME(nad, 0), 6) == 0) {
                    _out_verify(out, nad);
                    return 0;
                }
            }
                
            log_debug(ZONE, "unknown dialback packet, dropping it");

            nad_free(nad);
            return 0;

        case event_CLOSED:
            mio_close(out->s2s->mio, out->fd);
            return -1;
    }

    return 0;
}

/** process incoming auth responses */
static void _out_result(conn_t out, nad_t nad) {
    int attr;
    jid_t from, to;
    char *rkey;

    attr = nad_find_attr(nad, 0, -1, "from", NULL);
    if(attr < 0 || (from = jid_new(NAD_AVAL(nad, attr), NAD_AVAL_L(nad, attr))) == NULL) {
        log_debug(ZONE, "missing or invalid from on db result packet");
        nad_free(nad);
        return;
    }

    attr = nad_find_attr(nad, 0, -1, "to", NULL);
    if(attr < 0 || (to = jid_new(NAD_AVAL(nad, attr), NAD_AVAL_L(nad, attr))) == NULL) {
        log_debug(ZONE, "missing or invalid to on db result packet");
        jid_free(from);
        nad_free(nad);
        return;
    }

    rkey = s2s_route_key(NULL, to->domain, from->domain);

    /* key is valid */
    if(nad_find_attr(nad, 0, -1, "type", "valid") >= 0) {
        log_write(out->s2s->log, LOG_NOTICE, "[%d] [%s, port=%d] outgoing route '%s' is now valid%s", out->fd->fd, out->ip, out->port, rkey, (out->s->flags & SX_SSL_WRAPPER) ? ", TLS negotiated" : "");

        xhash_put(out->states, pstrdup(xhash_pool(out->states), rkey), (void *) conn_VALID);    /* !!! small leak here */

        log_debug(ZONE, "%s valid, flushing queue", rkey);

        /* flush the queue */
        out_flush_route_queue(out->s2s, rkey);

        free(rkey);

        jid_free(from);
        jid_free(to);

        nad_free(nad);

        return;
    }

    /* invalid */
    log_write(out->s2s->log, LOG_NOTICE, "[%d] [%s, port=%d] outgoing route '%s' is now invalid", out->fd->fd, out->ip, out->port, rkey);

    /* close connection */
    log_write(out->s2s->log, LOG_NOTICE, "[%d] [%s, port=%d] closing connection", out->fd->fd, out->ip, out->port);

    /* report stream error */
    sx_error(out->s, stream_err_INVALID_ID, "dialback negotiation failed");

    /* close the stream */
    sx_close(out->s);

    /* bounce queue */
    out_bounce_route_queue(out->s2s, rkey, stanza_err_SERVICE_UNAVAILABLE);

    free(rkey);

    jid_free(from);
    jid_free(to);

    nad_free(nad);
}

/** incoming stream authenticated */
static void _out_verify(conn_t out, nad_t nad) {
    int attr, ns;
    jid_t from, to;
    conn_t in;
    char *rkey;
    int valid;
    
    attr = nad_find_attr(nad, 0, -1, "from", NULL);
    if(attr < 0 || (from = jid_new(NAD_AVAL(nad, attr), NAD_AVAL_L(nad, attr))) == NULL) {
        log_debug(ZONE, "missing or invalid from on db verify packet");
        nad_free(nad);
        return;
    }

    attr = nad_find_attr(nad, 0, -1, "to", NULL);
    if(attr < 0 || (to = jid_new(NAD_AVAL(nad, attr), NAD_AVAL_L(nad, attr))) == NULL) {
        log_debug(ZONE, "missing or invalid to on db verify packet");
        jid_free(from);
        nad_free(nad);
        return;
    }

    attr = nad_find_attr(nad, 0, -1, "id", NULL);
    if(attr < 0) {
        log_debug(ZONE, "missing id on db verify packet");
        jid_free(from);
        jid_free(to);
        nad_free(nad);
        return;
    }

    /* get the incoming conn */
    in = xhash_getx(out->s2s->in, NAD_AVAL(nad, attr), NAD_AVAL_L(nad, attr));
    if(in == NULL) {
        log_debug(ZONE, "got a verify for incoming conn %.*s, but it doesn't exist, dropping the packet", NAD_AVAL_L(nad, attr), NAD_AVAL(nad, attr));
        jid_free(from);
        jid_free(to);
        nad_free(nad);
        return;
    }

    rkey = s2s_route_key(NULL, to->domain, from->domain);

    attr = nad_find_attr(nad, 0, -1, "type", "valid");
    if(attr >= 0) {
        xhash_put(in->states, pstrdup(xhash_pool(in->states), rkey), (void *) conn_VALID);
        log_write(in->s2s->log, LOG_NOTICE, "[%d] [%s, port=%d] incoming route '%s' is now valid%s", in->fd->fd, in->ip, in->port, rkey, (in->s->flags & SX_SSL_WRAPPER) ? ", TLS negotiated" : "");
        valid = 1;
    } else {
        log_write(in->s2s->log, LOG_NOTICE, "[%d] [%s, port=%d] incoming route '%s' is now invalid", in->fd->fd, in->ip, in->port, rkey);
        valid = 0;
    }

    free(rkey);

    nad_free(nad);

    /* decrement outstanding verify counter */
    --out->verify;

    /* let them know what happened */
    nad = nad_new();

    ns = nad_add_namespace(nad, uri_DIALBACK, "db");
    nad_append_elem(nad, ns, "result", 0);
    nad_append_attr(nad, -1, "to", from->domain);
    nad_append_attr(nad, -1, "from", to->domain);
    nad_append_attr(nad, -1, "type", valid ? "valid" : "invalid");

    /* off it goes */
    sx_nad_write(in->s, nad);

    /* if invalid, close the stream */
    if (!valid) {
        /* generate stream error */
        sx_error(in->s, stream_err_INVALID_ID, "dialback negotiation failed");

        /* close the incoming stream */
        sx_close(in->s);
    }

    jid_free(from);
    jid_free(to);
}

/* bounce all packets in the queues for domain */
int out_bounce_domain_queues(s2s_t s2s, const char *domain, int err)
{
    int pktcount = 0;

    if (xhash_iter_first(s2s->outq)) {
        do {
            char *rkey;
            int rkey_len;
            xhash_iter_get(s2s->outq, (const char **) &rkey, &rkey_len, NULL);
            char * local_rkey = (char *) malloc(rkey_len + 1);
            memcpy(local_rkey, rkey, rkey_len);
            local_rkey[rkey_len] = 0;
            if(s2s_route_key_match(NULL, domain, local_rkey))
                pktcount += out_bounce_route_queue(s2s, local_rkey, err);
            free(local_rkey);
        } while(xhash_iter_next(s2s->outq));
    }

    return pktcount;
}

/* bounce all packets in the queue for route */
int out_bounce_route_queue(s2s_t s2s, char *rkey, int err)
{
  jqueue_t q;
  pkt_t pkt;
  int pktcount = 0;

  q = xhash_get(s2s->outq, rkey);
  if(q == NULL)
     return 0;

  while((pkt = jqueue_pull(q)) != NULL) {
     /* only packets with content, in namespace jabber:client and not already errors */
     if(pkt->nad->ecur > 1 && NAD_NURI_L(pkt->nad, NAD_ENS(pkt->nad, 1)) == strlen(uri_CLIENT) && strncmp(NAD_NURI(pkt->nad, NAD_ENS(pkt->nad, 1)), uri_CLIENT, strlen(uri_CLIENT)) == 0 && nad_find_attr(pkt->nad, 0, -1, "error", NULL) < 0) {
         sx_nad_write(s2s->router, stanza_tofrom(stanza_tofrom(stanza_error(pkt->nad, 1, err), 1), 0));
         pktcount++;
     }
     else
         nad_free(pkt->nad);

     jid_free(pkt->to);
     jid_free(pkt->from);
     free(pkt);
  }

  /* delete queue and remove domain from queue hash */
  log_debug(ZONE, "deleting out packet queue for %s", rkey);
  rkey = q->key;
  jqueue_free(q);
  xhash_zap(s2s->outq, rkey);
  free(rkey);

  return pktcount;
}

int out_bounce_conn_queues(conn_t out, int err)
{
    int pktcount = 0;

    /* bounce queues for all domains handled by this connection - iterate through routes */
    if (xhash_iter_first(out->routes)) {
        do {
            char *rkey;
            int rkey_len;
            xhash_iter_get(out->routes, (const char **) &rkey, &rkey_len, NULL);
            char * local_rkey = (char *) malloc(rkey_len + 1);
            memcpy(local_rkey, rkey, rkey_len);
            local_rkey[rkey_len] = 0;
            pktcount += out_bounce_route_queue(out->s2s, rkey, err);
            free(local_rkey);
        } while(xhash_iter_next(out->routes));
    }

    return pktcount;
}

void out_flush_domain_queues(s2s_t s2s, const char *domain) {
    if (xhash_iter_first(s2s->outq)) {
        do {
            char *rkey;
            int rkey_len;
            char *c;

            xhash_iter_get(s2s->outq, (const char **) &rkey, &rkey_len, NULL);
            c = memchr(rkey, '/', rkey_len);
            assert(c);
            c++;
            if (!strncmp(domain, c, rkey_len - (c - rkey))) {
                char * local_rkey = (char *) malloc(rkey_len + 1);
                memcpy(local_rkey, rkey, rkey_len);
                local_rkey[rkey_len] = 0;
                out_flush_route_queue(s2s, local_rkey);
                free(local_rkey);
            }
        } while(xhash_iter_next(s2s->outq));
    }
}

void out_flush_route_queue(s2s_t s2s, char *rkey) {
    jqueue_t q;
    pkt_t pkt;
    int npkt, i, ret;

    q = xhash_get(s2s->outq, rkey);
    if(q == NULL)
        return;

    npkt = jqueue_size(q);
    log_debug(ZONE, "flushing %d packets for '%s' to out_packet", npkt, rkey);

    for(i = 0; i < npkt; i++) {
        pkt = jqueue_pull(q);
        if(pkt) {
            ret = out_packet(s2s, pkt);
            if (ret) {
                /* uh-oh. the queue was deleted...
                   q and pkt have been freed
                   if q->key == rkey, rkey has also been freed */
                return;
            }
        }
    }

    /* delete queue for route and remove route from queue hash */
    if (jqueue_size(q) == 0) {
        log_debug(ZONE, "deleting out packet queue for '%s'", rkey);
        rkey = q->key;
        jqueue_free(q);
        xhash_zap(s2s->outq, rkey);
        free(rkey);
    } else {
        log_debug(ZONE, "emptied queue gained more packets...");
    }
}
