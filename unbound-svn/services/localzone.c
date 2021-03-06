/*
 * services/localzone.c - local zones authority service.
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
 * This file contains functions to enable local zone authority service.
 */
#include "config.h"
#include "ldns/dname.h"
#include "ldns/host2wire.h"
#include "services/localzone.h"
#include "util/regional.h"
#include "util/config_file.h"
#include "util/data/dname.h"
#include "util/data/packed_rrset.h"
#include "util/data/msgencode.h"
#include "util/net_help.h"
#include "util/data/msgreply.h"
#include "util/data/msgparse.h"

struct local_zones* 
local_zones_create()
{
	struct local_zones* zones = (struct local_zones*)calloc(1, 
		sizeof(*zones));
	if(!zones)
		return NULL;
	rbtree_init(&zones->ztree, &local_zone_cmp);
	lock_quick_init(&zones->lock);
	lock_protect(&zones->lock, &zones->ztree, sizeof(zones->ztree));
	/* also lock protects the rbnode's in struct local_zone */
	return zones;
}

/** helper traverse to delete zones */
static void 
lzdel(rbnode_t* n, void* ATTR_UNUSED(arg))
{
	struct local_zone* z = (struct local_zone*)n->key;
	local_zone_delete(z);
}

void 
local_zones_delete(struct local_zones* zones)
{
	if(!zones)
		return;
	lock_quick_destroy(&zones->lock);
	/* walk through zones and delete them all */
	traverse_postorder(&zones->ztree, lzdel, NULL);
	free(zones);
}

void 
local_zone_delete(struct local_zone* z)
{
	if(!z)
		return;
	lock_rw_destroy(&z->lock);
	regional_destroy(z->region);
	free(z->name);
	free(z);
}

int 
local_zone_cmp(const void* z1, const void* z2)
{
	/* first sort on class, so that hierarchy can be maintained within
	 * a class */
	struct local_zone* a = (struct local_zone*)z1;
	struct local_zone* b = (struct local_zone*)z2;
	int m;
	if(a->dclass != b->dclass) {
		if(a->dclass < b->dclass)
			return -1;
		return 1;
	}
	return dname_lab_cmp(a->name, a->namelabs, b->name, b->namelabs, &m);
}

int 
local_data_cmp(const void* d1, const void* d2)
{
	struct local_data* a = (struct local_data*)d1;
	struct local_data* b = (struct local_data*)d2;
	int m;
	return dname_canon_lab_cmp(a->name, a->namelabs, b->name, 
		b->namelabs, &m);
}

/* form wireformat from text format domain name */
int
parse_dname(const char* str, uint8_t** res, size_t* len, int* labs)
{
	ldns_rdf* rdf;
	*res = NULL;
	*len = 0;
	*labs = 0;
	rdf = ldns_dname_new_frm_str(str);
	if(!rdf) {
		log_err("cannot parse name %s", str);
		return 0;
	}
	*res = memdup(ldns_rdf_data(rdf), ldns_rdf_size(rdf));
	ldns_rdf_deep_free(rdf);
	if(!*res) {
		log_err("out of memory");
		return 0;
	}
	*labs = dname_count_size_labels(*res, len);
	return 1;
}

/** create a new localzone */
static struct local_zone*
local_zone_create(uint8_t* nm, size_t len, int labs, 
	enum localzone_type t, uint16_t dclass)
{
	struct local_zone* z = (struct local_zone*)calloc(1, sizeof(*z));
	if(!z) {
		return NULL;
	}
	z->node.key = z;
	z->dclass = dclass;
	z->type = t;
	z->name = nm;
	z->namelen = len;
	z->namelabs = labs;
	lock_rw_init(&z->lock);
	z->region = regional_create();
	if(!z->region) {
		free(z);
		return NULL;
	}
	rbtree_init(&z->data, &local_data_cmp);
	lock_protect(&z->lock, &z->parent, sizeof(*z)-sizeof(rbnode_t));
	/* also the zones->lock protects node, parent, name*, class */
	return z;
}

/** enter a new zone with allocated dname returns with WRlock */
static struct local_zone*
lz_enter_zone_dname(struct local_zones* zones, uint8_t* nm, size_t len, 
	int labs, enum localzone_type t, uint16_t c)
{
	struct local_zone* z = local_zone_create(nm, len, labs, t, c);
	if(!z) {
		log_err("out of memory");
		return NULL;
	}

	/* add to rbtree */
	lock_quick_lock(&zones->lock);
	lock_rw_wrlock(&z->lock);
	if(!rbtree_insert(&zones->ztree, &z->node)) {
		log_warn("duplicate local-zone");
		lock_rw_unlock(&z->lock);
		local_zone_delete(z);
		lock_quick_unlock(&zones->lock);
		return NULL;
	}
	lock_quick_unlock(&zones->lock);
	return z;
}

/** enter a new zone */
static struct local_zone*
lz_enter_zone(struct local_zones* zones, const char* name, const char* type,
	uint16_t dclass)
{
	struct local_zone* z;
	enum localzone_type t;
	uint8_t* nm;
	size_t len;
	int labs;
	if(!parse_dname(name, &nm, &len, &labs)) {
		log_err("bad zone name %s %s", name, type);
		return NULL;
	}
	if(!local_zone_str2type(type, &t)) {
		log_err("bad lz_enter_zone type %s %s", name, type);
		free(nm);
		return NULL;
	}
	if(!(z=lz_enter_zone_dname(zones, nm, len, labs, t, dclass))) {
		log_err("could not enter zone %s %s", name, type);
		return NULL;
	}
	return z;
}

/** return name and class and rdata of rr; parses string */
static int
get_rr_content(const char* str, uint8_t** nm, uint16_t* type,
	uint16_t* dclass, uint32_t* ttl, ldns_buffer* rdata)
{
	ldns_rr* rr = NULL;
	ldns_status status = ldns_rr_new_frm_str(&rr, str, 3600, NULL, NULL);
	if(status != LDNS_STATUS_OK) {
		log_err("error parsing local-data '%s': %s",
			str, ldns_get_errorstr_by_id(status));
		ldns_rr_free(rr);
		return 0;
	}
	*nm = memdup(ldns_rdf_data(ldns_rr_owner(rr)), 
		ldns_rdf_size(ldns_rr_owner(rr)));
	if(!*nm) {
		log_err("out of memory");
		ldns_rr_free(rr);
		return 0;
	}
	*dclass = ldns_rr_get_class(rr);
	*type = ldns_rr_get_type(rr);
	*ttl = (uint32_t)ldns_rr_ttl(rr);
	ldns_buffer_clear(rdata);
	ldns_buffer_skip(rdata, 2);
	status = ldns_rr_rdata2buffer_wire(rdata, rr);
	ldns_rr_free(rr);
        if(status != LDNS_STATUS_OK) {
                log_err("error converting RR '%s' to wireformat: %s",
                        str, ldns_get_errorstr_by_id(status));
		free(*nm);
		*nm = NULL;
                return 0;
        }
        ldns_buffer_flip(rdata);
        ldns_buffer_write_u16_at(rdata, 0, ldns_buffer_limit(rdata) - 2);
	return 1;
}

/** return name and class of rr; parses string */
static int
get_rr_nameclass(const char* str, uint8_t** nm, uint16_t* dclass)
{
	ldns_rr* rr = NULL;
	ldns_status status = ldns_rr_new_frm_str(&rr, str, 3600, NULL, NULL);
	if(status != LDNS_STATUS_OK) {
		log_err("error parsing local-data '%s': %s",
			str, ldns_get_errorstr_by_id(status));
		ldns_rr_free(rr);
		return 0;
	}
	*nm = memdup(ldns_rdf_data(ldns_rr_owner(rr)), 
		ldns_rdf_size(ldns_rr_owner(rr)));
	*dclass = ldns_rr_get_class(rr);
	ldns_rr_free(rr);
	if(!*nm) {
		log_err("out of memory");
		return 0;
	}
	return 1;
}

/**
 * Find an rrset in local data structure.
 * @param data: local data domain name structure.
 * @param type: type to look for (host order).
 * @return rrset pointer or NULL if not found.
 */
struct local_rrset*
local_data_find_type(struct local_data* data, uint16_t type)
{
	struct local_rrset* p;
	type = htons(type);
	for(p = data->rrsets; p; p = p->next) {
		if(p->rrset->rk.type == type)
			return p;
	}
	return NULL;
}

/** check for RR duplicates */
static int
rr_is_duplicate(struct packed_rrset_data* pd, ldns_buffer* buf)
{
	size_t i;
	for(i=0; i<pd->count; i++) {
		if(ldns_buffer_limit(buf) == pd->rr_len[i] &&
			memcmp(ldns_buffer_begin(buf), pd->rr_data[i],
				ldns_buffer_limit(buf)) == 0) 
			return 1;
	}
	return 0;
}

/** new local_rrset */
static struct local_rrset*
new_local_rrset(struct regional* region, struct local_data* node,
	uint16_t rrtype, uint16_t rrclass)
{
	struct packed_rrset_data* pd;
	struct local_rrset* rrset = (struct local_rrset*)
		regional_alloc_zero(region, sizeof(*rrset));
	if(!rrset) {
		log_err("out of memory");
		return NULL;
	}
	rrset->next = node->rrsets;
	node->rrsets = rrset;
	rrset->rrset = (struct ub_packed_rrset_key*)
		regional_alloc_zero(region, sizeof(*rrset->rrset));
	if(!rrset->rrset) {
		log_err("out of memory");
		return NULL;
	}
	rrset->rrset->entry.key = rrset->rrset;
	pd = (struct packed_rrset_data*)regional_alloc_zero(region,
		sizeof(*pd));
	if(!pd) {
		log_err("out of memory");
		return NULL;
	}
	pd->trust = rrset_trust_prim_noglue;
	pd->security = sec_status_insecure;
	rrset->rrset->entry.data = pd;
	rrset->rrset->rk.dname = node->name;
	rrset->rrset->rk.dname_len = node->namelen;
	rrset->rrset->rk.type = htons(rrtype);
	rrset->rrset->rk.rrset_class = htons(rrclass);
	return rrset;
}

/** insert RR into RRset data structure; Wastes a couple of bytes */
static int
insert_rr(struct regional* region, struct packed_rrset_data* pd,
	ldns_buffer* buf, uint32_t ttl)
{
	size_t* oldlen = pd->rr_len;
	uint32_t* oldttl = pd->rr_ttl;
	uint8_t** olddata = pd->rr_data;

	/* add RR to rrset */
	pd->count++;
	pd->rr_len = regional_alloc(region, sizeof(*pd->rr_len)*pd->count);
	pd->rr_ttl = regional_alloc(region, sizeof(*pd->rr_ttl)*pd->count);
	pd->rr_data = regional_alloc(region, sizeof(*pd->rr_data)*pd->count);
	if(!pd->rr_len || !pd->rr_ttl || !pd->rr_data) {
		log_err("out of memory");
		return 0;
	}
	if(pd->count > 1) {
		memcpy(pd->rr_len+1, oldlen, 
			sizeof(*pd->rr_len)*(pd->count-1));
		memcpy(pd->rr_ttl+1, oldttl, 
			sizeof(*pd->rr_ttl)*(pd->count-1));
		memcpy(pd->rr_data+1, olddata, 
			sizeof(*pd->rr_data)*(pd->count-1));
	}
	pd->rr_len[0] = ldns_buffer_limit(buf);
	pd->rr_ttl[0] = ttl;
	pd->rr_data[0] = regional_alloc_init(region,
		ldns_buffer_begin(buf), ldns_buffer_limit(buf));
	if(!pd->rr_data[0]) {
		log_err("out of memory");
		return 0;
	}
	return 1;
}

/** find a data node by exact name */
static struct local_data* 
lz_find_node(struct local_zone* z, uint8_t* nm, size_t nmlen, int nmlabs)
{
	struct local_data key;
	key.node.key = &key;
	key.name = nm;
	key.namelen = nmlen;
	key.namelabs = nmlabs;
	return (struct local_data*)rbtree_search(&z->data, &key.node);
}

/** find a node, create it if not and all its empty nonterminal parents */
static int
lz_find_create_node(struct local_zone* z, uint8_t* nm, size_t nmlen, 
	int nmlabs, struct local_data** res)
{
	struct local_data* ld = lz_find_node(z, nm, nmlen, nmlabs);
	if(!ld) {
		/* create a domain name to store rr. */
		ld = (struct local_data*)regional_alloc_zero(z->region,
			sizeof(*ld));
		if(!ld) {
			log_err("out of memory adding local data");
			return 0;
		}
		ld->node.key = ld;
		ld->name = regional_alloc_init(z->region, nm, nmlen);
		if(!ld->name) {
			log_err("out of memory");
			return 0;
		}
		ld->namelen = nmlen;
		ld->namelabs = nmlabs;
		if(!rbtree_insert(&z->data, &ld->node)) {
			log_assert(0); /* duplicate name */
		}
		/* see if empty nonterminals need to be created */
		if(nmlabs > z->namelabs) {
			dname_remove_label(&nm, &nmlen);
			if(!lz_find_create_node(z, nm, nmlen, nmlabs-1, res))
				return 0;
		}
	}
	*res = ld;
	return 1;
}

/** enter data RR into auth zone */
static int
lz_enter_rr_into_zone(struct local_zone* z, ldns_buffer* buf,
	const char* rrstr)
{
	uint8_t* nm;
	size_t nmlen;
	int nmlabs;
	struct local_data* node;
	struct local_rrset* rrset;
	struct packed_rrset_data* pd;
	uint16_t rrtype, rrclass;
	uint32_t ttl;
	if(!get_rr_content(rrstr, &nm, &rrtype, &rrclass, &ttl, buf)) {
		log_err("bad local-data: %s", rrstr);
		return 0;
	}
	log_assert(z->dclass == rrclass);
	if(z->type == local_zone_redirect &&
		query_dname_compare(z->name, nm) != 0) {
		log_err("local-data in redirect zone must reside at top of zone"
			", not at %s", rrstr);
		free(nm);
		return 0;
	}
	nmlabs = dname_count_size_labels(nm, &nmlen);
	if(!lz_find_create_node(z, nm, nmlen, nmlabs, &node)) {
		free(nm);
		return 0;
	}
	log_assert(node);
	free(nm);

	rrset = local_data_find_type(node, rrtype);
	if(!rrset) {
		rrset = new_local_rrset(z->region, node, rrtype, rrclass);
		if(!rrset)
			return 0;
		if(query_dname_compare(node->name, z->name) == 0) {
			if(rrtype == LDNS_RR_TYPE_NSEC)
			  rrset->rrset->rk.flags = PACKED_RRSET_NSEC_AT_APEX;
			if(rrtype == LDNS_RR_TYPE_SOA)
				z->soa = rrset->rrset;
		}
	} 
	pd = (struct packed_rrset_data*)rrset->rrset->entry.data;
	log_assert(rrset && pd);

	/* check for duplicate RR */
	if(rr_is_duplicate(pd, buf)) {
		verbose(VERB_ALGO, "ignoring duplicate RR: %s", rrstr);
		return 1;
	} 
	return insert_rr(z->region, pd, buf, ttl);
}

/** enter a data RR into auth data; a zone for it must exist */
static int
lz_enter_rr_str(struct local_zones* zones, const char* rr, ldns_buffer* buf)
{
	uint8_t* rr_name;
	uint16_t rr_class;
	size_t len;
	int labs;
	struct local_zone* z;
	int r;
	if(!get_rr_nameclass(rr, &rr_name, &rr_class)) {
		log_err("bad rr %s", rr);
		return 0;
	}
	labs = dname_count_size_labels(rr_name, &len);
	lock_quick_lock(&zones->lock);
	z = local_zones_lookup(zones, rr_name, len, labs, rr_class);
	if(!z) {
		lock_quick_unlock(&zones->lock);
		fatal_exit("internal error: no zone for rr %s", rr);
	}
	lock_rw_wrlock(&z->lock);
	lock_quick_unlock(&zones->lock);
	free(rr_name);
	r = lz_enter_rr_into_zone(z, buf, rr);
	lock_rw_unlock(&z->lock);
	return r;
}

/** parse local-zone: statements */
static int
lz_enter_zones(struct local_zones* zones, struct config_file* cfg)
{
	struct config_str2list* p;
	struct local_zone* z;
	for(p = cfg->local_zones; p; p = p->next) {
		if(!(z=lz_enter_zone(zones, p->str, p->str2, 
			LDNS_RR_CLASS_IN)))
			return 0;
		lock_rw_unlock(&z->lock);
	}
	return 1;
}

/** lookup a zone in rbtree; exact match only; SLOW due to parse */
static int
lz_exists(struct local_zones* zones, const char* name)
{
	struct local_zone z;
	z.node.key = &z;
	z.dclass = LDNS_RR_CLASS_IN;
	if(!parse_dname(name, &z.name, &z.namelen, &z.namelabs)) {
		log_err("bad name %s", name);
		return 0;
	}
	lock_quick_lock(&zones->lock);
	if(rbtree_search(&zones->ztree, &z.node)) {
		lock_quick_unlock(&zones->lock);
		free(z.name);
		return 1;
	}
	lock_quick_unlock(&zones->lock);
	free(z.name);
	return 0;
}

/** lookup a zone in cfg->nodefault list */
static int
lz_nodefault(struct config_file* cfg, const char* name)
{
	struct config_strlist* p;
	size_t len = strlen(name);
	if(len == 0) return 0;
	if(name[len-1] == '.') len--;

	for(p = cfg->local_zones_nodefault; p; p = p->next) {
		/* compare zone name, lowercase, compare without ending . */
		if(strncasecmp(p->str, name, len) == 0 && 
			(strlen(p->str) == len || (strlen(p->str)==len+1 &&
			p->str[len] == '.')))
			return 1;
	}
	return 0;
}

/** enter AS112 default zone */
static int
add_as112_default(struct local_zones* zones, struct config_file* cfg,
        ldns_buffer* buf, const char* name)
{
	struct local_zone* z;
	char str[1024]; /* known long enough */
	if(lz_exists(zones, name) || lz_nodefault(cfg, name))
		return 1; /* do not enter default content */
	if(!(z=lz_enter_zone(zones, name, "static", LDNS_RR_CLASS_IN)))
		return 0;
	snprintf(str, sizeof(str), "%s 10800 IN SOA localhost. "
		"nobody.invalid. 1 3600 1200 604800 10800", name);
	if(!lz_enter_rr_into_zone(z, buf, str)) {
		lock_rw_unlock(&z->lock);
		return 0;
	}
	snprintf(str, sizeof(str), "%s 10800 IN NS localhost. ", name);
	if(!lz_enter_rr_into_zone(z, buf, str)) {
		lock_rw_unlock(&z->lock);
		return 0;
	}
	lock_rw_unlock(&z->lock);
	return 1;
}

/** enter default zones */
static int
lz_enter_defaults(struct local_zones* zones, struct config_file* cfg,
	ldns_buffer* buf)
{
	struct local_zone* z;

	/* localhost. zone */
	if(!lz_exists(zones, "localhost.") &&
		!lz_nodefault(cfg, "localhost.")) {
		if(!(z=lz_enter_zone(zones, "localhost.", "static", 
			LDNS_RR_CLASS_IN)) ||
		   !lz_enter_rr_into_zone(z, buf,
			"localhost. 10800 IN NS localhost.") ||
		   !lz_enter_rr_into_zone(z, buf,
			"localhost. 10800 IN SOA localhost. nobody.invalid. "
			"1 3600 1200 604800 10800") ||
		   !lz_enter_rr_into_zone(z, buf,
			"localhost. 10800 IN A 127.0.0.1") ||
		   !lz_enter_rr_into_zone(z, buf,
			"localhost. 10800 IN AAAA ::1")) {
			log_err("out of memory adding default zone");
			if(z) { lock_rw_unlock(&z->lock); }
			return 0;
		}
		lock_rw_unlock(&z->lock);
	}
	/* reverse ip4 zone */
	if(!lz_exists(zones, "127.in-addr.arpa.") &&
		!lz_nodefault(cfg, "127.in-addr.arpa.")) {
		if(!(z=lz_enter_zone(zones, "127.in-addr.arpa.", "static", 
			LDNS_RR_CLASS_IN)) ||
		   !lz_enter_rr_into_zone(z, buf,
			"127.in-addr.arpa. 10800 IN NS localhost.") ||
		   !lz_enter_rr_into_zone(z, buf,
			"127.in-addr.arpa. 10800 IN SOA localhost. "
			"nobody.invalid. 1 3600 1200 604800 10800") ||
		   !lz_enter_rr_into_zone(z, buf,
			"1.0.0.127.in-addr.arpa. 10800 IN PTR localhost.")) {
			log_err("out of memory adding default zone");
			if(z) { lock_rw_unlock(&z->lock); }
			return 0;
		}
		lock_rw_unlock(&z->lock);
	}
	/* reverse ip6 zone */
	if(!lz_exists(zones, "1.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.ip6.arpa.") &&
		!lz_nodefault(cfg, "1.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.ip6.arpa.")) {
		if(!(z=lz_enter_zone(zones, "1.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.ip6.arpa.", "static", 
			LDNS_RR_CLASS_IN)) ||
		   !lz_enter_rr_into_zone(z, buf,
			"1.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.ip6.arpa. 10800 IN NS localhost.") ||
		   !lz_enter_rr_into_zone(z, buf,
			"1.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.ip6.arpa. 10800 IN SOA localhost. "
			"nobody.invalid. 1 3600 1200 604800 10800") ||
		   !lz_enter_rr_into_zone(z, buf,
			"1.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.ip6.arpa. 10800 IN PTR localhost.")) {
			log_err("out of memory adding default zone");
			if(z) { lock_rw_unlock(&z->lock); }
			return 0;
		}
		lock_rw_unlock(&z->lock);
	}
	if (	!add_as112_default(zones, cfg, buf, "10.in-addr.arpa.") ||
		!add_as112_default(zones, cfg, buf, "16.172.in-addr.arpa.") ||
		!add_as112_default(zones, cfg, buf, "17.172.in-addr.arpa.") ||
		!add_as112_default(zones, cfg, buf, "18.172.in-addr.arpa.") ||
		!add_as112_default(zones, cfg, buf, "19.172.in-addr.arpa.") ||
		!add_as112_default(zones, cfg, buf, "20.172.in-addr.arpa.") ||
		!add_as112_default(zones, cfg, buf, "21.172.in-addr.arpa.") ||
		!add_as112_default(zones, cfg, buf, "22.172.in-addr.arpa.") ||
		!add_as112_default(zones, cfg, buf, "23.172.in-addr.arpa.") ||
		!add_as112_default(zones, cfg, buf, "24.172.in-addr.arpa.") ||
		!add_as112_default(zones, cfg, buf, "25.172.in-addr.arpa.") ||
		!add_as112_default(zones, cfg, buf, "26.172.in-addr.arpa.") ||
		!add_as112_default(zones, cfg, buf, "27.172.in-addr.arpa.") ||
		!add_as112_default(zones, cfg, buf, "28.172.in-addr.arpa.") ||
		!add_as112_default(zones, cfg, buf, "29.172.in-addr.arpa.") ||
		!add_as112_default(zones, cfg, buf, "30.172.in-addr.arpa.") ||
		!add_as112_default(zones, cfg, buf, "31.172.in-addr.arpa.") ||
		!add_as112_default(zones, cfg, buf, "168.192.in-addr.arpa.") ||
		!add_as112_default(zones, cfg, buf, "0.in-addr.arpa.") ||
		!add_as112_default(zones, cfg, buf, "254.169.in-addr.arpa.") ||
		!add_as112_default(zones, cfg, buf, "2.0.192.in-addr.arpa.") ||
		!add_as112_default(zones, cfg, buf, "100.51.198.in-addr.arpa.") ||
		!add_as112_default(zones, cfg, buf, "113.0.203.in-addr.arpa.") ||
		!add_as112_default(zones, cfg, buf, "255.255.255.255.in-addr.arpa.") ||
		!add_as112_default(zones, cfg, buf, "0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.ip6.arpa.") ||
		!add_as112_default(zones, cfg, buf, "d.f.ip6.arpa.") ||
		!add_as112_default(zones, cfg, buf, "8.e.f.ip6.arpa.") ||
		!add_as112_default(zones, cfg, buf, "9.e.f.ip6.arpa.") ||
		!add_as112_default(zones, cfg, buf, "a.e.f.ip6.arpa.") ||
		!add_as112_default(zones, cfg, buf, "b.e.f.ip6.arpa.") ||
		!add_as112_default(zones, cfg, buf, "0.1.1.0.0.2.ip6.arpa.") ||
		!add_as112_default(zones, cfg, buf, "8.b.d.0.1.0.0.2.ip6.arpa.")) {
		log_err("out of memory adding default zone");
		return 0;
	}
	return 1;
}

/** setup parent pointers, so that a lookup can be done for closest match */
static void
init_parents(struct local_zones* zones)
{
        struct local_zone* node, *prev = NULL, *p;
        int m;
	lock_quick_lock(&zones->lock);
        RBTREE_FOR(node, struct local_zone*, &zones->ztree) {
		lock_rw_wrlock(&node->lock);
                node->parent = NULL;
                if(!prev || prev->dclass != node->dclass) {
                        prev = node;
			lock_rw_unlock(&node->lock);
                        continue;
                }
                (void)dname_lab_cmp(prev->name, prev->namelabs, node->name,
                        node->namelabs, &m); /* we know prev is smaller */
                /* sort order like: . com. bla.com. zwb.com. net. */
                /* find the previous, or parent-parent-parent */
                for(p = prev; p; p = p->parent)
                        /* looking for name with few labels, a parent */
                        if(p->namelabs <= m) {
                                /* ==: since prev matched m, this is closest*/
                                /* <: prev matches more, but is not a parent,
                                 * this one is a (grand)parent */
                                node->parent = p;
                                break;
                        }
                prev = node;
		lock_rw_unlock(&node->lock);
        }
	lock_quick_unlock(&zones->lock);
}

/** enter implicit transparent zone for local-data: without local-zone: */
static int
lz_setup_implicit(struct local_zones* zones, struct config_file* cfg)
{
	/* walk over all items that have no parent zone and find
	 * the name that covers them all (could be the root) and
	 * add that as a transparent zone */
	struct config_strlist* p;
	int have_name = 0;
	int have_other_classes = 0;
	uint16_t dclass = 0;
	uint8_t* nm = 0;
	size_t nmlen = 0;
	int nmlabs = 0;
	int match = 0; /* number of labels match count */

	init_parents(zones); /* to enable local_zones_lookup() */
	for(p = cfg->local_data; p; p = p->next) {
		uint8_t* rr_name;
		uint16_t rr_class;
		size_t len;
		int labs;
		if(!get_rr_nameclass(p->str, &rr_name, &rr_class)) {
			log_err("Bad local-data RR %s", p->str);
			return 0;
		}
		labs = dname_count_size_labels(rr_name, &len);
		lock_quick_lock(&zones->lock);
		if(!local_zones_lookup(zones, rr_name, len, labs, rr_class)) {
			if(!have_name) {
				dclass = rr_class;
				nm = rr_name;
				nmlen = len;
				nmlabs = labs;
				match = labs;
				have_name = 1;
			} else {
				int m;
				if(rr_class != dclass) {
					/* process other classes later */
					free(rr_name);
					have_other_classes = 1;
					lock_quick_unlock(&zones->lock);
					continue;
				}
				/* find smallest shared topdomain */
				(void)dname_lab_cmp(nm, nmlabs, 
					rr_name, labs, &m);
				free(rr_name);
				if(m < match)
					match = m;
			}
		} else free(rr_name);
		lock_quick_unlock(&zones->lock);
	}
	if(have_name) {
		uint8_t* n2;
		struct local_zone* z;
		/* allocate zone of smallest shared topdomain to contain em */
		n2 = nm;
		dname_remove_labels(&n2, &nmlen, nmlabs - match);
		n2 = memdup(n2, nmlen);
		free(nm);
		if(!n2) {
			log_err("out of memory");
			return 0;
		}
		log_nametypeclass(VERB_ALGO, "implicit transparent local-zone", 
			n2, 0, dclass);
		if(!(z=lz_enter_zone_dname(zones, n2, nmlen, match, 
			local_zone_transparent, dclass))) {
			return 0;
		}
		lock_rw_unlock(&z->lock);
	}
	if(have_other_classes) { 
		/* restart to setup other class */
		return lz_setup_implicit(zones, cfg);
	}
	return 1;
}

/** enter auth data */
static int
lz_enter_data(struct local_zones* zones, struct config_file* cfg,
	ldns_buffer* buf)
{
	struct config_strlist* p;
	for(p = cfg->local_data; p; p = p->next) {
		if(!lz_enter_rr_str(zones, p->str, buf))
			return 0;
	}
	return 1;
}

/** free memory from config */
static void
lz_freeup_cfg(struct config_file* cfg)
{
	config_deldblstrlist(cfg->local_zones);
	cfg->local_zones = NULL;
	config_delstrlist(cfg->local_zones_nodefault);
	cfg->local_zones_nodefault = NULL;
	config_delstrlist(cfg->local_data);
	cfg->local_data = NULL;
}

int 
local_zones_apply_cfg(struct local_zones* zones, struct config_file* cfg)
{
	ldns_buffer* buf = ldns_buffer_new(65535);
	if(!buf) fatal_exit("cannot create temporary buffer");

	/* create zones from zone statements. */
	if(!lz_enter_zones(zones, cfg)) {
		ldns_buffer_free(buf);
		return 0;
	}
	/* apply default zones+content (unless disabled, or overridden) */
	if(!lz_enter_defaults(zones, cfg, buf)) {
		ldns_buffer_free(buf);
		return 0;
	}
	/* create implicit transparent zone from data. */
	if(!lz_setup_implicit(zones, cfg)) {
		ldns_buffer_free(buf);
		return 0;
	}

	/* setup parent ptrs for lookup during data entry */
	init_parents(zones);
	/* insert local data */
	if(!lz_enter_data(zones, cfg, buf)) {
		ldns_buffer_free(buf);
		return 0;
	}
	/* freeup memory from cfg struct. */
	lz_freeup_cfg(cfg);
	ldns_buffer_free(buf);
	return 1;
}

struct local_zone* 
local_zones_lookup(struct local_zones* zones,
        uint8_t* name, size_t len, int labs, uint16_t dclass)
{
	rbnode_t* res = NULL;
	struct local_zone *result;
	struct local_zone key;
	key.node.key = &key;
	key.dclass = dclass;
	key.name = name;
	key.namelen = len;
	key.namelabs = labs;
	if(rbtree_find_less_equal(&zones->ztree, &key, &res)) {
		/* exact */
		return (struct local_zone*)res;
	} else {
	        /* smaller element (or no element) */
                int m;
                result = (struct local_zone*)res;
                if(!result || result->dclass != dclass)
                        return NULL;
                /* count number of labels matched */
                (void)dname_lab_cmp(result->name, result->namelabs, key.name,
                        key.namelabs, &m);
                while(result) { /* go up until qname is subdomain of zone */
                        if(result->namelabs <= m)
                                break;
                        result = result->parent;
                }
		return result;
	}
}

struct local_zone* 
local_zones_find(struct local_zones* zones,
        uint8_t* name, size_t len, int labs, uint16_t dclass)
{
	struct local_zone key;
	key.node.key = &key;
	key.dclass = dclass;
	key.name = name;
	key.namelen = len;
	key.namelabs = labs;
	/* exact */
	return (struct local_zone*)rbtree_search(&zones->ztree, &key);
}

/** print all RRsets in local zone */
static void 
local_zone_out(struct local_zone* z)
{
	struct local_data* d;
	struct local_rrset* p;
	RBTREE_FOR(d, struct local_data*, &z->data) {
		for(p = d->rrsets; p; p = p->next) {
			log_nametypeclass(0, "rrset", d->name, 
				ntohs(p->rrset->rk.type),
				ntohs(p->rrset->rk.rrset_class));
		}
	}
}

void local_zones_print(struct local_zones* zones)
{
	struct local_zone* z;
	lock_quick_lock(&zones->lock);
	log_info("number of auth zones %u", (unsigned)zones->ztree.count);
	RBTREE_FOR(z, struct local_zone*, &zones->ztree) {
		lock_rw_rdlock(&z->lock);
		switch(z->type) {
		case local_zone_deny:
			log_nametypeclass(0, "deny zone", 
				z->name, 0, z->dclass);
			break;
		case local_zone_refuse:
			log_nametypeclass(0, "refuse zone", 
				z->name, 0, z->dclass);
			break;
		case local_zone_redirect:
			log_nametypeclass(0, "redirect zone", 
				z->name, 0, z->dclass);
			break;
		case local_zone_transparent:
			log_nametypeclass(0, "transparent zone", 
				z->name, 0, z->dclass);
			break;
		case local_zone_static:
			log_nametypeclass(0, "static zone", 
				z->name, 0, z->dclass);
			break;
		default:
			log_nametypeclass(0, "badtyped zone", 
				z->name, 0, z->dclass);
			break;
		}
		local_zone_out(z);
		lock_rw_unlock(&z->lock);
	}
	lock_quick_unlock(&zones->lock);
}

/** encode answer consisting of 1 rrset */
static int
local_encode(struct query_info* qinfo, struct edns_data* edns, 
	ldns_buffer* buf, struct regional* temp, 
	struct ub_packed_rrset_key* rrset, int ansec, int rcode)
{
	struct reply_info rep;
	uint16_t udpsize;
	/* make answer with time=0 for fixed TTL values */
	memset(&rep, 0, sizeof(rep));
	rep.flags = (uint16_t)((BIT_QR | BIT_AA | BIT_RA) | rcode);
	rep.qdcount = 1;
	if(ansec)
		rep.an_numrrsets = 1;
	else	rep.ns_numrrsets = 1;
	rep.rrset_count = 1;
	rep.rrsets = &rrset;
	udpsize = edns->udp_size;
	edns->edns_version = EDNS_ADVERTISED_VERSION;
	edns->udp_size = EDNS_ADVERTISED_SIZE;
	edns->ext_rcode = 0;
	edns->bits &= EDNS_DO;
	if(!reply_info_answer_encode(qinfo, &rep, 
		*(uint16_t*)ldns_buffer_begin(buf),
		ldns_buffer_read_u16_at(buf, 2),
		buf, 0, 0, temp, udpsize, edns, 
		(int)(edns->bits&EDNS_DO), 0))
		error_encode(buf, (LDNS_RCODE_SERVFAIL|BIT_AA), qinfo,
			*(uint16_t*)ldns_buffer_begin(buf),
		       ldns_buffer_read_u16_at(buf, 2), edns);
	return 1;
}

/** answer local data match */
static int
local_data_answer(struct local_zone* z, struct query_info* qinfo,
	struct edns_data* edns, ldns_buffer* buf, struct regional* temp,
	int labs, struct local_data** ldp)
{
	struct local_data key;
	struct local_data* ld;
	struct local_rrset* lr;
	key.node.key = &key;
	key.name = qinfo->qname;
	key.namelen = qinfo->qname_len;
	key.namelabs = labs;
	if(z->type == local_zone_redirect) {
		key.name = z->name;
		key.namelen = z->namelen;
		key.namelabs = z->namelabs;
	}
	ld = (struct local_data*)rbtree_search(&z->data, &key.node);
	*ldp = ld;
	if(!ld) {
		return 0;
	}
	lr = local_data_find_type(ld, qinfo->qtype);
	if(!lr)
		return 0;
	if(z->type == local_zone_redirect) {
		/* convert rrset name to query name; like a wildcard */
		struct ub_packed_rrset_key r = *lr->rrset;
		r.rk.dname = qinfo->qname;
		r.rk.dname_len = qinfo->qname_len;
		return local_encode(qinfo, edns, buf, temp, &r, 1, 
			LDNS_RCODE_NOERROR);
	}
	return local_encode(qinfo, edns, buf, temp, lr->rrset, 1, 
		LDNS_RCODE_NOERROR);
}

/** 
 * answer in case where no exact match is found 
 * @param z: zone for query
 * @param qinfo: query
 * @param edns: edns from query
 * @param buf: buffer for answer.
 * @param temp: temp region for encoding
 * @param ld: local data, if NULL, no such name exists in localdata.
 * @return 1 if a reply is to be sent, 0 if not.
 */
static int
lz_zone_answer(struct local_zone* z, struct query_info* qinfo,
	struct edns_data* edns, ldns_buffer* buf, struct regional* temp,
	struct local_data* ld)
{
	if(z->type == local_zone_deny) {
		/** no reply at all, signal caller by clearing buffer. */
		ldns_buffer_clear(buf);
		ldns_buffer_flip(buf);
		return 1;
	} else if(z->type == local_zone_refuse) {
		error_encode(buf, (LDNS_RCODE_REFUSED|BIT_AA), qinfo,
			*(uint16_t*)ldns_buffer_begin(buf),
		       ldns_buffer_read_u16_at(buf, 2), edns);
		return 1;
	} else if(z->type == local_zone_static ||
		z->type == local_zone_redirect) {
		/* for static, reply nodata or nxdomain
		 * for redirect, reply nodata */
		/* no additional section processing,
		 * cname, dname or wildcard processing,
		 * or using closest match for NSEC.
		 * or using closest match for returning delegation downwards
		 */
		int rcode = ld?LDNS_RCODE_NOERROR:LDNS_RCODE_NXDOMAIN;
		if(z->soa)
			return local_encode(qinfo, edns, buf, temp, 
				z->soa, 0, rcode);
		error_encode(buf, (rcode|BIT_AA), qinfo, 
			*(uint16_t*)ldns_buffer_begin(buf), 
			ldns_buffer_read_u16_at(buf, 2), edns);
		return 1;
	} 
	/* else z->type == local_zone_transparent */

	/* if the zone is transparent and the name exists, but the type
	 * does not, then we should make this noerror/nodata */
	if(ld && ld->rrsets) {
		int rcode = LDNS_RCODE_NOERROR;
		if(z->soa)
			return local_encode(qinfo, edns, buf, temp, 
				z->soa, 0, rcode);
		error_encode(buf, (rcode|BIT_AA), qinfo, 
			*(uint16_t*)ldns_buffer_begin(buf), 
			ldns_buffer_read_u16_at(buf, 2), edns);
		return 1;
	}

	/* stop here, and resolve further on */
	return 0;
}

int 
local_zones_answer(struct local_zones* zones, struct query_info* qinfo,
	struct edns_data* edns, ldns_buffer* buf, struct regional* temp)
{
	/* see if query is covered by a zone,
	 * 	if so:	- try to match (exact) local data 
	 * 		- look at zone type for negative response. */
	int labs = dname_count_labels(qinfo->qname);
	struct local_data* ld;
	struct local_zone* z;
	int r;
	lock_quick_lock(&zones->lock);
	z = local_zones_lookup(zones, qinfo->qname,
		qinfo->qname_len, labs, qinfo->qclass);
	if(!z) {
		lock_quick_unlock(&zones->lock);
		return 0;
	}
	lock_rw_rdlock(&z->lock);
	lock_quick_unlock(&zones->lock);

	if(local_data_answer(z, qinfo, edns, buf, temp, labs, &ld)) {
		lock_rw_unlock(&z->lock);
		return 1;
	}
	r = lz_zone_answer(z, qinfo, edns, buf, temp, ld);
	lock_rw_unlock(&z->lock);
	return r;
}

const char* local_zone_type2str(enum localzone_type t)
{
	switch(t) {
		case local_zone_deny: return "deny";
		case local_zone_refuse: return "refuse";
		case local_zone_redirect: return "redirect";
		case local_zone_transparent: return "transparent";
		case local_zone_static: return "static";
		case local_zone_nodefault: return "nodefault";
	}
	return "badtyped"; 
}

int local_zone_str2type(const char* type, enum localzone_type* t)
{
	if(strcmp(type, "deny") == 0)
		*t = local_zone_deny;
	else if(strcmp(type, "refuse") == 0)
		*t = local_zone_refuse;
	else if(strcmp(type, "static") == 0)
		*t = local_zone_static;
	else if(strcmp(type, "transparent") == 0)
		*t = local_zone_transparent;
	else if(strcmp(type, "redirect") == 0)
		*t = local_zone_redirect;
	else return 0;
	return 1;
}

/** iterate over the kiddies of the given name and set their parent ptr */
static void
set_kiddo_parents(struct local_zone* z, struct local_zone* match, 
	struct local_zone* newp)
{
	/* both zones and z are locked already */
	/* in the sorted rbtree, the kiddies of z are located after z */
	/* z must be present in the tree */
	struct local_zone* p = z;
	p = (struct local_zone*)rbtree_next(&p->node);
	while(p!=(struct local_zone*)RBTREE_NULL &&
		p->dclass == z->dclass && dname_strict_subdomain(p->name,
		p->namelabs, z->name, z->namelabs)) {
		/* update parent ptr */
		/* only when matches with existing parent pointer, so that
		 * deeper child structures are not touched, i.e.
		 * update of x, and a.x, b.x, f.b.x, g.b.x, c.x, y
		 * gets to update a.x, b.x and c.x */
		lock_rw_wrlock(&p->lock);
		if(p->parent == match)
			p->parent = newp;
		lock_rw_unlock(&p->lock);
		p = (struct local_zone*)rbtree_next(&p->node);
	}
}

struct local_zone* local_zones_add_zone(struct local_zones* zones,
	uint8_t* name, size_t len, int labs, uint16_t dclass,
	enum localzone_type tp)
{
	/* create */
	struct local_zone* z = local_zone_create(name, len, labs, tp, dclass);
	if(!z) return NULL;
	lock_rw_wrlock(&z->lock);

	/* find the closest parent */
	z->parent = local_zones_find(zones, name, len, labs, dclass);

	/* insert into the tree */
	if(!rbtree_insert(&zones->ztree, &z->node)) {
		/* duplicate entry! */
		lock_rw_unlock(&z->lock);
		local_zone_delete(z);
		log_err("internal: duplicate entry in local_zones_add_zone");
		return NULL;
	}

	/* set parent pointers right */
	set_kiddo_parents(z, z->parent, z);

	lock_rw_unlock(&z->lock);
	return z;
}

void local_zones_del_zone(struct local_zones* zones, struct local_zone* z)
{
	/* fix up parents in tree */
	lock_rw_wrlock(&z->lock);
	set_kiddo_parents(z, z, z->parent);

	/* remove from tree */
	(void)rbtree_delete(&zones->ztree, z);

	/* delete the zone */
	lock_rw_unlock(&z->lock);
	local_zone_delete(z);
}

int
local_zones_add_RR(struct local_zones* zones, const char* rr, ldns_buffer* buf)
{
	uint8_t* rr_name;
	uint16_t rr_class;
	size_t len;
	int labs;
	struct local_zone* z;
	int r;
	if(!get_rr_nameclass(rr, &rr_name, &rr_class)) {
		return 0;
	}
	labs = dname_count_size_labels(rr_name, &len);
	lock_quick_lock(&zones->lock);
	z = local_zones_lookup(zones, rr_name, len, labs, rr_class);
	if(!z) {
		z = local_zones_add_zone(zones, rr_name, len, labs, rr_class,
			local_zone_transparent);
		if(!z) {
			lock_quick_unlock(&zones->lock);
			return 0;
		}
	} else {
		free(rr_name);
	}
	lock_rw_wrlock(&z->lock);
	lock_quick_unlock(&zones->lock);
	r = lz_enter_rr_into_zone(z, buf, rr);
	lock_rw_unlock(&z->lock);
	return r;
}

/** returns true if the node is terminal so no deeper domain names exist */
static int
is_terminal(struct local_data* d)
{
	/* for empty nonterminals, the deeper domain names are sorted
	 * right after them, so simply check the next name in the tree 
	 */
	struct local_data* n = (struct local_data*)rbtree_next(&d->node);
	if(n == (struct local_data*)RBTREE_NULL)
		return 1; /* last in tree, no deeper node */
	if(dname_strict_subdomain(n->name, n->namelabs, d->name, d->namelabs))
		return 0; /* there is a deeper node */
	return 1;
}

/** delete empty terminals from tree when final data is deleted */
static void 
del_empty_term(struct local_zone* z, struct local_data* d, 
	uint8_t* name, size_t len, int labs)
{
	while(d && d->rrsets == NULL && is_terminal(d)) {
		/* is this empty nonterminal? delete */
		/* note, no memory recycling in zone region */
		(void)rbtree_delete(&z->data, d);

		/* go up and to the next label */
		if(dname_is_root(name))
			return;
		dname_remove_label(&name, &len);
		labs--;
		d = lz_find_node(z, name, len, labs);
	}
}

void local_zones_del_data(struct local_zones* zones, 
	uint8_t* name, size_t len, int labs, uint16_t dclass)
{
	/* find zone */
	struct local_zone* z;
	struct local_data* d;
	lock_quick_lock(&zones->lock);
	z = local_zones_lookup(zones, name, len, labs, dclass);
	if(!z) {
		/* no such zone, we're done */
		lock_quick_unlock(&zones->lock);
		return;
	}
	lock_rw_wrlock(&z->lock);
	lock_quick_unlock(&zones->lock);

	/* find the domain */
	d = lz_find_node(z, name, len, labs);
	if(d) {
		/* no memory recycling for zone deletions ... */
		d->rrsets = NULL;
		/* did we delete the soa record ? */
		if(query_dname_compare(d->name, z->name) == 0)
			z->soa = NULL;

		/* cleanup the empty nonterminals for this name */
		del_empty_term(z, d, name, len, labs);
	}

	lock_rw_unlock(&z->lock);
}
