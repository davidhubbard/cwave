/*
 * services/cache/infra.h - infrastructure cache, server rtt and capabilities
 *
 * Copyright (c) 2007, NLnet Labs. All rights reserved.
 *
 * This software is open source.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 
 * Neither the name of the NLNET LABS nor the names of its contributors may
 * be used to endorse or promote products derived from this software without
 * specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * \file
 *
 * This file contains the infrastructure cache.
 */

#ifndef SERVICES_CACHE_INFRA_H
#define SERVICES_CACHE_INFRA_H
#include "util/storage/lruhash.h"
#include "util/rtt.h"
struct slabhash;
struct config_file;

/**
 * Host information kept for every server.
 */
struct infra_host_key {
	/** the host address. */
	struct sockaddr_storage addr;
	/** length of addr. */
	socklen_t addrlen;
	/** hash table entry, data of type infra_host_data. */
	struct lruhash_entry entry;
};

/**
 * Host information encompasses host capabilities and retransmission timeouts.
 */
struct infra_host_data {
	/** TTL value for this entry. absolute time. */
	uint32_t ttl;
	/** round trip times for timeout calculation */
	struct rtt_info rtt;
	/** Names of the zones that are lame. NULL=no lame zones. */
	struct lruhash* lameness;
	/** edns version that the host supports, -1 means no EDNS */
	int edns_version;
	/** if the EDNS lameness is already known or not.
	 * EDNS lame is when EDNS queries or replies are dropped, 
	 * and cause a timeout */
	uint8_t edns_lame_known;
	/** Number of consequtive timeouts; reset when reply arrives OK. */
	uint8_t num_timeouts;
};

/**
 * Lameness information, per host, per zone. 
 */
struct infra_lame_key {
	/** key is zone name in wireformat */
	uint8_t* zonename;
	/** length of zonename */
	size_t namelen;
	/** lruhash entry */
	struct lruhash_entry entry;
};

/**
 * Lameness information. Expires.
 * This host is lame because it is in the cache.
 */
struct infra_lame_data {
	/** TTL of this entry. absolute time. */
	uint32_t ttl;
	/** is the host lame (does not serve the zone authoritatively),
	 * or is the host dnssec lame (does not serve DNSSEC data) */
	int isdnsseclame;
	/** is the host recursion lame (not AA, but RA) */
	int rec_lame;
	/** the host is lame (not authoritative) for A records */
	int lame_type_A;
	/** the host is lame (not authoritative) for other query types */
	int lame_other;
};

/**
 * Infra cache 
 */
struct infra_cache {
	/** The hash table with hosts */
	struct slabhash* hosts;
	/** TTL value for host information, in seconds */
	int host_ttl;
	/** TTL for Lameness information, in seconds */
	int lame_ttl;
	/** infra lame cache max memory per host, in bytes */
	size_t max_lame_size;
};

/** infra host cache default hash lookup size */
#define INFRA_HOST_STARTSIZE 32
/** infra lame cache default hash lookup size */
#define INFRA_LAME_STARTSIZE 2

/**
 * Create infra cache.
 * @param cfg: config parameters or NULL for defaults.
 * @return: new infra cache, or NULL.
 */
struct infra_cache* infra_create(struct config_file* cfg);

/**
 * Delete infra cache.
 * @param infra: infrastructure cache to delete.
 */
void infra_delete(struct infra_cache* infra);

/**
 * Adjust infra cache to use updated configuration settings.
 * This may clean the cache. Operates a bit like realloc.
 * There may be no threading or use by other threads.
 * @param infra: existing cache. If NULL a new infra cache is returned.
 * @param cfg: config options.
 * @return the new infra cache pointer or NULL on error.
 */
struct infra_cache* infra_adjust(struct infra_cache* infra, 
	struct config_file* cfg);

/**
 * Lookup host data
 * @param infra: infrastructure cache.
 * @param addr: host address.
 * @param addrlen: length of addr.
 * @param wr: set to true to get a writelock on the entry.
 * @param timenow: what time it is now.
 * @param key: the key for the host, returned so caller can unlock when done.
 * @return: host data or NULL if not found or expired.
 */
struct infra_host_data* infra_lookup_host(struct infra_cache* infra, 
	struct sockaddr_storage* addr, socklen_t addrlen, int wr, 
	uint32_t timenow, struct infra_host_key** key);

/**
 * Find host information to send a packet. Creates new entry if not found.
 * Lameness is empty. EDNS is 0 (try with first), and rtt is returned for 
 * the first message to it.
 * @param infra: infrastructure cache.
 * @param addr: host address.
 * @param addrlen: length of addr.
 * @param timenow: what time it is now.
 * @param edns_vs: edns version it supports, is returned.
 * @param edns_lame_known: if EDNS lame (EDNS is dropped in transit) has
 * 	already been probed, is returned.
 * @param to: timeout to use, is returned.
 * @return: 0 on error.
 */
int infra_host(struct infra_cache* infra, struct sockaddr_storage* addr, 
	socklen_t addrlen, uint32_t timenow, int* edns_vs, 
	uint8_t* edns_lame_known, int* to);

/**
 * Check for lameness of this server for a particular zone.
 * You must have a lock on the host structure.
 * @param host: infrastructure cache data for the host. Caller holds lock.
 * @param name: domain name of zone apex.
 * @param namelen: length of domain name.
 * @param timenow: what time it is now.
 * @param dlame: if the function returns true, is set true if dnssec lame.
 * @param rlame: if the function returns true, is set true if recursion lame.
 * @param alame: if the function returns true, is set true if qtype A lame.
 * @param olame: if the function returns true, is set true if qtype other lame.
 * @return: 0 if not lame or unknown or timed out, 1 if lame
 */
int infra_lookup_lame(struct infra_host_data* host,
	uint8_t* name, size_t namelen, uint32_t timenow,
	int* dlame, int* rlame, int* alame, int* olame);

/**
 * Set a host to be lame for the given zone.
 * @param infra: infrastructure cache.
 * @param addr: host address.
 * @param addrlen: length of addr.
 * @param name: domain name of zone apex.
 * @param namelen: length of domain name.
 * @param timenow: what time it is now.
 * @param dnsseclame: if true the host is set dnssec lame.
 *	if false, the host is marked lame (not serving the zone).
 * @param reclame: if true host is a recursor not AA server.
 *      if false, dnsseclame or marked lame.
 * @param qtype: the query type for which it is lame.
 * @return: 0 on error.
 */
int infra_set_lame(struct infra_cache* infra,
        struct sockaddr_storage* addr, socklen_t addrlen,
	uint8_t* name, size_t namelen, uint32_t timenow, int dnsseclame,
	int reclame, uint16_t qtype);

/**
 * Update rtt information for the host.
 * @param infra: infrastructure cache.
 * @param addr: host address.
 * @param addrlen: length of addr.
 * @param roundtrip: estimate of roundtrip time in milliseconds or -1 for 
 * 	timeout.
 * @param orig_rtt: original rtt for the query that timed out (roundtrip==-1).
 * 	ignored if roundtrip != -1.
 * @param timenow: what time it is now.
 * @return: 0 on error. new rto otherwise.
 */
int infra_rtt_update(struct infra_cache* infra,
        struct sockaddr_storage* addr, socklen_t addrlen,
	int roundtrip, int orig_rtt, uint32_t timenow);

/**
 * Update information for the host, store that a TCP transaction works.
 * @param infra: infrastructure cache.
 * @param addr: host address.
 * @param addrlen: length of addr.
 */
void infra_update_tcp_works(struct infra_cache* infra,
        struct sockaddr_storage* addr, socklen_t addrlen);

/**
 * Update edns information for the host.
 * @param infra: infrastructure cache.
 * @param addr: host address.
 * @param addrlen: length of addr.
 * @param edns_version: the version that it publishes.
 * @param timenow: what time it is now.
 * @return: 0 on error.
 */
int infra_edns_update(struct infra_cache* infra,
        struct sockaddr_storage* addr, socklen_t addrlen,
	int edns_version, uint32_t timenow);

/**
 * Get Lameness information and average RTT if host is in the cache.
 * @param infra: infrastructure cache.
 * @param addr: host address.
 * @param addrlen: length of addr.
 * @param name: zone name.
 * @param namelen: zone name length.
 * @param qtype: the query to be made.
 * @param lame: if function returns true, this returns lameness of the zone.
 * @param dnsseclame: if function returns true, this returns if the zone
 *	is dnssec-lame.
 * @param reclame: if function returns true, this is if it is recursion lame.
 * @param rtt: if function returns true, this returns avg rtt of the server.
 * 	The rtt value is unclamped and reflects recent timeouts.
 * @param lost: number of queries lost in a row.  Reset to 0 when an answer
 * 	gets back.  Gives a connectivity number.
 * @param timenow: what time it is now.
 * @return if found in cache, or false if not (or TTL bad).
 */
int infra_get_lame_rtt(struct infra_cache* infra,
        struct sockaddr_storage* addr, socklen_t addrlen, 
	uint8_t* name, size_t namelen, uint16_t qtype, 
	int* lame, int* dnsseclame, int* reclame, int* rtt, int* lost,
	uint32_t timenow);

/**
 * Get memory used by the infra cache.
 * @param infra: infrastructure cache.
 * @return memory in use in bytes.
 */
size_t infra_get_mem(struct infra_cache* infra);

/** calculate size for the hashtable, does not count size of lameness,
 * so the hashtable is a fixed number of items */
size_t infra_host_sizefunc(void* k, void* d);

/** compare two addresses, returns -1, 0, or +1 */
int infra_host_compfunc(void* key1, void* key2);

/** delete key, and destroy the lock */
void infra_host_delkeyfunc(void* k, void* arg);

/** delete data and destroy the lameness hashtable */
void infra_host_deldatafunc(void* d, void* arg);

/** calculate size, which is fixed, zonename does not count so that
 * a fixed number of items is stored */
size_t infra_lame_sizefunc(void* k, void* d);

/** compare zone names, returns -1, 0, +1 */
int infra_lame_compfunc(void* key1, void* key2);

/** free key, lock and zonename */
void infra_lame_delkeyfunc(void* k, void* arg);

/** free the lameness data */
void infra_lame_deldatafunc(void* d, void* arg);

#endif /* SERVICES_CACHE_INFRA_H */
