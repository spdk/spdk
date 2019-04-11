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

struct spdk_iscsi_globals g_spdk_iscsi;

const char *config_file;

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

DEFINE_STUB(spdk_iscsi_portal_grp_find_by_tag,
	    struct spdk_iscsi_portal_grp *, (int tag), NULL);

DEFINE_STUB(spdk_iscsi_init_grp_find_by_tag, struct spdk_iscsi_init_grp *,
	    (int tag), NULL);

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
	struct spdk_iscsi_tgt_node tgtnode;
	int lun_id = 0;
	char *bdev_name = NULL;
	struct spdk_scsi_dev scsi_dev;
	int rc;

	memset(&tgtnode, 0, sizeof(struct spdk_iscsi_tgt_node));
	memset(&scsi_dev, 0, sizeof(struct spdk_scsi_dev));

	/* case 1 */
	tgtnode.num_active_conns = 1;

	rc = spdk_iscsi_tgt_node_add_lun(&tgtnode, bdev_name, lun_id);
	CU_ASSERT(rc != 0);

	/* case 2 */
	tgtnode.num_active_conns = 0;
	lun_id = -2;

	rc = spdk_iscsi_tgt_node_add_lun(&tgtnode, bdev_name, lun_id);
	CU_ASSERT(rc != 0);

	/* case 3 */
	lun_id = SPDK_SCSI_DEV_MAX_LUN;

	rc = spdk_iscsi_tgt_node_add_lun(&tgtnode, bdev_name, lun_id);
	CU_ASSERT(rc != 0);

	/* case 4 */
	lun_id = -1;
	tgtnode.dev = NULL;

	rc = spdk_iscsi_tgt_node_add_lun(&tgtnode, bdev_name, lun_id);
	CU_ASSERT(rc != 0);

	/* case 5 */
	tgtnode.dev = &scsi_dev;

	rc = spdk_iscsi_tgt_node_add_lun(&tgtnode, bdev_name, lun_id);
	CU_ASSERT(rc != 0);

	/* case 6 */
	bdev_name = "LUN0";

	rc = spdk_iscsi_tgt_node_add_lun(&tgtnode, bdev_name, lun_id);
	CU_ASSERT(rc == 0);
}

static void
config_file_fail_cases(void)
{
	struct spdk_conf *config;
	struct spdk_conf_section *sp;
	char section_name[64];
	int section_index;
	int rc;

	config = spdk_conf_allocate();

	rc = spdk_conf_read(config, config_file);
	CU_ASSERT(rc == 0);

	section_index = 0;
	while (true) {
		snprintf(section_name, sizeof(section_name), "Failure%d", section_index);
		sp = spdk_conf_find_section(config, section_name);
		if (sp == NULL) {
			break;
		}
		rc = iscsi_parse_tgt_node(sp);
		CU_ASSERT(rc < 0);
		section_index++;
	}

	spdk_conf_free(config);
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
	struct spdk_iscsi_tgt_node tgtnode;
	struct spdk_iscsi_portal_grp pg;
	struct spdk_iscsi_init_grp ig;
	struct spdk_iscsi_conn conn;
	struct spdk_iscsi_portal portal;
	struct spdk_iscsi_initiator_name iname;
	struct spdk_iscsi_initiator_netmask imask;
	struct spdk_scsi_dev scsi_dev;
	struct spdk_iscsi_pg_map *pg_map;
	char *iqn, *addr;
	bool result;

	/* portal group initialization */
	memset(&pg, 0, sizeof(struct spdk_iscsi_portal_grp));
	pg.tag = 1;

	/* initiator group initialization */
	memset(&ig, 0, sizeof(struct spdk_iscsi_init_grp));
	ig.tag = 1;

	ig.ninitiators = 1;
	iname.name = "iqn.2017-10.spdk.io:0001";
	TAILQ_INIT(&ig.initiator_head);
	TAILQ_INSERT_TAIL(&ig.initiator_head, &iname, tailq);

	ig.nnetmasks = 1;
	imask.mask = "192.168.2.0/24";
	TAILQ_INIT(&ig.netmask_head);
	TAILQ_INSERT_TAIL(&ig.netmask_head, &imask, tailq);

	/* target initialization */
	memset(&tgtnode, 0, sizeof(struct spdk_iscsi_tgt_node));
	tgtnode.name = "iqn.2017-10.spdk.io:0001";
	TAILQ_INIT(&tgtnode.pg_map_head);

	memset(&scsi_dev, 0, sizeof(struct spdk_scsi_dev));
	snprintf(scsi_dev.name, sizeof(scsi_dev.name), "iqn.2017-10.spdk.io:0001");
	tgtnode.dev = &scsi_dev;

	pg_map = iscsi_tgt_node_add_pg_map(&tgtnode, &pg);
	iscsi_pg_map_add_ig_map(pg_map, &ig);

	/* portal initialization */
	memset(&portal, 0, sizeof(struct spdk_iscsi_portal));
	portal.group = &pg;
	portal.host = "192.168.2.0";
	portal.port = "3260";

	/* input for UT */
	memset(&conn, 0, sizeof(struct spdk_iscsi_conn));
	conn.portal = &portal;

	iqn = "iqn.2017-10.spdk.io:0001";
	addr = "192.168.2.1";

	result = spdk_iscsi_tgt_node_access(&conn, &tgtnode, iqn, addr);
	CU_ASSERT(result == true);

	iscsi_pg_map_delete_ig_map(pg_map, &ig);
	iscsi_tgt_node_delete_pg_map(&tgtnode, &pg);
}

static void
node_access_denied_by_empty_netmask(void)
{
	struct spdk_iscsi_tgt_node tgtnode;
	struct spdk_iscsi_portal_grp pg;
	struct spdk_iscsi_init_grp ig;
	struct spdk_iscsi_conn conn;
	struct spdk_iscsi_portal portal;
	struct spdk_iscsi_initiator_name iname;
	struct spdk_scsi_dev scsi_dev;
	struct spdk_iscsi_pg_map *pg_map;
	char *iqn, *addr;
	bool result;

	/* portal group initialization */
	memset(&pg, 0, sizeof(struct spdk_iscsi_portal_grp));
	pg.tag = 1;

	/* initiator group initialization */
	memset(&ig, 0, sizeof(struct spdk_iscsi_init_grp));
	ig.tag = 1;

	ig.ninitiators = 1;
	iname.name = "iqn.2017-10.spdk.io:0001";
	TAILQ_INIT(&ig.initiator_head);
	TAILQ_INSERT_TAIL(&ig.initiator_head, &iname, tailq);

	ig.nnetmasks = 0;
	TAILQ_INIT(&ig.netmask_head);

	/* target initialization */
	memset(&tgtnode, 0, sizeof(struct spdk_iscsi_tgt_node));
	tgtnode.name = "iqn.2017-10.spdk.io:0001";
	TAILQ_INIT(&tgtnode.pg_map_head);

	memset(&scsi_dev, 0, sizeof(struct spdk_scsi_dev));
	snprintf(scsi_dev.name, sizeof(scsi_dev.name), "iqn.2017-10.spdk.io:0001");
	tgtnode.dev = &scsi_dev;

	pg_map = iscsi_tgt_node_add_pg_map(&tgtnode, &pg);
	iscsi_pg_map_add_ig_map(pg_map, &ig);

	/* portal initialization */
	memset(&portal, 0, sizeof(struct spdk_iscsi_portal));
	portal.group = &pg;
	portal.host = "192.168.2.0";
	portal.port = "3260";

	/* input for UT */
	memset(&conn, 0, sizeof(struct spdk_iscsi_conn));
	conn.portal = &portal;

	iqn = "iqn.2017-10.spdk.io:0001";
	addr = "192.168.3.1";

	result = spdk_iscsi_tgt_node_access(&conn, &tgtnode, iqn, addr);
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
	struct spdk_iscsi_tgt_node tgtnode;
	struct spdk_iscsi_conn conn;
	struct spdk_iscsi_portal_grp pg;
	struct spdk_iscsi_portal portal;
	struct spdk_iscsi_init_grp ig1, ig2;
	struct spdk_iscsi_initiator_name iname1, iname2;
	struct spdk_iscsi_initiator_netmask imask1, imask2;
	struct spdk_scsi_dev scsi_dev;
	struct spdk_iscsi_pg_map *pg_map;
	char *iqn, *addr;
	bool result;

	/* target initialization */
	memset(&tgtnode, 0, sizeof(struct spdk_iscsi_tgt_node));
	tgtnode.name = IQN1;
	TAILQ_INIT(&tgtnode.pg_map_head);

	memset(&scsi_dev, 0, sizeof(struct spdk_scsi_dev));
	snprintf(scsi_dev.name, sizeof(scsi_dev.name), IQN1);
	tgtnode.dev = &scsi_dev;

	/* initiator group initialization */
	memset(&ig1, 0, sizeof(struct spdk_iscsi_init_grp));
	ig1.tag = 1;
	TAILQ_INIT(&ig1.initiator_head);
	TAILQ_INIT(&ig1.netmask_head);

	ig1.ninitiators = 1;
	iname1.name = NULL;
	TAILQ_INSERT_TAIL(&ig1.initiator_head, &iname1, tailq);

	ig1.nnetmasks = 1;
	imask1.mask = NULL;
	TAILQ_INSERT_TAIL(&ig1.netmask_head, &imask1, tailq);

	memset(&ig2, 0, sizeof(struct spdk_iscsi_init_grp));
	ig2.tag = 2;
	TAILQ_INIT(&ig2.initiator_head);
	TAILQ_INIT(&ig2.netmask_head);

	ig2.ninitiators = 1;
	iname2.name = NULL;
	TAILQ_INSERT_TAIL(&ig2.initiator_head, &iname2, tailq);

	ig2.nnetmasks = 1;
	imask2.mask = NULL;
	TAILQ_INSERT_TAIL(&ig2.netmask_head, &imask2, tailq);

	/* portal group initialization */
	memset(&pg, 0, sizeof(struct spdk_iscsi_portal_grp));
	pg.tag = 1;

	pg_map = iscsi_tgt_node_add_pg_map(&tgtnode, &pg);
	iscsi_pg_map_add_ig_map(pg_map, &ig1);
	iscsi_pg_map_add_ig_map(pg_map, &ig2);

	/* portal initialization */
	memset(&portal, 0, sizeof(struct spdk_iscsi_portal));
	portal.group = &pg;
	portal.host = IP1;
	portal.port = "3260";

	/* connection initialization */
	memset(&conn, 0, sizeof(struct spdk_iscsi_conn));
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
	iname1.name = NO_IQN1;

	result = spdk_iscsi_tgt_node_access(&conn, &tgtnode, iqn, addr);
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
	iname1.name = IQN1;
	imask1.mask = IP1;

	result = spdk_iscsi_tgt_node_access(&conn, &tgtnode, iqn, addr);
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
	iname1.name = IQN1;
	imask1.mask = IP2;
	iname2.name = NO_IQN1;

	result = spdk_iscsi_tgt_node_access(&conn, &tgtnode, iqn, addr);
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
	iname1.name = IQN1;
	imask1.mask = IP2;
	iname2.name = IQN1;
	imask2.mask = IP1;

	result = spdk_iscsi_tgt_node_access(&conn, &tgtnode, iqn, addr);
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
	iname1.name = IQN1;
	imask1.mask = IP2;
	iname2.name = IQN1;
	imask2.mask = IP2;

	result = spdk_iscsi_tgt_node_access(&conn, &tgtnode, iqn, addr);
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
	iname1.name = IQN1;
	imask1.mask = IP2;
	iname2.name = IQN2;

	result = spdk_iscsi_tgt_node_access(&conn, &tgtnode, iqn, addr);
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
	iname1.name = IQN2;
	iname2.name = NO_IQN1;

	result = spdk_iscsi_tgt_node_access(&conn, &tgtnode, iqn, addr);
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
	iname1.name = IQN2;
	iname2.name = IQN1;
	imask2.mask = IP1;

	result = spdk_iscsi_tgt_node_access(&conn, &tgtnode, iqn, addr);
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
	iname1.name = IQN2;
	iname2.name = IQN1;
	imask2.mask = IP2;

	result = spdk_iscsi_tgt_node_access(&conn, &tgtnode, iqn, addr);
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
	iname1.name = IQN2;
	iname2.name = IQN2;

	result = spdk_iscsi_tgt_node_access(&conn, &tgtnode, iqn, addr);
	CU_ASSERT(result == false);

	iscsi_pg_map_delete_ig_map(pg_map, &ig1);
	iscsi_pg_map_delete_ig_map(pg_map, &ig2);
	iscsi_tgt_node_delete_pg_map(&tgtnode, &pg);
}

static void
allow_iscsi_name_multi_maps_case(void)
{
	struct spdk_iscsi_tgt_node tgtnode;
	struct spdk_iscsi_portal_grp pg1, pg2;
	struct spdk_iscsi_init_grp ig;
	struct spdk_iscsi_initiator_name iname;
	struct spdk_iscsi_pg_map *pg_map1, *pg_map2;
	struct spdk_scsi_dev scsi_dev;
	char *iqn;
	bool result;

	/* target initialization */
	memset(&tgtnode, 0, sizeof(struct spdk_iscsi_tgt_node));
	TAILQ_INIT(&tgtnode.pg_map_head);

	memset(&scsi_dev, 0, sizeof(struct spdk_scsi_dev));
	snprintf(scsi_dev.name, sizeof(scsi_dev.name), IQN1);
	tgtnode.dev = &scsi_dev;

	/* initiator group initialization */
	memset(&ig, 0, sizeof(struct spdk_iscsi_init_grp));
	TAILQ_INIT(&ig.initiator_head);

	ig.ninitiators = 1;
	iname.name = NULL;
	TAILQ_INSERT_TAIL(&ig.initiator_head, &iname, tailq);

	/* portal group initialization */
	memset(&pg1, 0, sizeof(struct spdk_iscsi_portal_grp));
	pg1.tag = 1;
	memset(&pg2, 0, sizeof(struct spdk_iscsi_portal_grp));
	pg2.tag = 1;

	pg_map1 = iscsi_tgt_node_add_pg_map(&tgtnode, &pg1);
	pg_map2 = iscsi_tgt_node_add_pg_map(&tgtnode, &pg2);
	iscsi_pg_map_add_ig_map(pg_map1, &ig);
	iscsi_pg_map_add_ig_map(pg_map2, &ig);

	/* test for IG1 <-> PG1, PG2 case */
	iqn = IQN1;

	iname.name = IQN1;

	result = iscsi_tgt_node_allow_iscsi_name(&tgtnode, iqn);
	CU_ASSERT(result == true);

	iname.name = IQN2;

	result = iscsi_tgt_node_allow_iscsi_name(&tgtnode, iqn);
	CU_ASSERT(result == false);

	iscsi_pg_map_delete_ig_map(pg_map1, &ig);
	iscsi_pg_map_delete_ig_map(pg_map2, &ig);
	iscsi_tgt_node_delete_pg_map(&tgtnode, &pg1);
	iscsi_tgt_node_delete_pg_map(&tgtnode, &pg2);
}

/*
 * static bool
 * spdk_iscsi_check_chap_params(bool disable_chap, bool require_chap,
 *                              bool mutual_chap, int chap_group);
 */
static void
chap_param_test_cases(void)
{
	/* Auto */
	CU_ASSERT(spdk_iscsi_check_chap_params(false, false, false, 0) == true);

	/* None */
	CU_ASSERT(spdk_iscsi_check_chap_params(true, false, false, 0) == true);

	/* CHAP */
	CU_ASSERT(spdk_iscsi_check_chap_params(false, true, false, 0) == true);

	/* CHAP Mutual */
	CU_ASSERT(spdk_iscsi_check_chap_params(false, true, true, 0) == true);

	/* Check mutual exclusiveness of disabled and required */
	CU_ASSERT(spdk_iscsi_check_chap_params(true, true, false, 0) == false);

	/* Mutual requires Required */
	CU_ASSERT(spdk_iscsi_check_chap_params(false, false, true, 0) == false);

	/* Remaining combinations */
	CU_ASSERT(spdk_iscsi_check_chap_params(true, false, true, 0) == false);
	CU_ASSERT(spdk_iscsi_check_chap_params(true, true, true, 0) == false);

	/* Valid auth group ID */
	CU_ASSERT(spdk_iscsi_check_chap_params(false, false, false, 1) == true);

	/* Invalid auth group ID */
	CU_ASSERT(spdk_iscsi_check_chap_params(false, false, false, -1) == false);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <config file>\n", argv[0]);
		exit(1);
	}

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	config_file = argv[1];

	suite = CU_add_suite("iscsi_target_node_suite", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "add lun test cases", add_lun_test_cases) == NULL
		|| CU_add_test(suite, "config file fail cases", config_file_fail_cases) == NULL
		|| CU_add_test(suite, "allow any allowed case", allow_any_allowed) == NULL
		|| CU_add_test(suite, "allow ipv6 allowed case", allow_ipv6_allowed) == NULL
		|| CU_add_test(suite, "allow ipv6 denied case", allow_ipv6_denied) == NULL
		|| CU_add_test(suite, "allow ipv6 invalid case", allow_ipv6_invalid) == NULL
		|| CU_add_test(suite, "allow ipv4 allowed case", allow_ipv4_allowed) == NULL
		|| CU_add_test(suite, "allow ipv4 denied case", allow_ipv4_denied) == NULL
		|| CU_add_test(suite, "allow ipv4 invalid case", allow_ipv4_invalid) == NULL
		|| CU_add_test(suite, "node access allowed case", node_access_allowed) == NULL
		|| CU_add_test(suite, "node access denied case (empty netmask)",
			       node_access_denied_by_empty_netmask) == NULL
		|| CU_add_test(suite, "node access multiple initiator groups cases",
			       node_access_multi_initiator_groups_cases) == NULL
		|| CU_add_test(suite, "allow iscsi name case",
			       allow_iscsi_name_multi_maps_case) == NULL
		|| CU_add_test(suite, "chap param test cases", chap_param_test_cases) == NULL
	) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
