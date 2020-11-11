/*-
 *   BSD LICENSE
 *
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

#include "spdk/scsi.h"

#include "CUnit/Basic.h"
#include "spdk_internal/mock.h"

#include "../common.c"
#include "iscsi/tgt_node.c"
#include "scsi/scsi_internal.h"
#include "unit/lib/json_mock.c"
#include "common/lib/test_env.c"

struct spdk_iscsi_globals g_iscsi;

DEFINE_STUB(spdk_scsi_dev_get_id,
	    int,
	    (const struct spdk_scsi_dev *dev),
	    0);

DEFINE_STUB(spdk_scsi_lun_get_bdev_name,
	    const char *,
	    (const struct spdk_scsi_lun *lun),
	    NULL);

DEFINE_STUB(spdk_scsi_lun_get_id,
	    int,
	    (const struct spdk_scsi_lun *lun),
	    0);

DEFINE_STUB_V(spdk_iscsi_op_abort_task_set,
	      (struct spdk_iscsi_task *task,
	       uint8_t function));

DEFINE_STUB(spdk_sock_is_ipv6, bool, (struct spdk_sock *sock), false);

DEFINE_STUB(spdk_sock_is_ipv4, bool, (struct spdk_sock *sock), false);

DEFINE_STUB(iscsi_portal_grp_find_by_tag,
	    struct spdk_iscsi_portal_grp *, (int tag), NULL);

DEFINE_STUB(iscsi_init_grp_find_by_tag, struct spdk_iscsi_init_grp *,
	    (int tag), NULL);

DEFINE_STUB_V(iscsi_op_abort_task_set,
	      (struct spdk_iscsi_task *task, uint8_t function));

DEFINE_STUB(iscsi_parse_redirect_addr,
	    int,
	    (struct sockaddr_storage *sa, const char *host, const char *port),
	    0);

DEFINE_STUB(iscsi_portal_grp_find_portal_by_addr,
	    struct spdk_iscsi_portal *,
	    (struct spdk_iscsi_portal_grp *pg, const char *host, const char *port),
	    NULL);

struct spdk_scsi_lun *
spdk_scsi_dev_get_lun(struct spdk_scsi_dev *dev, int lun_id)
{
	if (lun_id < 0 || lun_id >= SPDK_SCSI_DEV_MAX_LUN) {
		return NULL;
	}

	return dev->lun[lun_id];
}

int
spdk_scsi_dev_add_lun(struct spdk_scsi_dev *dev, const char *bdev_name, int lun_id,
		      void (*hotremove_cb)(const struct spdk_scsi_lun *, void *),
		      void *hotremove_ctx)
{
	if (bdev_name == NULL) {
		return -1;
	} else {
		return 0;
	}
}

static void
add_lun_test_cases(void)
{
	struct spdk_iscsi_tgt_node tgtnode = {};
	int lun_id = 0;
	char *bdev_name = NULL;
	struct spdk_scsi_dev scsi_dev = {};
	int rc;

	/* case 1 */
	tgtnode.num_active_conns = 1;

	rc = iscsi_tgt_node_add_lun(&tgtnode, bdev_name, lun_id);
	CU_ASSERT(rc != 0);

	/* case 2 */
	tgtnode.num_active_conns = 0;
	lun_id = -2;

	rc = iscsi_tgt_node_add_lun(&tgtnode, bdev_name, lun_id);
	CU_ASSERT(rc != 0);

	/* case 3 */
	lun_id = SPDK_SCSI_DEV_MAX_LUN;

	rc = iscsi_tgt_node_add_lun(&tgtnode, bdev_name, lun_id);
	CU_ASSERT(rc != 0);

	/* case 4 */
	lun_id = -1;
	tgtnode.dev = NULL;

	rc = iscsi_tgt_node_add_lun(&tgtnode, bdev_name, lun_id);
	CU_ASSERT(rc != 0);

	/* case 5 */
	tgtnode.dev = &scsi_dev;

	rc = iscsi_tgt_node_add_lun(&tgtnode, bdev_name, lun_id);
	CU_ASSERT(rc != 0);

	/* case 6 */
	bdev_name = "LUN0";

	rc = iscsi_tgt_node_add_lun(&tgtnode, bdev_name, lun_id);
	CU_ASSERT(rc == 0);
}

static void
allow_any_allowed(void)
{
	bool result;
	char *netmask;
	char *addr1, *addr2;

	netmask = "ANY";
	addr1 = "2001:ad6:1234:5678:9abc::";
	addr2 = "192.168.2.1";

	result = iscsi_netmask_allow_addr(netmask, addr1);
	CU_ASSERT(result == true);

	result = iscsi_netmask_allow_addr(netmask, addr2);
	CU_ASSERT(result == true);
}

static void
allow_ipv6_allowed(void)
{
	bool result;
	char *netmask;
	char *addr;

	netmask = "[2001:ad6:1234::]/48";
	addr = "2001:ad6:1234:5678:9abc::";

	result = iscsi_ipv6_netmask_allow_addr(netmask, addr);
	CU_ASSERT(result == true);

	result = iscsi_netmask_allow_addr(netmask, addr);
	CU_ASSERT(result == true);

	/* Netmask prefix bits == 128 (all bits must match) */
	netmask = "[2001:ad6:1234:5678:9abc::1]/128";
	addr = "2001:ad6:1234:5678:9abc::1";
	result = iscsi_ipv6_netmask_allow_addr(netmask, addr);
	CU_ASSERT(result == true);
}

static void
allow_ipv6_denied(void)
{
	bool result;
	char *netmask;
	char *addr;

	netmask = "[2001:ad6:1234::]/56";
	addr = "2001:ad6:1234:5678:9abc::";

	result = iscsi_ipv6_netmask_allow_addr(netmask, addr);
	CU_ASSERT(result == false);

	result = iscsi_netmask_allow_addr(netmask, addr);
	CU_ASSERT(result == false);

	/* Netmask prefix bits == 128 (all bits must match) */
	netmask = "[2001:ad6:1234:5678:9abc::1]/128";
	addr = "2001:ad6:1234:5678:9abc::2";
	result = iscsi_ipv6_netmask_allow_addr(netmask, addr);
	CU_ASSERT(result == false);
}

static void
allow_ipv6_invalid(void)
{
	bool result;
	char *netmask;
	char *addr;

	/* Netmask prefix bits > 128 */
	netmask = "[2001:ad6:1234::]/129";
	addr = "2001:ad6:1234:5678:9abc::";
	result = iscsi_ipv6_netmask_allow_addr(netmask, addr);
	CU_ASSERT(result == false);

	/* Netmask prefix bits == 0 */
	netmask = "[2001:ad6:1234::]/0";
	addr = "2001:ad6:1234:5678:9abc::";
	result = iscsi_ipv6_netmask_allow_addr(netmask, addr);
	CU_ASSERT(result == false);

	/* Netmask prefix bits < 0 */
	netmask = "[2001:ad6:1234::]/-1";
	addr = "2001:ad6:1234:5678:9abc::";
	result = iscsi_ipv6_netmask_allow_addr(netmask, addr);
	CU_ASSERT(result == false);
}

static void
allow_ipv4_allowed(void)
{
	bool result;
	char *netmask;
	char *addr;

	netmask = "192.168.2.0/24";
	addr = "192.168.2.1";

	result = iscsi_ipv4_netmask_allow_addr(netmask, addr);
	CU_ASSERT(result == true);

	result = iscsi_netmask_allow_addr(netmask, addr);
	CU_ASSERT(result == true);

	/* Netmask prefix == 32 (all bits must match) */
	netmask = "192.168.2.1/32";
	addr = "192.168.2.1";
	result = iscsi_ipv4_netmask_allow_addr(netmask, addr);
	CU_ASSERT(result == true);
}

static void
allow_ipv4_denied(void)
{
	bool result;
	char *netmask;
	char *addr;

	netmask = "192.168.2.0";
	addr  = "192.168.2.1";

	result = iscsi_ipv4_netmask_allow_addr(netmask, addr);
	CU_ASSERT(result == false);

	result = iscsi_netmask_allow_addr(netmask, addr);
	CU_ASSERT(result == false);

	/* Netmask prefix == 32 (all bits must match) */
	netmask = "192.168.2.1/32";
	addr = "192.168.2.2";
	result = iscsi_ipv4_netmask_allow_addr(netmask, addr);
	CU_ASSERT(result == false);
}

static void
allow_ipv4_invalid(void)
{
	bool result;
	char *netmask;
	char *addr;

	/* Netmask prefix bits > 32 */
	netmask = "192.168.2.0/33";
	addr = "192.168.2.1";
	result = iscsi_ipv4_netmask_allow_addr(netmask, addr);
	CU_ASSERT(result == false);

	/* Netmask prefix bits == 0 */
	netmask = "192.168.2.0/0";
	addr = "192.168.2.1";
	result = iscsi_ipv4_netmask_allow_addr(netmask, addr);
	CU_ASSERT(result == false);

	/* Netmask prefix bits < 0 */
	netmask = "192.168.2.0/-1";
	addr = "192.168.2.1";
	result = iscsi_ipv4_netmask_allow_addr(netmask, addr);
	CU_ASSERT(result == false);
}

static void
node_access_allowed(void)
{
	struct spdk_iscsi_tgt_node tgtnode = {};
	struct spdk_iscsi_portal_grp pg = {};
	struct spdk_iscsi_init_grp ig = {};
	struct spdk_iscsi_conn conn = {};
	struct spdk_iscsi_portal portal = {};
	struct spdk_iscsi_initiator_name iname = {};
	struct spdk_iscsi_initiator_netmask imask = {};
	struct spdk_scsi_dev scsi_dev = {};
	struct spdk_iscsi_pg_map *pg_map;
	char *iqn, *addr;
	bool result;

	/* portal group initialization */
	pg.tag = 1;

	/* initiator group initialization */
	ig.tag = 1;

	ig.ninitiators = 1;
	snprintf(iname.name, sizeof(iname.name), "iqn.2017-10.spdk.io:0001");
	TAILQ_INIT(&ig.initiator_head);
	TAILQ_INSERT_TAIL(&ig.initiator_head, &iname, tailq);

	ig.nnetmasks = 1;
	snprintf(imask.mask, sizeof(imask.mask), "192.168.2.0/24");
	TAILQ_INIT(&ig.netmask_head);
	TAILQ_INSERT_TAIL(&ig.netmask_head, &imask, tailq);

	/* target initialization */
	snprintf(tgtnode.name, sizeof(tgtnode.name), "iqn.2017-10.spdk.io:0001");
	TAILQ_INIT(&tgtnode.pg_map_head);

	snprintf(scsi_dev.name, sizeof(scsi_dev.name), "iqn.2017-10.spdk.io:0001");
	tgtnode.dev = &scsi_dev;

	pg_map = iscsi_tgt_node_add_pg_map(&tgtnode, &pg);
	iscsi_pg_map_add_ig_map(pg_map, &ig);

	/* portal initialization */
	portal.group = &pg;
	snprintf(portal.host, sizeof(portal.host), "192.168.2.0");
	snprintf(portal.port, sizeof(portal.port), "3260");

	/* input for UT */
	conn.portal = &portal;

	iqn = "iqn.2017-10.spdk.io:0001";
	addr = "192.168.2.1";

	result = iscsi_tgt_node_access(&conn, &tgtnode, iqn, addr);
	CU_ASSERT(result == true);

	iscsi_pg_map_delete_ig_map(pg_map, &ig);
	iscsi_tgt_node_delete_pg_map(&tgtnode, &pg);
}

static void
node_access_denied_by_empty_netmask(void)
{
	struct spdk_iscsi_tgt_node tgtnode = {};
	struct spdk_iscsi_portal_grp pg = {};
	struct spdk_iscsi_init_grp ig = {};
	struct spdk_iscsi_conn conn = {};
	struct spdk_iscsi_portal portal = {};
	struct spdk_iscsi_initiator_name iname = {};
	struct spdk_scsi_dev scsi_dev = {};
	struct spdk_iscsi_pg_map *pg_map;
	char *iqn, *addr;
	bool result;

	/* portal group initialization */
	pg.tag = 1;

	/* initiator group initialization */
	ig.tag = 1;

	ig.ninitiators = 1;
	snprintf(iname.name, sizeof(iname.name), "iqn.2017-10.spdk.io:0001");
	TAILQ_INIT(&ig.initiator_head);
	TAILQ_INSERT_TAIL(&ig.initiator_head, &iname, tailq);

	ig.nnetmasks = 0;
	TAILQ_INIT(&ig.netmask_head);

	/* target initialization */
	snprintf(tgtnode.name, sizeof(tgtnode.name), "iqn.2017-10.spdk.io:0001");
	TAILQ_INIT(&tgtnode.pg_map_head);

	snprintf(scsi_dev.name, sizeof(scsi_dev.name), "iqn.2017-10.spdk.io:0001");
	tgtnode.dev = &scsi_dev;

	pg_map = iscsi_tgt_node_add_pg_map(&tgtnode, &pg);
	iscsi_pg_map_add_ig_map(pg_map, &ig);

	/* portal initialization */
	portal.group = &pg;
	snprintf(portal.host, sizeof(portal.host), "192.168.2.0");
	snprintf(portal.port, sizeof(portal.port), "3260");

	/* input for UT */
	conn.portal = &portal;

	iqn = "iqn.2017-10.spdk.io:0001";
	addr = "192.168.3.1";

	result = iscsi_tgt_node_access(&conn, &tgtnode, iqn, addr);
	CU_ASSERT(result == false);

	iscsi_pg_map_delete_ig_map(pg_map, &ig);
	iscsi_tgt_node_delete_pg_map(&tgtnode, &pg);
}

#define IQN1	"iqn.2017-11.spdk.io:0001"
#define NO_IQN1	"!iqn.2017-11.spdk.io:0001"
#define IQN2	"iqn.2017-11.spdk.io:0002"
#define IP1	"192.168.2.0"
#define	IP2	"192.168.2.1"

static void
node_access_multi_initiator_groups_cases(void)
{
	struct spdk_iscsi_tgt_node tgtnode = {};
	struct spdk_iscsi_conn conn = {};
	struct spdk_iscsi_portal_grp pg = {};
	struct spdk_iscsi_portal portal = {};
	struct spdk_iscsi_init_grp ig1 = {}, ig2 = {};
	struct spdk_iscsi_initiator_name iname1 = {}, iname2 = {};
	struct spdk_iscsi_initiator_netmask imask1 = {}, imask2 = {};
	struct spdk_scsi_dev scsi_dev = {};
	struct spdk_iscsi_pg_map *pg_map;
	char *iqn, *addr;
	bool result;

	/* target initialization */
	snprintf(tgtnode.name, sizeof(tgtnode.name), IQN1);
	TAILQ_INIT(&tgtnode.pg_map_head);

	snprintf(scsi_dev.name, sizeof(scsi_dev.name), IQN1);
	tgtnode.dev = &scsi_dev;

	/* initiator group initialization */
	ig1.tag = 1;
	TAILQ_INIT(&ig1.initiator_head);
	TAILQ_INIT(&ig1.netmask_head);

	ig1.ninitiators = 1;
	TAILQ_INSERT_TAIL(&ig1.initiator_head, &iname1, tailq);

	ig1.nnetmasks = 1;
	TAILQ_INSERT_TAIL(&ig1.netmask_head, &imask1, tailq);

	ig2.tag = 2;
	TAILQ_INIT(&ig2.initiator_head);
	TAILQ_INIT(&ig2.netmask_head);

	ig2.ninitiators = 1;
	TAILQ_INSERT_TAIL(&ig2.initiator_head, &iname2, tailq);

	ig2.nnetmasks = 1;
	TAILQ_INSERT_TAIL(&ig2.netmask_head, &imask2, tailq);

	/* portal group initialization */
	pg.tag = 1;

	pg_map = iscsi_tgt_node_add_pg_map(&tgtnode, &pg);
	iscsi_pg_map_add_ig_map(pg_map, &ig1);
	iscsi_pg_map_add_ig_map(pg_map, &ig2);

	/* portal initialization */
	portal.group = &pg;
	snprintf(portal.host, sizeof(portal.host), IP1);
	snprintf(portal.port, sizeof(portal.port), "3260");

	/* connection initialization */
	conn.portal = &portal;

	iqn = IQN1;
	addr = IP1;

	/*
	 * case 1:
	 * +-------------------------------------------+---------+
	 * | IG1                 | IG2                 |         |
	 * +-------------------------------------------+         |
	 * | name      | addr    | name      | addr    | result  |
	 * +-------------------------------------------+---------+
	 * +-------------------------------------------+---------+
	 * | denied    | -       | -         | -       | denied  |
	 * +-------------------------------------------+---------+
	 */
	snprintf(iname1.name, sizeof(iname1.name), NO_IQN1);

	result = iscsi_tgt_node_access(&conn, &tgtnode, iqn, addr);
	CU_ASSERT(result == false);

	/*
	 * case 2:
	 * +-------------------------------------------+---------+
	 * | IG1                 | IG2                 |         |
	 * +-------------------------------------------+         |
	 * | name      | addr    | name      | addr    | result  |
	 * +-------------------------------------------+---------+
	 * +-------------------------------------------+---------+
	 * | allowed   | allowed | -         | -       | allowed |
	 * +-------------------------------------------+---------+
	 */
	snprintf(iname1.name, sizeof(iname1.name), IQN1);
	snprintf(imask1.mask, sizeof(imask1.mask), IP1);

	result = iscsi_tgt_node_access(&conn, &tgtnode, iqn, addr);
	CU_ASSERT(result == true);

	/*
	 * case 3:
	 * +-------------------------------------------+---------+
	 * | IG1                 | IG2                 |         |
	 * +-------------------------------------------+         |
	 * | name      | addr    | name     | addr     | result  |
	 * +-------------------------------------------+---------+
	 * +-------------------------------------------+---------+
	 * | allowed   | denied  | denied   | -        | denied  |
	 * +-------------------------------------------+---------+
	 */
	snprintf(iname1.name, sizeof(iname1.name), IQN1);
	snprintf(imask1.mask, sizeof(imask1.mask), IP2);
	snprintf(iname2.name, sizeof(iname2.name), NO_IQN1);

	result = iscsi_tgt_node_access(&conn, &tgtnode, iqn, addr);
	CU_ASSERT(result == false);

	/*
	 * case 4:
	 * +-------------------------------------------+---------+
	 * | IG1                 | IG2                 |         |
	 * +-------------------------------------------+         |
	 * | name      | addr    | name      | addr    | result  |
	 * +-------------------------------------------+---------+
	 * +-------------------------------------------+---------+
	 * | allowed   | denied  | allowed   | allowed | allowed |
	 * +-------------------------------------------+---------+
	 */
	snprintf(iname1.name, sizeof(iname1.name), IQN1);
	snprintf(imask1.mask, sizeof(imask1.mask), IP2);
	snprintf(iname2.name, sizeof(iname2.name), IQN1);
	snprintf(imask2.mask, sizeof(imask2.mask), IP1);

	result = iscsi_tgt_node_access(&conn, &tgtnode, iqn, addr);
	CU_ASSERT(result == true);

	/*
	 * case 5:
	 * +---------------------------------------------+---------+
	 * | IG1                 | IG2                   |         |
	 * +---------------------------------------------+         |
	 * | name      | addr    | name        | addr    | result  |
	 * +---------------------------------------------+---------+
	 * +---------------------------------------------+---------+
	 * | allowed   | denied  | allowed     | denied  | denied  |
	 * +---------------------------------------------+---------+
	 */
	snprintf(iname1.name, sizeof(iname1.name), IQN1);
	snprintf(imask1.mask, sizeof(imask1.mask), IP2);
	snprintf(iname2.name, sizeof(iname2.name), IQN1);
	snprintf(imask2.mask, sizeof(imask2.mask), IP2);

	result = iscsi_tgt_node_access(&conn, &tgtnode, iqn, addr);
	CU_ASSERT(result == false);

	/*
	 * case 6:
	 * +---------------------------------------------+---------+
	 * | IG1                 | IG2                   |         |
	 * +---------------------------------------------+         |
	 * | name      | addr    | name        | addr    | result  |
	 * +---------------------------------------------+---------+
	 * +---------------------------------------------+---------+
	 * | allowed   | denied  | not found   | -       | denied  |
	 * +---------------------------------------------+---------+
	 */
	snprintf(iname1.name, sizeof(iname1.name), IQN1);
	snprintf(imask1.mask, sizeof(imask1.mask), IP2);
	snprintf(iname2.name, sizeof(iname2.name), IQN2);

	result = iscsi_tgt_node_access(&conn, &tgtnode, iqn, addr);
	CU_ASSERT(result == false);

	/*
	 * case 7:
	 * +---------------------------------------------+---------+
	 * | IG1                   | IG2                 |         |
	 * +---------------------------------------------+         |
	 * | name        | addr    | name      | addr    | result  |
	 * +---------------------------------------------+---------+
	 * +---------------------------------------------+---------+
	 * | not found   | -       | denied    | -       | denied  |
	 * +---------------------------------------------+---------+
	 */
	snprintf(iname1.name, sizeof(iname1.name), IQN2);
	snprintf(iname2.name, sizeof(iname2.name), NO_IQN1);

	result = iscsi_tgt_node_access(&conn, &tgtnode, iqn, addr);
	CU_ASSERT(result == false);

	/*
	 * case 8:
	 * +---------------------------------------------+---------+
	 * | IG1                   | IG2                 |         |
	 * +---------------------------------------------+         |
	 * | name        | addr    | name      | addr    | result  |
	 * +---------------------------------------------+---------+
	 * +---------------------------------------------+---------+
	 * | not found   | -       | allowed   | allowed | allowed |
	 * +---------------------------------------------+---------+
	 */
	snprintf(iname1.name, sizeof(iname1.name), IQN2);
	snprintf(iname2.name, sizeof(iname2.name), IQN1);
	snprintf(imask2.mask, sizeof(imask2.mask), IP1);

	result = iscsi_tgt_node_access(&conn, &tgtnode, iqn, addr);
	CU_ASSERT(result == true);

	/*
	 * case 9:
	 * +---------------------------------------------+---------+
	 * | IG1                   | IG2                 |         |
	 * +---------------------------------------------+         |
	 * | name        | addr    | name      | addr    | result  |
	 * +---------------------------------------------+---------+
	 * +---------------------------------------------+---------+
	 * | not found   | -       | allowed   | denied  | denied  |
	 * +---------------------------------------------+---------+
	 */
	snprintf(iname1.name, sizeof(iname1.name), IQN2);
	snprintf(iname2.name, sizeof(iname2.name), IQN1);
	snprintf(imask2.mask, sizeof(imask2.mask), IP2);

	result = iscsi_tgt_node_access(&conn, &tgtnode, iqn, addr);
	CU_ASSERT(result == false);

	/*
	 * case 10:
	 * +---------------------------------------------+---------+
	 * | IG1                   | IG2                 |         |
	 * +---------------------------------------------+         |
	 * | name        | addr    | name      | addr    | result  |
	 * +---------------------------------------------+---------+
	 * +---------------------------------------------+---------+
	 * | not found   | -       | not found | -       | denied  |
	 * +---------------------------------------------+---------+
	 */
	snprintf(iname1.name, sizeof(iname1.name), IQN2);
	snprintf(iname2.name, sizeof(iname2.name), IQN2);

	result = iscsi_tgt_node_access(&conn, &tgtnode, iqn, addr);
	CU_ASSERT(result == false);

	iscsi_pg_map_delete_ig_map(pg_map, &ig1);
	iscsi_pg_map_delete_ig_map(pg_map, &ig2);
	iscsi_tgt_node_delete_pg_map(&tgtnode, &pg);
}

static void
allow_iscsi_name_multi_maps_case(void)
{
	struct spdk_iscsi_tgt_node tgtnode = {};
	struct spdk_iscsi_portal_grp pg1 = {}, pg2 = {};
	struct spdk_iscsi_init_grp ig = {};
	struct spdk_iscsi_initiator_name iname = {};
	struct spdk_iscsi_pg_map *pg_map1, *pg_map2;
	struct spdk_scsi_dev scsi_dev = {};
	char *iqn;
	bool result;

	/* target initialization */
	TAILQ_INIT(&tgtnode.pg_map_head);

	snprintf(scsi_dev.name, sizeof(scsi_dev.name), IQN1);
	tgtnode.dev = &scsi_dev;

	/* initiator group initialization */
	TAILQ_INIT(&ig.initiator_head);

	ig.ninitiators = 1;
	TAILQ_INSERT_TAIL(&ig.initiator_head, &iname, tailq);

	/* portal group initialization */
	pg1.tag = 1;
	pg2.tag = 1;

	pg_map1 = iscsi_tgt_node_add_pg_map(&tgtnode, &pg1);
	pg_map2 = iscsi_tgt_node_add_pg_map(&tgtnode, &pg2);
	iscsi_pg_map_add_ig_map(pg_map1, &ig);
	iscsi_pg_map_add_ig_map(pg_map2, &ig);

	/* test for IG1 <-> PG1, PG2 case */
	iqn = IQN1;

	snprintf(iname.name, sizeof(iname.name), IQN1);

	result = iscsi_tgt_node_allow_iscsi_name(&tgtnode, iqn);
	CU_ASSERT(result == true);

	snprintf(iname.name, sizeof(iname.name), IQN2);

	result = iscsi_tgt_node_allow_iscsi_name(&tgtnode, iqn);
	CU_ASSERT(result == false);

	iscsi_pg_map_delete_ig_map(pg_map1, &ig);
	iscsi_pg_map_delete_ig_map(pg_map2, &ig);
	iscsi_tgt_node_delete_pg_map(&tgtnode, &pg1);
	iscsi_tgt_node_delete_pg_map(&tgtnode, &pg2);
}

/*
 * static bool
 * iscsi_check_chap_params(bool disable_chap, bool require_chap,
 *                              bool mutual_chap, int chap_group);
 */
static void
chap_param_test_cases(void)
{
	/* Auto */
	CU_ASSERT(iscsi_check_chap_params(false, false, false, 0) == true);

	/* None */
	CU_ASSERT(iscsi_check_chap_params(true, false, false, 0) == true);

	/* CHAP */
	CU_ASSERT(iscsi_check_chap_params(false, true, false, 0) == true);

	/* CHAP Mutual */
	CU_ASSERT(iscsi_check_chap_params(false, true, true, 0) == true);

	/* Check mutual exclusiveness of disabled and required */
	CU_ASSERT(iscsi_check_chap_params(true, true, false, 0) == false);

	/* Mutual requires Required */
	CU_ASSERT(iscsi_check_chap_params(false, false, true, 0) == false);

	/* Remaining combinations */
	CU_ASSERT(iscsi_check_chap_params(true, false, true, 0) == false);
	CU_ASSERT(iscsi_check_chap_params(true, true, true, 0) == false);

	/* Valid auth group ID */
	CU_ASSERT(iscsi_check_chap_params(false, false, false, 1) == true);

	/* Invalid auth group ID */
	CU_ASSERT(iscsi_check_chap_params(false, false, false, -1) == false);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("iscsi_target_node_suite", NULL, NULL);

	CU_ADD_TEST(suite, add_lun_test_cases);
	CU_ADD_TEST(suite, allow_any_allowed);
	CU_ADD_TEST(suite, allow_ipv6_allowed);
	CU_ADD_TEST(suite, allow_ipv6_denied);
	CU_ADD_TEST(suite, allow_ipv6_invalid);
	CU_ADD_TEST(suite, allow_ipv4_allowed);
	CU_ADD_TEST(suite, allow_ipv4_denied);
	CU_ADD_TEST(suite, allow_ipv4_invalid);
	CU_ADD_TEST(suite, node_access_allowed);
	CU_ADD_TEST(suite, node_access_denied_by_empty_netmask);
	CU_ADD_TEST(suite, node_access_multi_initiator_groups_cases);
	CU_ADD_TEST(suite, allow_iscsi_name_multi_maps_case);
	CU_ADD_TEST(suite, chap_param_test_cases);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
