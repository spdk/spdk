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

#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/conf.h"
#include "spdk/net.h"

#include "spdk_internal/log.h"

#include "iscsi/iscsi.h"
#include "iscsi/conn.h"
#include "iscsi/tgt_node.h"
#include "iscsi/portal_grp.h"
#include "iscsi/init_grp.h"
#include "spdk/scsi.h"
#include "iscsi/task.h"

#define MAX_TMPBUF 1024
#define MAX_MASKBUF 128

static int
spdk_iscsi_is_addr_allowed_by_ipv6_netmask(const char *netmask, const char *addr)
{
	struct in6_addr in6_mask;
	struct in6_addr in6_addr;
	char mask[MAX_MASKBUF];
	const char *p;
	size_t n;
	int bits, bmask;
	int i;

	if (netmask[0] != '[')
		return 0;
	p = strchr(netmask, ']');
	if (p == NULL)
		return 0;
	n = p - (netmask + 1);
	if (n + 1 > sizeof mask)
		return 0;

	memcpy(mask, netmask + 1, n);
	mask[n] = '\0';
	p++;

	if (p[0] == '/') {
		bits = (int) strtol(p + 1, NULL, 10);
		if (bits < 0 || bits > 128)
			return 0;
	} else {
		bits = 128;
	}

#if 0
	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "input %s\n", addr);
	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "mask  %s / %d\n", mask, bits);
#endif

	/* presentation to network order binary */
	if (inet_pton(AF_INET6, mask, &in6_mask) <= 0
	    || inet_pton(AF_INET6, addr, &in6_addr) <= 0) {
		return 0;
	}

	/* check 128bits */
	for (i = 0; i < (bits / 8); i++) {
		if (in6_mask.s6_addr[i] != in6_addr.s6_addr[i])
			return 0;
	}
	if (bits % 8) {
		bmask = (0xffU << (8 - (bits % 8))) & 0xffU;
		if ((in6_mask.s6_addr[i] & bmask) != (in6_addr.s6_addr[i] & bmask))
			return 0;
	}

	/* match */
	return 1;
}

static int
spdk_iscsi_is_addr_allowed_by_ipv4_netmask(const char *netmask, const char *addr)
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
	if (n + 1 > sizeof mask)
		return 0;

	memcpy(mask, netmask, n);
	mask[n] = '\0';

	if (p[0] == '/') {
		bits = (int) strtol(p + 1, NULL, 10);
		if (bits < 0 || bits > 32)
			return 0;
	} else {
		bits = 32;
	}

	/* presentation to network order binary */
	if (inet_pton(AF_INET, mask, &in4_mask) <= 0
	    || inet_pton(AF_INET, addr, &in4_addr) <= 0) {
		return 0;
	}

	/* check 32bits */
	bmask = (0xffffffffULL << (32 - bits)) & 0xffffffffU;
	if ((ntohl(in4_mask.s_addr) & bmask) != (ntohl(in4_addr.s_addr) & bmask))
		return 0;

	/* match */
	return 1;
}

static int
spdk_iscsi_is_addr_allowed_by_netmask(const char *netmask, const char *addr)
{
	if (netmask == NULL || addr == NULL)
		return 0;
	if (strcasecmp(netmask, "ALL") == 0)
		return 1;
	if (netmask[0] == '[') {
		/* IPv6 */
		if (spdk_iscsi_is_addr_allowed_by_ipv6_netmask(netmask, addr))
			return 1;
	} else {
		/* IPv4 */
		if (spdk_iscsi_is_addr_allowed_by_ipv4_netmask(netmask, addr))
			return 1;
	}
	return 0;
}

static int
spdk_iscsi_is_addr_allowed_by_init_grp(struct spdk_iscsi_init_grp *ig,
				       const char *addr)
{
	struct spdk_iscsi_initiator_netmask *imask;
	int rc;

	if (ig->nnetmasks == 0) {
		/* OK, empty netmask as ALL */
		return 1;
	}

	SLIST_FOREACH(imask, &ig->netmask_head, slist) {
		SPDK_DEBUGLOG(SPDK_TRACE_DEBUG, "netmasks=%s, addr=%s\n",
			      imask->mask, addr);
		rc = spdk_iscsi_is_addr_allowed_by_netmask(imask->mask, addr);
		if (rc > 0) {
			/* OK netmask */
			return 1;
		}
	}
	/* NG netmask in this group */
	return 0;
}

static int
spdk_iscsi_is_iqn_allowed_by_iname(const char *name, const char *iqn)
{
	/* deny initiators */
	if (name[0] == '!'
	    && (strcasecmp(&name[1], "ALL") == 0
		|| strcasecmp(&name[1], iqn) == 0)) {
		/* NG */
		return 0;
	}

	if (strcasecmp(name, "ALL") == 0
	    || strcasecmp(name, iqn) == 0) {
		/* OK iqn, no check addr */
		return 1;
	}

	/* Next initiator name */
	return -1;
}

static int
spdk_iscsi_is_iqn_allowed_by_init_grp(struct spdk_iscsi_init_grp *ig,
				      const char *iqn)
{
	struct spdk_iscsi_initiator_name *iname;
	int rc;

	SLIST_FOREACH(iname, &ig->initiator_head, slist) {
		rc = spdk_iscsi_is_iqn_allowed_by_iname(iname->name, iqn);
		if (rc == 0 || rc == 1) {
			return rc;
		}
	}

	/* Next initiator group */
	return -1;
}

static struct spdk_iscsi_pg_map *
spdk_iscsi_tgt_node_find_pg_map(struct spdk_iscsi_tgt_node *target,
				struct spdk_iscsi_portal_grp *pg)
{
	struct spdk_iscsi_pg_map *pg_map;

	SLIST_FOREACH(pg_map, &target->pg_head, slist) {
		if (pg_map->pg == pg) {
			return pg_map;
		}
	}

	return NULL;
}

int
spdk_iscsi_tgt_node_access(struct spdk_iscsi_conn *conn,
			   struct spdk_iscsi_tgt_node *target,
			   const char *iqn, const char *addr)
{
	struct spdk_iscsi_portal_grp *pg;
	struct spdk_iscsi_pg_map *pg_map;
	struct spdk_iscsi_ig_map *ig_map;
	int rc;

	if (conn == NULL || target == NULL || iqn == NULL || addr == NULL)
		return 0;
	pg = conn->portal->group;

	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "pg=%d, iqn=%s, addr=%s\n",
		      pg->tag, iqn, addr);

	pg_map = spdk_iscsi_tgt_node_find_pg_map(target, pg);
	if (pg_map == NULL) {
		goto denied;
	}

	/* iqn is initiator group? */
	SLIST_FOREACH(ig_map, &pg_map->ig_head, slist) {
		rc = spdk_iscsi_is_iqn_allowed_by_init_grp(ig_map->ig, iqn);
		if (rc == 0) {
			goto denied;
		} else if (rc == 1) {
			/* OK iqn, check netmask */
			rc = spdk_iscsi_is_addr_allowed_by_init_grp(ig_map->ig, addr);
			if (rc == 1) {
				return 1;
			}
		}
	}

denied:
	/* NG */
	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "access denied from %s (%s) to %s (%s:%s,%d)\n",
		      iqn, addr, target->name, conn->portal->host,
		      conn->portal->port, conn->portal->group->tag);
	return 0;
}

static struct spdk_iscsi_pg_map *
spdk_iscsi_tgt_node_find_pg_map_by_tag(struct spdk_iscsi_tgt_node *target,
				       int pg_tag)
{
        struct spdk_iscsi_pg_map *pg_map;

        SLIST_FOREACH(pg_map, &target->pg_head, slist) {
                if (pg_map->pg->tag == pg_tag) {
                        return pg_map;
                }
        }

        return NULL;
}

static int
spdk_iscsi_is_iqn_visible_in_tgt_node(struct spdk_iscsi_tgt_node *target,
				      const char *iqn, int pg_tag)
{
	struct spdk_iscsi_pg_map *pg_map;
	struct spdk_iscsi_ig_map *ig_map;
	int rc;

	if (target == NULL || iqn == NULL)
		return 0;
	/* pg_tag exist map? */
	pg_map = spdk_iscsi_tgt_node_find_pg_map_by_tag(target, pg_tag);
	if (pg_map == NULL) {
		/* cat't access from pg_tag */
		return 0;
	}
	SLIST_FOREACH(pg_map, &target->pg_head, slist) {
		SLIST_FOREACH(ig_map, &pg_map->ig_head, slist) {
			rc = spdk_iscsi_is_iqn_allowed_by_init_grp(ig_map->ig, iqn);
			if (rc == 0 || rc == 1) {
				return rc;
			}
		}
	}

	return 0;
}

static int
spdk_iscsi_is_iqn_visible_in_tgt_portal_grp(struct spdk_iscsi_tgt_node *target,
					    const char *iqn, int pg_tag)
{
	struct spdk_iscsi_pg_map *pg_map;
	struct spdk_iscsi_ig_map *ig_map;
	int rc;

	if (target == NULL || iqn == NULL)
		return 0;
	pg_map = spdk_iscsi_tgt_node_find_pg_map_by_tag(target, pg_tag);
	if (pg_map == NULL) {
		/* cant't find pg_tag */
		return 0;
	}

	/* iqn is initiator group? */
	SLIST_FOREACH(ig_map, &pg_map->ig_head, slist) {
		SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "iqn=%s, pg=%d, ig=%d\n",
			      iqn, pg_tag, ig_map->ig->tag);
		rc = spdk_iscsi_is_iqn_allowed_by_init_grp(ig_map->ig, iqn);
		if (rc == 0 || rc == 1) {
			return rc;
		}
	}

	return 0;
}

int
spdk_iscsi_send_tgts(struct spdk_iscsi_conn *conn, const char *iiqn,
		     const char *iaddr, const char *tiqn, uint8_t *data, int alloc_len,
		     int data_len)
{
	char buf[MAX_TMPBUF];
	struct spdk_iscsi_portal_grp	*pg;
	struct spdk_iscsi_pg_map	*pg_map;
	struct spdk_iscsi_portal		*p;
	struct spdk_iscsi_tgt_node	*target;
	char *host;
	int total;
	int len;
	int rc;
	int i;

	if (conn == NULL)
		return 0;

	total = data_len;
	if (alloc_len < 1) {
		return 0;
	}
	if (total > alloc_len) {
		total = alloc_len;
		data[total - 1] = '\0';
		return total;
	}

	if (alloc_len - total < 1) {
		SPDK_ERRLOG("data space small %d\n", alloc_len);
		return total;
	}

	pthread_mutex_lock(&g_spdk_iscsi.mutex);
	for (i = 0; i < MAX_ISCSI_TARGET_NODE; i++) {
		target = g_spdk_iscsi.target[i];
		if (target == NULL)
			continue;
		if (strcasecmp(tiqn, "ALL") != 0
		    && strcasecmp(tiqn, target->name) != 0) {
			continue;
		}
		rc = spdk_iscsi_is_iqn_visible_in_tgt_node(target, iiqn,
							   conn->pg_tag);
		if (rc == 0) {
			continue;
		}

		/* DO SENDTARGETS */
		len = snprintf((char *) data + total, alloc_len - total,
			       "TargetName=%s", target->name);
		total += len + 1;

		SLIST_FOREACH(pg_map, &target->pg_head, slist) {
			pg = pg_map->pg;
			rc = spdk_iscsi_is_iqn_visible_in_tgt_portal_grp(target,
									 iiqn, pg->tag);
			if (rc == 0) {
				SPDK_DEBUGLOG(SPDK_TRACE_ISCSI,
					      "SKIP pg=%d, iqn=%s for %s from %s (%s)\n",
					      pg->tag, tiqn, target->name, iiqn, iaddr);
				goto skip_pg_tag;
			}

			/* write to data */
			TAILQ_FOREACH(p, &pg->head, tailq) {
				if (alloc_len - total < 1) {
					pthread_mutex_unlock(&g_spdk_iscsi.mutex);
					SPDK_ERRLOG("data space small %d\n", alloc_len);
					return total;
				}
				host = p->host;
				/* wildcard? */
				if (strcasecmp(host, "[::]") == 0
				    || strcasecmp(host, "[*]") == 0
				    || strcasecmp(host, "0.0.0.0") == 0
				    || strcasecmp(host, "*") == 0) {
					if ((strcasecmp(host, "[::]") == 0
					     || strcasecmp(host, "[*]") == 0)
					    && spdk_sock_is_ipv6(conn->sock)) {
						snprintf(buf, sizeof buf, "[%s]",
							 conn->target_addr);
						host = buf;
					} else if ((strcasecmp(host, "0.0.0.0") == 0
						    || strcasecmp(host, "*") == 0)
						   && spdk_sock_is_ipv4(conn->sock)) {
						snprintf(buf, sizeof buf, "%s",
							 conn->target_addr);
						host = buf;
					} else {
						/* skip portal for the family */
						continue;
					}
				}
				SPDK_DEBUGLOG(SPDK_TRACE_ISCSI,
					      "TargetAddress=%s:%s,%d\n",
					      host, p->port, pg->tag);
				len = snprintf((char *) data + total,
					       alloc_len - total,
					       "TargetAddress=%s:%s,%d",
					       host, p->port, pg->tag);
				total += len + 1;
			}
skip_pg_tag:
			;
		}
	}
	pthread_mutex_unlock(&g_spdk_iscsi.mutex);

	return total;
}

struct spdk_iscsi_tgt_node *
spdk_iscsi_find_tgt_node(const char *target_name)
{
	struct spdk_iscsi_tgt_node *target;
	int i;

	if (target_name == NULL)
		return NULL;
	for (i = 0; i < MAX_ISCSI_TARGET_NODE; i++) {
		target = g_spdk_iscsi.target[i];
		if (target == NULL)
			continue;
		if (strcasecmp(target_name, target->name) == 0) {
			return target;
		}
	}
	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "can't find target %s\n", target_name);
	return NULL;
}

static void
spdk_iscsi_tgt_node_init_map_pool(struct spdk_iscsi_tgt_node *target)
{
	int i;

	SLIST_INIT(&target->free_pg_head);
	for (i = 0; i < SPDK_SCSI_DEV_MAX_PORTS; i++) {
		SLIST_INSERT_HEAD(&target->free_pg_head, &target->pg_maps[i],
				  slist);
	}
	SLIST_INIT(&target->free_ig_head);
	for (i = 0; i < MAX_TARGET_MAP; i++) {
		SLIST_INSERT_HEAD(&target->free_ig_head, &target->ig_maps[i],
				  slist);
	}
}

static struct spdk_iscsi_pg_map *
spdk_iscsi_tgt_node_alloc_pg_map(struct spdk_iscsi_tgt_node *target)
{
	struct spdk_iscsi_pg_map *pg_map = NULL;

	if (!SLIST_EMPTY(&target->free_pg_head)) {
		pg_map = SLIST_FIRST(&target->free_pg_head);
		SLIST_REMOVE_HEAD(&target->free_pg_head, slist);
	}

	return pg_map;
}

static void
spdk_iscsi_tgt_node_free_pg_map(struct spdk_iscsi_tgt_node *target,
				struct spdk_iscsi_pg_map *pg_map)
{
	SLIST_INSERT_HEAD(&target->free_pg_head, pg_map, slist);
}

static struct spdk_iscsi_ig_map *
spdk_iscsi_tgt_node_alloc_ig_map(struct spdk_iscsi_tgt_node *target)
{
	struct spdk_iscsi_ig_map *ig_map = NULL;

	if (!SLIST_EMPTY(&target->free_ig_head)) {
		ig_map = SLIST_FIRST(&target->free_ig_head);
		SLIST_REMOVE_HEAD(&target->free_ig_head, slist);
	}

	return ig_map;
}

static void
spdk_iscsi_tgt_node_free_ig_map(struct spdk_iscsi_tgt_node *target,
				struct spdk_iscsi_ig_map *ig_map)
{
	SLIST_INSERT_HEAD(&target->free_ig_head, ig_map, slist);
}

static struct spdk_iscsi_ig_map *
spdk_iscsi_pg_map_find_ig_map(struct spdk_iscsi_pg_map *pg_map,
			      struct spdk_iscsi_init_grp *ig)
{
	struct spdk_iscsi_ig_map *ig_map;

	SLIST_FOREACH(ig_map, &pg_map->ig_head, slist) {
		if (ig_map->ig == ig) {
			return ig_map;
		}
	}
	return NULL;
}

static struct spdk_iscsi_ig_map *
spdk_iscsi_pg_map_add_ig_map(struct spdk_iscsi_tgt_node *target,
			     struct spdk_iscsi_pg_map *pg_map,
			     struct spdk_iscsi_init_grp *ig)
{
	struct spdk_iscsi_ig_map *ig_map;

	ig_map = spdk_iscsi_pg_map_find_ig_map(pg_map, ig);
	if (ig_map != NULL) {
		return NULL;
	}

	ig_map = spdk_iscsi_tgt_node_alloc_ig_map(target);
	if (ig_map == NULL) {
		return NULL;
	}

	ig_map->ig = ig;
	ig->ref++;

	SLIST_INSERT_HEAD(&pg_map->ig_head, ig_map, slist);
	pg_map->num_ig_maps++;

	return ig_map;
}

static int
spdk_iscsi_pg_map_delete_ig_map(struct spdk_iscsi_tgt_node *target,
				struct spdk_iscsi_pg_map *pg_map,
				struct spdk_iscsi_ig_map *ig_map)
{
	SLIST_REMOVE(&pg_map->ig_head, ig_map, spdk_iscsi_ig_map, slist);
	pg_map->num_ig_maps--;

	ig_map->ig->ref--;
	spdk_iscsi_tgt_node_free_ig_map(target, ig_map);

	return 0;
}

static void
spdk_iscsi_pg_map_delete_all_ig_maps(struct spdk_iscsi_tgt_node *target,
				     struct spdk_iscsi_pg_map *pg_map)
{
	struct spdk_iscsi_ig_map *ig_map;

	while (!SLIST_EMPTY(&pg_map->ig_head)) {
		ig_map = SLIST_FIRST(&pg_map->ig_head);
		SLIST_REMOVE_HEAD(&pg_map->ig_head, slist);
		pg_map->num_ig_maps--;

		ig_map->ig->ref--;
		spdk_iscsi_tgt_node_free_ig_map(target, ig_map);
	}
}

static struct spdk_iscsi_pg_map *
spdk_iscsi_tgt_node_add_pg_map(struct spdk_iscsi_tgt_node *target,
			       struct spdk_iscsi_portal_grp *pg)
{
	struct spdk_iscsi_pg_map *pg_map;
	char port_name[MAX_TMPBUF];
	int rc;

	pg_map = spdk_iscsi_tgt_node_find_pg_map(target, pg);
	if (pg_map != NULL) {
		return NULL;
	}

	pg_map = spdk_iscsi_tgt_node_alloc_pg_map(target);
	if (pg_map == NULL) {
		return NULL;
	}

	rc = spdk_scsi_dev_add_port(target->dev, pg->tag, port_name);
	if (rc != 0) {
		spdk_iscsi_tgt_node_free_pg_map(target, pg_map);
		return NULL;
	}

	snprintf(port_name, sizeof(port_name), "%s,t,0x%4.4x",
		 target->name, pg->tag);

	SLIST_INIT(&pg_map->ig_head);
	pg_map->num_ig_maps = 0;

	pg_map->pg = pg;
	pg->ref++;
	SLIST_INSERT_HEAD(&target->pg_head, pg_map, slist);
	target->num_pg_maps++;

	return pg_map;
}

static int
spdk_iscsi_tgt_node_delete_pg_map(struct spdk_iscsi_tgt_node *target,
				  struct spdk_iscsi_pg_map *pg_map)
{
	if (!SLIST_EMPTY(&pg_map->ig_head)) {
		return -1;
	}

	spdk_scsi_dev_delete_port(target->dev, pg_map->pg->tag);

	SLIST_REMOVE(&target->pg_head, pg_map, spdk_iscsi_pg_map, slist);
	target->num_pg_maps--;
	pg_map->pg->ref--;

	spdk_iscsi_tgt_node_free_pg_map(target, pg_map);

	return 0;
}

static void
spdk_iscsi_tgt_node_delete_all_pg_maps(struct spdk_iscsi_tgt_node *target)
{
	struct spdk_iscsi_pg_map *pg_map;

	while (!SLIST_EMPTY(&target->pg_head)) {
		pg_map = SLIST_FIRST(&target->pg_head);

		SLIST_REMOVE_HEAD(&target->pg_head, slist);
		target->num_pg_maps--;
		pg_map->pg->ref--;

		spdk_iscsi_pg_map_delete_all_ig_maps(target, pg_map);
		spdk_scsi_dev_delete_port(target->dev, pg_map->pg->tag);

		spdk_iscsi_tgt_node_free_pg_map(target, pg_map);
	}
}

static int
spdk_check_iscsi_name(const char *name)
{
	const unsigned char *up = (const unsigned char *) name;
	size_t n;

	/* valid iSCSI name? */
	for (n = 0; up[n] != 0; n++) {
		if (up[n] > 0x00U && up[n] <= 0x2cU)
			return -1;
		if (up[n] == 0x2fU)
			return -1;
		if (up[n] >= 0x3bU && up[n] <= 0x40U)
			return -1;
		if (up[n] >= 0x5bU && up[n] <= 0x60U)
			return -1;
		if (up[n] >= 0x7bU && up[n] <= 0x7fU)
			return -1;
		if (isspace(up[n]))
			return -1;
	}
	/* valid format? */
	if (strncasecmp(name, "iqn.", 4) == 0) {
		/* iqn.YYYY-MM.reversed.domain.name */
		if (!isdigit(up[4]) || !isdigit(up[5]) || !isdigit(up[6])
		    || !isdigit(up[7]) || up[8] != '-' || !isdigit(up[9])
		    || !isdigit(up[10]) || up[11] != '.') {
			SPDK_ERRLOG("invalid iqn format. "
				    "expect \"iqn.YYYY-MM.reversed.domain.name\"\n");
			return -1;
		}
	} else if (strncasecmp(name, "eui.", 4) == 0) {
		/* EUI-64 -> 16bytes */
		/* XXX */
	} else if (strncasecmp(name, "naa.", 4) == 0) {
		/* 64bit -> 16bytes, 128bit -> 32bytes */
		/* XXX */
	}
	/* OK */
	return 0;
}

static void
spdk_iscsi_tgt_node_destruct(struct spdk_iscsi_tgt_node *target)
{
	if (target == NULL) {
		return;
	}

	free(target->name);
	free(target->alias);
	spdk_iscsi_tgt_node_delete_all_pg_maps(target);
	spdk_scsi_dev_destruct(target->dev);

	pthread_mutex_destroy(&target->mutex);
	free(target);
}

static int spdk_get_next_available_tgt_number(void)
{
	int i;

	for (i = 0; i < MAX_ISCSI_TARGET_NODE; i++) {
		if (g_spdk_iscsi.target[i] == NULL)
			break;
	}
	return i; //Returns MAX_TARGET if none available.
}

static int
spdk_iscsi_tgt_node_delete_map(struct spdk_iscsi_tgt_node *target,
			       int32_t pg_tag, int32_t ig_tag)
{
	struct spdk_iscsi_portal_grp *pg;
	struct spdk_iscsi_init_grp *ig;
	struct spdk_iscsi_pg_map *pg_map;
	struct spdk_iscsi_ig_map *ig_map;

	pg = spdk_iscsi_portal_grp_find_by_tag(pg_tag);
	if (pg == NULL) {
		SPDK_ERRLOG("%s: PortalGroup%d not found\n", target->name, pg_tag);
		return -1;
	}
	ig = spdk_iscsi_init_grp_find_by_tag(ig_tag);
	if (ig == NULL) {
		SPDK_ERRLOG("%s: InitiatorGroup%d not found", target->name, ig_tag);
		return -1;
	}

	pg_map = spdk_iscsi_tgt_node_find_pg_map(target, pg);
	if (pg_map == NULL) {
		return -1;
	}
	ig_map = spdk_iscsi_pg_map_find_ig_map(pg_map, ig);
	if (ig_map == NULL) {
		return -1;
	}

	spdk_iscsi_pg_map_delete_ig_map(target, pg_map, ig_map);
	if (pg_map->num_ig_maps == 0) {
		spdk_iscsi_tgt_node_delete_pg_map(target, pg_map);
	}

	return 0;
}

static int
spdk_iscsi_tgt_node_add_map(struct spdk_iscsi_tgt_node *target,
			    int pg_tag, int ig_tag)
{
	struct spdk_iscsi_portal_grp		*pg;
	struct spdk_iscsi_init_grp	*ig;
	struct spdk_iscsi_pg_map	*pg_map;
	struct spdk_iscsi_ig_map	*ig_map;
	bool create_pg = false;

	pg = spdk_iscsi_portal_grp_find_by_tag(pg_tag);
	if (pg == NULL) {
		SPDK_ERRLOG("%s: PortalGroup%d not found\n", target->name, pg_tag);
		return -1;
	}
	if (pg->state != GROUP_READY) {
		SPDK_ERRLOG("%s: PortalGroup%d not active\n", target->name, pg_tag);
		return -1;
	}
	ig = spdk_iscsi_init_grp_find_by_tag(ig_tag);
	if (ig == NULL) {
		SPDK_ERRLOG("%s: InitiatorGroup%d not found\n", target->name, ig_tag);
		return -1;
	}
	if (ig->state != GROUP_READY) {
		SPDK_ERRLOG("%s: InitiatorGroup%d not active\n", target->name, ig_tag);
		return -1;
	}

	pg_map = spdk_iscsi_tgt_node_find_pg_map(target, pg);
	if (pg_map == NULL) {
		pg_map = spdk_iscsi_tgt_node_add_pg_map(target, pg);
		if (pg_map == NULL) {
			return -1;
		}
		create_pg = true;
	}

	ig_map = spdk_iscsi_pg_map_find_ig_map(pg_map, ig);
	if (ig_map == NULL) {
		ig_map = spdk_iscsi_pg_map_add_ig_map(target, pg_map, ig);
		if (ig_map != NULL) {
			return 0;
		}
	}

	if (create_pg) {
		spdk_iscsi_tgt_node_delete_pg_map(target, pg_map);
	}

	return -1;
}

static int
spdk_iscsi_tgt_node_add_maps(struct spdk_iscsi_tgt_node *target,
			     int *pg_tags, int *ig_tags, uint16_t num_maps)
{
	uint16_t i;
	int rc;

	pthread_mutex_lock(&g_spdk_iscsi.mutex);
	for (i = 0; i < num_maps; i++) {
		rc = spdk_iscsi_tgt_node_add_map(target, pg_tags[i], ig_tags[i]);
		if (rc != 0) {
			goto invalid;
		}
	}
	pthread_mutex_unlock(&g_spdk_iscsi.mutex);
	return 0;

invalid:
	for ( ; i > 0; --i) {
		spdk_iscsi_tgt_node_delete_map(target, pg_tags[i-1], ig_tags[i-1]);
	}
	pthread_mutex_unlock(&g_spdk_iscsi.mutex);
	return -1;
}

static int
spdk_iscsi_tgt_node_delete_maps(struct spdk_iscsi_tgt_node *target,
				int *pg_tags, int *ig_tags, uint16_t num_maps)
{
	uint16_t i;
	int rc;

	pthread_mutex_lock(&g_spdk_iscsi.mutex);
	for (i = 0; i < num_maps; i++) {
		rc = spdk_iscsi_tgt_node_delete_map(target, pg_tags[i], ig_tags[i]);
		if (rc != 0) {
			goto invalid;
		}
	}
	pthread_mutex_unlock(&g_spdk_iscsi.mutex);
	return 0;

invalid:
	for ( ; i > 0; --i) {
		spdk_iscsi_tgt_node_add_map(target, pg_tags[i-1], ig_tags[i-1]);
	}
	pthread_mutex_unlock(&g_spdk_iscsi.mutex);
	return -1;
}

_spdk_iscsi_tgt_node *
spdk_iscsi_tgt_node_construct(int target_index,
			      const char *name, const char *alias,
			      int *pg_tag_list, int *ig_tag_list, uint16_t num_maps,
			      char *lun_name_list[], int *lun_id_list, int num_luns,
			      int queue_depth,
			      int auth_chap_disabled, int auth_chap_required, int auth_chap_mutual, int auth_group,
			      int header_digest, int data_digest)
{
	char				fullname[MAX_TMPBUF];
	struct spdk_iscsi_tgt_node	*target;
	int				rc;

	if (auth_chap_disabled && auth_chap_required) {
		SPDK_ERRLOG("auth_chap_disabled and auth_chap_required are mutually exclusive\n");
		return NULL;
	}

	if ((num_maps > MAX_TARGET_MAP) || (num_maps == 0)) {
		SPDK_ERRLOG("num_maps = %d out of range\n", num_maps);
		return NULL;
	}

	if (target_index == -1)
		target_index = spdk_get_next_available_tgt_number();

	if (target_index >= MAX_ISCSI_TARGET_NODE) {
		SPDK_ERRLOG("%d over maximum unit number\n", target_index);
		return NULL;
	}

	if (g_spdk_iscsi.target[target_index] != NULL) {
		SPDK_ERRLOG("tgt_node%d: duplicate unit\n", target_index);
		return NULL;
	}

	if (name == NULL) {
		SPDK_ERRLOG("TargetName not found\n");
		return NULL;
	}

	if (strncasecmp(name, "iqn.", 4) != 0
	    && strncasecmp(name, "eui.", 4) != 0
	    && strncasecmp(name, "naa.", 4) != 0) {
		snprintf(fullname, sizeof(fullname), "%s:%s", g_spdk_iscsi.nodebase, name);
	} else
		snprintf(fullname, sizeof(fullname), "%s", name);

	if (spdk_check_iscsi_name(fullname) != 0) {
		SPDK_ERRLOG("TargetName %s contains an invalid character or format.\n",
			    name);
		return NULL;
	}

	target = malloc(sizeof(*target));
	if (!target) {
		SPDK_ERRLOG("could not allocate target\n");
		return NULL;
	}

	memset(target, 0, sizeof(*target));
	spdk_iscsi_tgt_node_init_map_pool(target);

	rc = pthread_mutex_init(&target->mutex, NULL);
	if (rc != 0) {
		SPDK_ERRLOG("tgt_node%d: mutex_init() failed\n", target->num);
		spdk_iscsi_tgt_node_destruct(target);
		return NULL;
	}

	target->num = target_index;

	target->name = strdup(fullname);
	if (!target->name) {
		SPDK_ERRLOG("Could not allocate TargetName\n");
		spdk_iscsi_tgt_node_destruct(target);
		return NULL;
	}

	if (alias == NULL) {
		target->alias = NULL;
	} else {
		target->alias = strdup(alias);
		if (!target->alias) {
			SPDK_ERRLOG("Could not allocate TargetAlias\n");
			spdk_iscsi_tgt_node_destruct(target);
			return NULL;
		}
	}

	target->dev = spdk_scsi_dev_construct(name, lun_name_list, lun_id_list, num_luns,
					      SPDK_SPC_PROTOCOL_IDENTIFIER_ISCSI, NULL, NULL);

	if (!target->dev) {
		SPDK_ERRLOG("Could not construct SCSI device\n");
		spdk_iscsi_tgt_node_destruct(target);
		return NULL;
	}

	rc = spdk_iscsi_tgt_node_add_maps(target, pg_tag_list, ig_tag_list,
					  num_maps);
	if (rc < 0) {
		SPDK_ERRLOG("could not add map to target\n");
		spdk_iscsi_tgt_node_destruct(target);
		return NULL;
	}

	target->auth_chap_disabled = auth_chap_disabled;
	target->auth_chap_required = auth_chap_required;
	target->auth_chap_mutual = auth_chap_mutual;
	target->auth_group = auth_group;
	target->header_digest = header_digest;
	target->data_digest = data_digest;

	if (queue_depth > SPDK_ISCSI_MAX_QUEUE_DEPTH) {
		SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "QueueDepth %d > Max %d.  Using %d instead.\n",
			      queue_depth, SPDK_ISCSI_MAX_QUEUE_DEPTH,
			      SPDK_ISCSI_MAX_QUEUE_DEPTH);
		queue_depth = SPDK_ISCSI_MAX_QUEUE_DEPTH;
	}
	target->queue_depth = queue_depth;

	pthread_mutex_lock(&g_spdk_iscsi.mutex);
	g_spdk_iscsi.ntargets++;
	g_spdk_iscsi.target[target->num] = target;
	pthread_mutex_unlock(&g_spdk_iscsi.mutex);

	return target;
}

static int
spdk_cf_add_iscsi_tgt_node(struct spdk_conf_section *sp)
{
	char buf[MAX_TMPBUF];
	struct spdk_iscsi_tgt_node *target;
	int pg_tag_list[MAX_TARGET_MAP], ig_tag_list[MAX_TARGET_MAP];
	int num_target_maps;
	const char *alias, *pg_tag, *ig_tag;
	const char *ag_tag;
	const char *val, *name;
	int target_num, auth_group, pg_tag_i, ig_tag_i;
	int header_digest, data_digest;
	int auth_chap_disabled, auth_chap_required, auth_chap_mutual;
	int i;
	int lun_id_list[SPDK_SCSI_DEV_MAX_LUN];
	char lun_name_array[SPDK_SCSI_DEV_MAX_LUN][SPDK_SCSI_LUN_MAX_NAME_LENGTH] = {};
	char *lun_name_list[SPDK_SCSI_DEV_MAX_LUN];
	int num_luns, queue_depth;

	target_num = spdk_conf_section_get_num(sp);

	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "add unit %d\n", target_num);

	data_digest = 0;
	header_digest = 0;

	name = spdk_conf_section_get_val(sp, "TargetName");

	if (name == NULL) {
		SPDK_ERRLOG("tgt_node%d: TargetName not found\n", target_num);
		return -1;
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
		if (val == NULL)
			break;
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

	/* Setup AuthMethod */
	val = spdk_conf_section_get_val(sp, "AuthMethod");
	auth_chap_disabled = 0;
	auth_chap_required = 0;
	auth_chap_mutual = 0;
	if (val != NULL) {
		for (i = 0; ; i++) {
			val = spdk_conf_section_get_nmval(sp, "AuthMethod", 0, i);
			if (val == NULL)
				break;
			if (strcasecmp(val, "CHAP") == 0) {
				auth_chap_required = 1;
			} else if (strcasecmp(val, "Mutual") == 0) {
				auth_chap_mutual = 1;
			} else if (strcasecmp(val, "Auto") == 0) {
				auth_chap_disabled = 0;
				auth_chap_required = 0;
				auth_chap_mutual = 0;
			} else if (strcasecmp(val, "None") == 0) {
				auth_chap_disabled = 1;
				auth_chap_required = 0;
				auth_chap_mutual = 0;
			} else {
				SPDK_ERRLOG("tgt_node%d: unknown auth\n", target_num);
				return -1;
			}
		}
		if (auth_chap_mutual && !auth_chap_required) {
			SPDK_ERRLOG("tgt_node%d: Mutual but not CHAP\n", target_num);
			return -1;
		}
	}
	if (auth_chap_disabled == 1) {
		SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "AuthMethod None\n");
	} else if (auth_chap_required == 0) {
		SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "AuthMethod Auto\n");
	} else {
		SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "AuthMethod CHAP %s\n",
			      auth_chap_mutual ? "Mutual" : "");
	}

	val = spdk_conf_section_get_val(sp, "AuthGroup");
	if (val == NULL) {
		auth_group = 0;
	} else {
		ag_tag = val;
		if (strcasecmp(ag_tag, "None") == 0) {
			auth_group = 0;
		} else {
			if (strncasecmp(ag_tag, "AuthGroup",
					strlen("AuthGroup")) != 0
			    || sscanf(ag_tag, "%*[^0-9]%d", &auth_group) != 1) {
				SPDK_ERRLOG("tgt_node%d: auth group error\n", target_num);
				return -1;
			}
			if (auth_group == 0) {
				SPDK_ERRLOG("tgt_node%d: invalid auth group 0\n", target_num);
				return -1;
			}
		}
	}
	if (auth_group == 0) {
		SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "AuthGroup None\n");
	} else {
		SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "AuthGroup AuthGroup%d\n",
			      auth_group);
	}

	val = spdk_conf_section_get_val(sp, "UseDigest");
	if (val != NULL) {
		for (i = 0; ; i++) {
			val = spdk_conf_section_get_nmval(sp, "UseDigest", 0, i);
			if (val == NULL)
				break;
			if (strcasecmp(val, "Header") == 0) {
				header_digest = 1;
			} else if (strcasecmp(val, "Data") == 0) {
				data_digest = 1;
			} else if (strcasecmp(val, "Auto") == 0) {
				header_digest = 0;
				data_digest = 0;
			} else {
				SPDK_ERRLOG("tgt_node%d: unknown digest\n", target_num);
				return -1;
			}
		}
	}
	if (header_digest == 0 && data_digest == 0) {
		SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "UseDigest Auto\n");
	} else {
		SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "UseDigest %s %s\n",
			      header_digest ? "Header" : "",
			      data_digest ? "Data" : "");
	}

	val = spdk_conf_section_get_val(sp, "QueueDepth");
	if (val == NULL) {
		queue_depth = SPDK_ISCSI_MAX_QUEUE_DEPTH;
	} else {
		queue_depth = (int) strtol(val, NULL, 10);
	}

	num_luns = 0;

	for (i = 0; i < SPDK_SCSI_DEV_MAX_LUN; i++) {
		snprintf(buf, sizeof(buf), "LUN%d", i);
		val = spdk_conf_section_get_val(sp, buf);
		if (val == NULL) {
			continue;
		}

		snprintf(lun_name_array[num_luns], SPDK_SCSI_LUN_MAX_NAME_LENGTH, "%s", val);
		lun_name_list[num_luns] = lun_name_array[num_luns];
		lun_id_list[num_luns] = i;
		num_luns++;
	}

	if (num_luns == 0) {
		SPDK_ERRLOG("tgt_node%d: No LUN specified for target %s.\n", target_num, name);
		return -1;
	}

	target = spdk_iscsi_tgt_node_construct(target_num, name, alias,
					       pg_tag_list, ig_tag_list, num_target_maps,
					       lun_name_list, lun_id_list, num_luns, queue_depth,
					       auth_chap_disabled, auth_chap_required,
					       auth_chap_mutual, auth_group,
					       header_digest, data_digest);

	if (target == NULL) {
		SPDK_ERRLOG("tgt_node%d: add_iscsi_target_node error\n", target_num);
		return -1;
	}

	spdk_scsi_dev_print(target->dev);
	return 0;
}

int spdk_iscsi_init_tgt_nodes(void)
{
	struct spdk_conf_section *sp;
	int rc;

	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI, "spdk_iscsi_init_tgt_nodes\n");

	sp = spdk_conf_first_section(NULL);
	while (sp != NULL) {
		if (spdk_conf_section_match_prefix(sp, "TargetNode")) {
			int tag = spdk_conf_section_get_num(sp);

			if (tag > SPDK_TN_TAG_MAX) {
				SPDK_ERRLOG("tag %d is invalid\n", tag);
				return -1;
			}
			rc = spdk_cf_add_iscsi_tgt_node(sp);
			if (rc < 0) {
				SPDK_ERRLOG("spdk_cf_add_iscsi_tgt_node() failed\n");
				return -1;
			}
		}
		sp = spdk_conf_next_section(sp);
	}
	return 0;
}

int
spdk_iscsi_shutdown_tgt_nodes(void)
{
	struct spdk_iscsi_tgt_node *target;
	int i;

	pthread_mutex_lock(&g_spdk_iscsi.mutex);
	for (i = 0; i < MAX_ISCSI_TARGET_NODE; i++) {
		target = g_spdk_iscsi.target[i];
		if (target == NULL)
			continue;
		spdk_iscsi_tgt_node_destruct(target);
		g_spdk_iscsi.ntargets--;
		g_spdk_iscsi.target[i] = NULL;
	}
	pthread_mutex_unlock(&g_spdk_iscsi.mutex);

	return 0;
}

int
spdk_iscsi_shutdown_tgt_node_by_name(const char *target_name)
{
	struct spdk_iscsi_tgt_node *target;
	int i = 0;
	int ret = -1;

	pthread_mutex_lock(&g_spdk_iscsi.mutex);
	for (i = 0; i < MAX_ISCSI_TARGET_NODE; i++) {
		target = g_spdk_iscsi.target[i];
		if (target == NULL)
			continue;

		if (strncmp(target_name, target->name, MAX_TMPBUF) == 0) {
			spdk_iscsi_tgt_node_destruct(target);
			g_spdk_iscsi.ntargets--;
			g_spdk_iscsi.target[i] = NULL;
			ret = 0;
			break;
		}
	}
	pthread_mutex_unlock(&g_spdk_iscsi.mutex);

	return ret;
}

int
spdk_iscsi_tgt_node_cleanup_luns(struct spdk_iscsi_conn *conn,
				 struct spdk_iscsi_tgt_node *target)
{
	int i;
	struct spdk_iscsi_task *task;

	for (i = 0; i < SPDK_SCSI_DEV_MAX_LUN; i++) {
		struct spdk_scsi_lun *lun = spdk_scsi_dev_get_lun(target->dev, i);

		if (!lun)
			continue;

		/* we create a fake management task per LUN to cleanup */
		task = spdk_iscsi_task_get(conn, NULL, spdk_iscsi_task_mgmt_cpl);
		if (!task) {
			SPDK_ERRLOG("Unable to acquire task\n");
			return -1;
		}

		task->scsi.target_port = conn->target_port;
		task->scsi.initiator_port = conn->initiator_port;
		task->scsi.lun = lun;

		spdk_scsi_dev_queue_mgmt_task(target->dev, &task->scsi, SPDK_SCSI_TASK_FUNC_LUN_RESET);
	}

	return 0;
}

void
spdk_iscsi_tgt_nodes_delete_pg_map(struct spdk_iscsi_portal_grp *pg)
{
	int i;
	struct spdk_iscsi_tgt_node *target;
	struct spdk_iscsi_pg_map *pg_map;

	for (i = 0; i < MAX_ISCSI_TARGET_NODE; i++) {
		target = g_spdk_iscsi.target[i];
		if (target == NULL)
			continue;

		pg_map = spdk_iscsi_tgt_node_find_pg_map(target, pg);
		if (pg_map == NULL)
			continue;

		spdk_iscsi_pg_map_delete_all_ig_maps(target, pg_map);
		spdk_iscsi_tgt_node_delete_pg_map(target, pg_map);
	}
}

void
spdk_iscsi_tgt_nodes_delete_ig_map(struct spdk_iscsi_init_grp *ig)
{
	int i;
	struct spdk_iscsi_tgt_node *target;
	struct spdk_iscsi_pg_map *pg_map;
	struct spdk_iscsi_ig_map *ig_map;

	for (i = 0; i < MAX_ISCSI_TARGET_NODE; i++) {
		target = g_spdk_iscsi.target[i];
		if (target == NULL)
			continue;
		
		SLIST_FOREACH(pg_map, &target->pg_head, slist) {
			ig_map = spdk_iscsi_pg_map_find_ig_map(pg_map, ig);
			if (ig_map == NULL)
				continue;
			
			spdk_iscsi_pg_map_delete_ig_map(target, pg_map, ig_map);
			if (pg_map->num_ig_maps == 0) {
				spdk_iscsi_tgt_node_delete_pg_map(target, pg_map);
			}
		}
	}
}
