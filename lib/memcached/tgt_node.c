/*-
 *   BSD LICENSE
 *
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk/stdinc.h"

#include "spdk/conf.h"
#include "spdk/sock.h"
#include "spdk/scsi.h"

#include "spdk_internal/log.h"

#include "spdk/hashtable.h"
#include "spdk/slab.h"

#include "memcached/memcached.h"
#include "memcached/conn.h"
#include "memcached/tgt_node.h"
#include "memcached/portal_grp.h"
#include "memcached/init_grp.h"

#define MAX_TMPBUF 1024
#define MAX_MASKBUF 128

#if 1 /* pg map operations */
static struct spdk_memcached_ig_map *
memcached_pg_map_find_ig_map(struct spdk_memcached_pg_map *pg_map,
			     struct spdk_memcached_init_grp *ig)
{
	struct spdk_memcached_ig_map *ig_map;

	TAILQ_FOREACH(ig_map, &pg_map->ig_map_head, tailq) {
		if (ig_map->ig == ig) {
			return ig_map;
		}
	}

	return NULL;
}

static struct spdk_memcached_ig_map *
memcached_pg_map_add_ig_map(struct spdk_memcached_pg_map *pg_map,
			    struct spdk_memcached_init_grp *ig)
{
	struct spdk_memcached_ig_map *ig_map;

	if (memcached_pg_map_find_ig_map(pg_map, ig) != NULL) {
		return NULL;
	}

	ig_map = malloc(sizeof(*ig_map));
	if (ig_map == NULL) {
		return NULL;
	}

	ig_map->ig = ig;
	ig->ref++;
	pg_map->num_ig_maps++;
	TAILQ_INSERT_TAIL(&pg_map->ig_map_head, ig_map, tailq);

	return ig_map;
}

static void
_memcached_pg_map_delete_ig_map(struct spdk_memcached_pg_map *pg_map,
				struct spdk_memcached_ig_map *ig_map)
{
	TAILQ_REMOVE(&pg_map->ig_map_head, ig_map, tailq);
	pg_map->num_ig_maps--;
	ig_map->ig->ref--;
	free(ig_map);
}

static int
memcached_pg_map_delete_ig_map(struct spdk_memcached_pg_map *pg_map,
			       struct spdk_memcached_init_grp *ig)
{
	struct spdk_memcached_ig_map *ig_map;

	ig_map = memcached_pg_map_find_ig_map(pg_map, ig);
	if (ig_map == NULL) {
		return -ENOENT;
	}

	_memcached_pg_map_delete_ig_map(pg_map, ig_map);
	return 0;
}

static void
memcached_pg_map_delete_all_ig_maps(struct spdk_memcached_pg_map *pg_map)
{
	struct spdk_memcached_ig_map *ig_map, *tmp;

	TAILQ_FOREACH_SAFE(ig_map, &pg_map->ig_map_head, tailq, tmp) {
		_memcached_pg_map_delete_ig_map(pg_map, ig_map);
	}
}

static struct spdk_memcached_pg_map *
memcached_tgt_node_find_pg_map(struct spdk_memcached_tgt_node *target,
			       struct spdk_memcached_portal_grp *pg)
{
	struct spdk_memcached_pg_map *pg_map;

	TAILQ_FOREACH(pg_map, &target->pg_map_head, tailq) {
		if (pg_map->pg == pg) {
			return pg_map;
		}
	}

	return NULL;
}

static struct spdk_memcached_pg_map *
memcached_tgt_node_add_pg_map(struct spdk_memcached_tgt_node *target,
			      struct spdk_memcached_portal_grp *pg)
{
	struct spdk_memcached_pg_map *pg_map;
	char port_name[MAX_TMPBUF];
	int rc;

	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED_TGT, "Try to set pg %d with target %p\n", pg->tag, target->name);
	if (memcached_tgt_node_find_pg_map(target, pg) != NULL) {
		return NULL;
	}

	if (spdk_memcached_portal_grp_is_target_set(pg)) {
		SPDK_ERRLOG("PG (tag %d) already set target\n",
			    pg->tag);
		return NULL;
	}

	pg_map = malloc(sizeof(*pg_map));
	if (pg_map == NULL) {
		return NULL;
	}

	(void)rc;
	(void)port_name;

	TAILQ_INIT(&pg_map->ig_map_head);
	pg_map->num_ig_maps = 0;
	pg->ref++;
	pg_map->pg = pg;
	target->num_pg_maps++;
	TAILQ_INSERT_TAIL(&target->pg_map_head, pg_map, tailq);
	rc = spdk_memcached_portal_grp_set_target(pg, target);
	assert(rc == 0);

	return pg_map;
}

static void
_memcached_tgt_node_delete_pg_map(struct spdk_memcached_tgt_node *target,
				  struct spdk_memcached_pg_map *pg_map)
{
	TAILQ_REMOVE(&target->pg_map_head, pg_map, tailq);
	target->num_pg_maps--;
	pg_map->pg->ref--;
	spdk_memcached_portal_grp_clear_target(pg_map->pg);

	free(pg_map);
}

static int
memcached_tgt_node_delete_pg_map(struct spdk_memcached_tgt_node *target,
				 struct spdk_memcached_portal_grp *pg)
{
	struct spdk_memcached_pg_map *pg_map;

	pg_map = memcached_tgt_node_find_pg_map(target, pg);
	if (pg_map == NULL) {
		return -ENOENT;
	}

	if (pg_map->num_ig_maps > 0) {
		SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "delete %d ig_maps forcefully\n",
			      pg_map->num_ig_maps);
	}

	memcached_pg_map_delete_all_ig_maps(pg_map);
	_memcached_tgt_node_delete_pg_map(target, pg_map);
	return 0;
}

static void
memcached_tgt_node_delete_ig_maps(struct spdk_memcached_tgt_node *target,
				  struct spdk_memcached_init_grp *ig)
{
	struct spdk_memcached_pg_map *pg_map, *tmp;

	TAILQ_FOREACH_SAFE(pg_map, &target->pg_map_head, tailq, tmp) {
		memcached_pg_map_delete_ig_map(pg_map, ig);
		if (pg_map->num_ig_maps == 0) {
			_memcached_tgt_node_delete_pg_map(target, pg_map);
		}
	}
}

static void
memcached_tgt_node_delete_all_pg_maps(struct spdk_memcached_tgt_node *target)
{
	struct spdk_memcached_pg_map *pg_map, *tmp;

	TAILQ_FOREACH_SAFE(pg_map, &target->pg_map_head, tailq, tmp) {
		memcached_pg_map_delete_all_ig_maps(pg_map);
		_memcached_tgt_node_delete_pg_map(target, pg_map);
	}
}

static int
memcached_tgt_node_delete_pg_ig_map(struct spdk_memcached_tgt_node *target,
				    int pg_tag, int ig_tag)
{
	struct spdk_memcached_portal_grp	*pg;
	struct spdk_memcached_init_grp	*ig;
	struct spdk_memcached_pg_map	*pg_map;
	struct spdk_memcached_ig_map	*ig_map;

	pg = spdk_memcached_portal_grp_find_by_tag(pg_tag);
	if (pg == NULL) {
		SPDK_ERRLOG("%s: PortalGroup%d not found\n", target->name, pg_tag);
		return -ENOENT;
	}
	ig = spdk_memcached_init_grp_find_by_tag(ig_tag);
	if (ig == NULL) {
		SPDK_ERRLOG("%s: InitiatorGroup%d not found\n", target->name, ig_tag);
		return -ENOENT;
	}

	pg_map = memcached_tgt_node_find_pg_map(target, pg);
	if (pg_map == NULL) {
		SPDK_ERRLOG("%s: PortalGroup%d is not mapped\n", target->name, pg_tag);
		return -ENOENT;
	}
	ig_map = memcached_pg_map_find_ig_map(pg_map, ig);
	if (ig_map == NULL) {
		SPDK_ERRLOG("%s: InitiatorGroup%d is not mapped\n", target->name, pg_tag);
		return -ENOENT;
	}

	_memcached_pg_map_delete_ig_map(pg_map, ig_map);
	if (pg_map->num_ig_maps == 0) {
		_memcached_tgt_node_delete_pg_map(target, pg_map);
	}

	return 0;
}

static int
memcached_tgt_node_add_pg_ig_map(struct spdk_memcached_tgt_node *target,
				 int pg_tag, int ig_tag)
{
	struct spdk_memcached_portal_grp	*pg;
	struct spdk_memcached_pg_map	*pg_map;
	struct spdk_memcached_init_grp	*ig;
	struct spdk_memcached_ig_map	*ig_map;
	bool				new_pg_map = false;

	pg = spdk_memcached_portal_grp_find_by_tag(pg_tag);
	if (pg == NULL) {
		SPDK_ERRLOG("%s: PortalGroup%d not found\n", target->name, pg_tag);
		return -ENOENT;
	}
	ig = spdk_memcached_init_grp_find_by_tag(ig_tag);
	if (ig == NULL) {
		SPDK_ERRLOG("%s: InitiatorGroup%d not found\n", target->name, ig_tag);
		return -ENOENT;
	}

	/* get existing pg_map or create new pg_map and add it to target */
	pg_map = memcached_tgt_node_find_pg_map(target, pg);
	if (pg_map == NULL) {
		pg_map = memcached_tgt_node_add_pg_map(target, pg);
		if (pg_map == NULL) {
			goto failed;
		}
		new_pg_map = true;
	}

	/* create new ig_map and add it to pg_map */
	ig_map = memcached_pg_map_add_ig_map(pg_map, ig);
	if (ig_map == NULL) {
		goto failed;
	}

	return 0;

failed:
	if (new_pg_map) {
		_memcached_tgt_node_delete_pg_map(target, pg_map);
	}

	return -1;
}

int
spdk_memcached_tgt_node_add_pg_ig_maps(struct spdk_memcached_tgt_node *target,
				       int *pg_tag_list, int *ig_tag_list, uint16_t num_maps)
{
	uint16_t i;
	int rc;

	pthread_mutex_lock(&g_spdk_memcached.mutex);
	for (i = 0; i < num_maps; i++) {
		rc = memcached_tgt_node_add_pg_ig_map(target, pg_tag_list[i],
						      ig_tag_list[i]);
		if (rc != 0) {
			SPDK_ERRLOG("could not add map to target\n");
			goto invalid;
		}
	}
	pthread_mutex_unlock(&g_spdk_memcached.mutex);
	return 0;

invalid:
	for (; i > 0; --i) {
		memcached_tgt_node_delete_pg_ig_map(target, pg_tag_list[i - 1],
						    ig_tag_list[i - 1]);
	}
	pthread_mutex_unlock(&g_spdk_memcached.mutex);
	return -1;
}

int
spdk_memcached_tgt_node_delete_pg_ig_maps(struct spdk_memcached_tgt_node *target,
		int *pg_tag_list, int *ig_tag_list, uint16_t num_maps)
{
	uint16_t i;
	int rc;

	pthread_mutex_lock(&g_spdk_memcached.mutex);
	for (i = 0; i < num_maps; i++) {
		rc = memcached_tgt_node_delete_pg_ig_map(target, pg_tag_list[i],
				ig_tag_list[i]);
		if (rc != 0) {
			SPDK_ERRLOG("could not delete map from target\n");
			goto invalid;
		}
	}
	pthread_mutex_unlock(&g_spdk_memcached.mutex);
	return 0;

invalid:
	for (; i > 0; --i) {
		rc = memcached_tgt_node_add_pg_ig_map(target, pg_tag_list[i - 1],
						      ig_tag_list[i - 1]);
		if (rc != 0) {
			memcached_tgt_node_delete_all_pg_maps(target);
			break;
		}
	}
	pthread_mutex_unlock(&g_spdk_memcached.mutex);
	return -1;
}

void spdk_memcached_tgt_node_delete_map(struct spdk_memcached_portal_grp *portal_group,
					struct spdk_memcached_init_grp *initiator_group)
{
	struct spdk_memcached_tgt_node *target;

	pthread_mutex_lock(&g_spdk_memcached.mutex);
	TAILQ_FOREACH(target, &g_spdk_memcached.target_head, tailq) {
		if (portal_group) {
			memcached_tgt_node_delete_pg_map(target, portal_group);
		}
		if (initiator_group) {
			memcached_tgt_node_delete_ig_maps(target, initiator_group);
		}
	}
	pthread_mutex_unlock(&g_spdk_memcached.mutex);
}
#endif /* pg map operations */


#if 1 /* spdk_memcached_tgt_node_access */
static bool
memcached_ipv6_netmask_allow_addr(const char *netmask, const char *addr)
{
	struct in6_addr in6_mask;
	struct in6_addr in6_addr;
	char mask[MAX_MASKBUF];
	const char *p;
	size_t n;
	int bits, bmask;
	int i;

	if (netmask[0] != '[') {
		return false;
	}
	p = strchr(netmask, ']');
	if (p == NULL) {
		return false;
	}
	n = p - (netmask + 1);
	if (n + 1 > sizeof mask) {
		return false;
	}

	memcpy(mask, netmask + 1, n);
	mask[n] = '\0';
	p++;

	if (p[0] == '/') {
		bits = (int) strtol(p + 1, NULL, 10);
		if (bits <= 0 || bits > 128) {
			return false;
		}
	} else {
		bits = 128;
	}

	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "input %s\n", addr);
	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "mask  %s / %d\n", mask, bits);

	/* presentation to network order binary */
	if (inet_pton(AF_INET6, mask, &in6_mask) <= 0
	    || inet_pton(AF_INET6, addr, &in6_addr) <= 0) {
		return false;
	}

	/* check 128bits */
	for (i = 0; i < (bits / 8); i++) {
		if (in6_mask.s6_addr[i] != in6_addr.s6_addr[i]) {
			return false;
		}
	}
	if (bits % 8) {
		bmask = (0xffU << (8 - (bits % 8))) & 0xffU;
		if ((in6_mask.s6_addr[i] & bmask) != (in6_addr.s6_addr[i] & bmask)) {
			return false;
		}
	}

	/* match */
	return true;
}

static bool
memcached_ipv4_netmask_allow_addr(const char *netmask, const char *addr)
{
	struct in_addr in4_mask;
	struct in_addr in4_addr;
	char mask[MAX_MASKBUF];
	const char *p;
	uint32_t bmask;
	size_t n;
	int bits;

	p = strchr(netmask, '/');
	if (p == NULL) {
		p = netmask + strlen(netmask);
	}
	n = p - netmask;
	if (n + 1 > sizeof mask) {
		return false;
	}

	memcpy(mask, netmask, n);
	mask[n] = '\0';

	if (p[0] == '/') {
		bits = (int) strtol(p + 1, NULL, 10);
		if (bits <= 0 || bits > 32) {
			return false;
		}
	} else {
		bits = 32;
	}

	/* presentation to network order binary */
	if (inet_pton(AF_INET, mask, &in4_mask) <= 0
	    || inet_pton(AF_INET, addr, &in4_addr) <= 0) {
		return false;
	}

	/* check 32bits */
	bmask = (0xffffffffU << (32 - bits)) & 0xffffffffU;
	if ((ntohl(in4_mask.s_addr) & bmask) != (ntohl(in4_addr.s_addr) & bmask)) {
		return false;
	}

	/* match */
	return true;
}

static bool
memcached_netmask_allow_addr(const char *netmask, const char *addr)
{
	if (netmask == NULL || addr == NULL) {
		return false;
	}
	if (strcasecmp(netmask, "ANY") == 0) {
		return true;
	}
	if (netmask[0] == '[') {
		/* IPv6 */
		if (memcached_ipv6_netmask_allow_addr(netmask, addr)) {
			return true;
		}
	} else {
		/* IPv4 */
		if (memcached_ipv4_netmask_allow_addr(netmask, addr)) {
			return true;
		}
	}
	return false;
}

static bool
memcached_init_grp_allow_addr(struct spdk_memcached_init_grp *igp,
			      const char *addr)
{
	struct spdk_memcached_initiator_netmask *imask;

	TAILQ_FOREACH(imask, &igp->netmask_head, tailq) {
		SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "netmask=%s, addr=%s\n",
			      imask->mask, addr);
		if (memcached_netmask_allow_addr(imask->mask, addr)) {
			return true;
		}
	}
	return false;
}

bool
spdk_memcached_tgt_node_access(struct spdk_memcached_conn *conn,
			       struct spdk_memcached_tgt_node *target, const char *addr)
{
	struct spdk_memcached_portal_grp *pg;
	struct spdk_memcached_pg_map *pg_map;
	struct spdk_memcached_ig_map *ig_map;

	if (conn == NULL || target == NULL || addr == NULL) {
		goto denied;
	}
	pg = conn->portal->group;

	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "pg=%d, addr=%s\n",
		      pg->tag, addr);
	pg_map = memcached_tgt_node_find_pg_map(target, pg);
	if (pg_map == NULL) {
		goto denied;
	}
	TAILQ_FOREACH(ig_map, &pg_map->ig_map_head, tailq) {
		if (memcached_init_grp_allow_addr(ig_map->ig, addr)) {
			return true;
		}
	}

denied:
	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "access denied from (%s) to %s (%s:%s,%d)\n",
		      addr, target->name, conn->portal->host,
		      conn->portal->port, conn->portal->group->tag);
	return false;
}
#endif /* spdk_memcached_tgt_node_access */



struct spdk_memcached_tgt_node *
spdk_memcached_find_tgt_node(const char *target_name)
{
	struct spdk_memcached_tgt_node *target;

	if (target_name == NULL) {
		return NULL;
	}
	TAILQ_FOREACH(target, &g_spdk_memcached.target_head, tailq) {
		if (strcasecmp(target_name, target->name) == 0) {
			return target;
		}
	}
	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "can't find target %s\n", target_name);
	return NULL;
}

struct spdk_memcached_tgt_node *
spdk_memcached_first_tgt_node(void)
{
	struct spdk_memcached_tgt_node *target;

	target = TAILQ_FIRST(&g_spdk_memcached.target_head);

	if (target == NULL) {
		assert(false);
	}
	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "find target %s\n", target->name);
	return target;
}

static int
memcached_tgt_node_register(struct spdk_memcached_tgt_node *target)
{
	pthread_mutex_lock(&g_spdk_memcached.mutex);

	if (spdk_memcached_find_tgt_node(target->name) != NULL) {
		pthread_mutex_unlock(&g_spdk_memcached.mutex);
		return -EEXIST;
	}

	TAILQ_INSERT_TAIL(&g_spdk_memcached.target_head, target, tailq);

	pthread_mutex_unlock(&g_spdk_memcached.mutex);
	return 0;
}

static int
memcached_tgt_node_unregister(struct spdk_memcached_tgt_node *target)
{
	struct spdk_memcached_tgt_node *t;

	TAILQ_FOREACH(t, &g_spdk_memcached.target_head, tailq) {
		if (t == target) {
			TAILQ_REMOVE(&g_spdk_memcached.target_head, t, tailq);
			return 0;
		}
	}

	return -1;
}

static void
memcached_tgt_node_destruct(struct spdk_memcached_tgt_node *target)
{
	if (target == NULL) {
		return;
	}

	free(target->name);
	free(target->alias);
	free((char *)target->bdev_name);
	memcached_tgt_node_delete_all_pg_maps(target);

//	spdk_slab_destroy();
//	spdk_hashtable_destroy();

	pthread_mutex_destroy(&target->mutex);
	free(target);
}

static void
tgt_node_construct_slab_create_cb(void *cb_arg, int slab_errno)
{
	assert(slab_errno == 0);
	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED_TGT, "slab is create for tgt node\n");
}

_spdk_memcached_tgt_node *
spdk_memcached_tgt_node_construct(const char *tgt_name, const char *tgt_alias, int target_index,
				  const char *bdev_name, const char *cpu_mask,
				  int *pg_tag_list, int *ig_tag_list, uint16_t num_maps, int queue_depth)
{
	char				fullname[MAX_TMPBUF];
	struct spdk_memcached_tgt_node	*target;
	int				rc;

	if (num_maps == 0) {
		SPDK_ERRLOG("num_maps = 0\n");
		return NULL;
	}

	if (tgt_name == NULL) {
		SPDK_ERRLOG("TargetName not found\n");
		return NULL;
	}


	snprintf(fullname, sizeof(fullname), "%s", tgt_name);

	target = malloc(sizeof(*target));
	if (!target) {
		SPDK_ERRLOG("could not allocate target\n");
		return NULL;
	}

	memset(target, 0, sizeof(*target));

	rc = pthread_mutex_init(&target->mutex, NULL);
	if (rc != 0) {
		SPDK_ERRLOG("tgt_node%d: mutex_init() failed\n", target->num);
		memcached_tgt_node_destruct(target);
		return NULL;
	}

	target->num = target_index;

	target->name = strdup(fullname);
	if (!target->name) {
		SPDK_ERRLOG("Could not allocate TargetName\n");
		memcached_tgt_node_destruct(target);
		return NULL;
	}

	if (tgt_alias == NULL) {
		target->alias = NULL;
	} else {
		target->alias = strdup(tgt_alias);
		if (!target->alias) {
			SPDK_ERRLOG("Could not allocate TargetAlias\n");
			memcached_tgt_node_destruct(target);
			return NULL;
		}
	}

	TAILQ_INIT(&target->pg_map_head);
	rc = spdk_memcached_tgt_node_add_pg_ig_maps(target, pg_tag_list, ig_tag_list, num_maps);
	if (rc != 0) {
		SPDK_ERRLOG("could not add map to target\n");
		memcached_tgt_node_destruct(target);
		return NULL;
	}


	if (queue_depth > 0 && ((uint32_t)queue_depth <= g_spdk_memcached.MaxQueueDepth)) {
		target->queue_depth = queue_depth;
	} else {
		SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "QueueDepth %d is invalid and %d is used instead.\n",
			      queue_depth, g_spdk_memcached.MaxQueueDepth);
		target->queue_depth = g_spdk_memcached.MaxQueueDepth;
	}

	rc = memcached_tgt_node_register(target);
	if (rc != 0) {
		SPDK_ERRLOG("register target is failed\n");
		memcached_tgt_node_destruct(target);
		return NULL;
	}

	target->bdev_name = strdup(bdev_name);
	target->core_mask = spdk_cpuset_alloc();
	rc = spdk_cpuset_parse(target->core_mask, cpu_mask);
	assert(rc == 0);

	rc = spdk_hashtable_create(target->core_mask);
	assert(rc == 0);

	rc = spdk_slab_mgr_create(target->bdev_name, target->core_mask, NULL,
				  tgt_node_construct_slab_create_cb, NULL);

	assert(rc == 0);

	return target;
}

static int
memcached_parse_tgt_node(struct spdk_conf_section *sp)
{
	struct spdk_memcached_tgt_node *target;
	int pg_tag_list[MAX_TARGET_MAP], ig_tag_list[MAX_TARGET_MAP];
	int num_target_maps;
	const char *alias, *pg_tag, *ig_tag;
	const char *val, *name;
	const char *bdev_name, *cpu_mask;
	int target_num, pg_tag_i, ig_tag_i;
	int i;
	int queue_depth;

	target_num = spdk_conf_section_get_num(sp);

	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "add unit %d\n", target_num);

	name = spdk_conf_section_get_val(sp, "TargetName");
	if (name == NULL) {
		SPDK_ERRLOG("tgt_node%d: TargetName not found\n", target_num);
		return -1;
	}

	bdev_name = spdk_conf_section_get_val(sp, "Bdev");
	if (bdev_name == NULL) {
		SPDK_ERRLOG("tgt_node%d: BDEV not found\n", target_num);
		return -1;
	}

	cpu_mask = spdk_conf_section_get_val(sp, "Cpumask");
	if (cpu_mask == NULL) {
		cpu_mask = "0xF";
	}

	alias = spdk_conf_section_get_val(sp, "TargetAlias");

	/* Setup initiator and portal group mapping */
	val = spdk_conf_section_get_val(sp, "Mapping");
	if (val == NULL) {
		/* no map */
		SPDK_ERRLOG("tgt_node%d: no Mapping\n", target_num);
		return -1;
	}

	for (i = 0; i < MAX_TARGET_MAP; i++) {
		val = spdk_conf_section_get_nmval(sp, "Mapping", i, 0);
		if (val == NULL) {
			break;
		}
		pg_tag = spdk_conf_section_get_nmval(sp, "Mapping", i, 0);
		ig_tag = spdk_conf_section_get_nmval(sp, "Mapping", i, 1);
		if (pg_tag == NULL || ig_tag == NULL) {
			SPDK_ERRLOG("tgt_node%d: mapping error\n", target_num);
			return -1;
		}
		if (strncasecmp(pg_tag, "PortalGroup",
				strlen("PortalGroup")) != 0
		    || sscanf(pg_tag, "%*[^0-9]%d", &pg_tag_i) != 1) {
			SPDK_ERRLOG("tgt_node%d: mapping portal error\n", target_num);
			return -1;
		}
		if (strncasecmp(ig_tag, "InitiatorGroup",
				strlen("InitiatorGroup")) != 0
		    || sscanf(ig_tag, "%*[^0-9]%d", &ig_tag_i) != 1) {
			SPDK_ERRLOG("tgt_node%d: mapping initiator error\n", target_num);
			return -1;
		}
		if (pg_tag_i < 1 || ig_tag_i < 1) {
			SPDK_ERRLOG("tgt_node%d: invalid group tag\n", target_num);
			return -1;
		}
		pg_tag_list[i] = pg_tag_i;
		ig_tag_list[i] = ig_tag_i;
	}

	num_target_maps = i;


	val = spdk_conf_section_get_val(sp, "QueueDepth");
	if (val == NULL) {
		queue_depth = g_spdk_memcached.MaxQueueDepth;
	} else {
		queue_depth = (int) strtol(val, NULL, 10);
	}

	target = spdk_memcached_tgt_node_construct(name, alias, target_num, bdev_name, cpu_mask,
			pg_tag_list, ig_tag_list, num_target_maps, queue_depth);


	if (target == NULL) {
		SPDK_ERRLOG("tgt_node%d: add_memcached_target_node error\n", target_num);
		return -1;
	}

	return 0;
}

int spdk_memcached_parse_tgt_nodes(void)
{
	struct spdk_conf_section *sp;
	int rc;

	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "spdk_memcached_parse_tgt_nodes\n");

	sp = spdk_conf_first_section(NULL);
	while (sp != NULL) {
		if (spdk_conf_section_match_prefix(sp, "TargetNode")) {
			int tag = spdk_conf_section_get_num(sp);

			if (tag > SPDK_TN_TAG_MAX) {
				SPDK_ERRLOG("tag %d is invalid\n", tag);
				return -1;
			}
			rc = memcached_parse_tgt_node(sp);
			if (rc < 0) {
				SPDK_ERRLOG("spdk_memcached_parse_tgt_node() failed\n");
				return -1;
			}
		}
		sp = spdk_conf_next_section(sp);
	}
	return 0;
}

void
spdk_memcached_shutdown_tgt_nodes(void)
{
	struct spdk_memcached_tgt_node *target, *tmp;

	pthread_mutex_lock(&g_spdk_memcached.mutex);
	TAILQ_FOREACH_SAFE(target, &g_spdk_memcached.target_head, tailq, tmp) {
		TAILQ_REMOVE(&g_spdk_memcached.target_head, target, tailq);
		memcached_tgt_node_destruct(target);
	}
	pthread_mutex_unlock(&g_spdk_memcached.mutex);
}

int
spdk_memcached_shutdown_tgt_node_by_name(const char *target_name)
{
	struct spdk_memcached_tgt_node *target;

	pthread_mutex_lock(&g_spdk_memcached.mutex);
	target = spdk_memcached_find_tgt_node(target_name);
	if (target != NULL) {
		memcached_tgt_node_unregister(target);
		memcached_tgt_node_destruct(target);
		pthread_mutex_unlock(&g_spdk_memcached.mutex);

		return 0;
	}
	pthread_mutex_unlock(&g_spdk_memcached.mutex);

	return -ENOENT;
}

SPDK_LOG_REGISTER_COMPONENT("memcached_tgt", SPDK_LOG_MEMCACHED_TGT)
