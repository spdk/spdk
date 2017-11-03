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

#include "../common.c"
#include "iscsi/tgt_node.c"
#include "scsi/scsi_internal.h"

struct spdk_iscsi_globals g_spdk_iscsi;

const char *config_file;

bool
spdk_sock_is_ipv6(int sock)
{
	return false;
}

bool
spdk_sock_is_ipv4(int sock)
{
	return false;
}

struct spdk_iscsi_portal_grp *
spdk_iscsi_portal_grp_find_by_tag(int tag)
{
	return NULL;
}

struct spdk_iscsi_init_grp *
spdk_iscsi_init_grp_find_by_tag(int tag)
{
	return NULL;
}

struct spdk_scsi_lun *
spdk_scsi_dev_get_lun(struct spdk_scsi_dev *dev, int lun_id)
{
	if (lun_id < 0 || lun_id >= SPDK_SCSI_DEV_MAX_LUN) {
		return NULL;
	}

	return dev->lun[lun_id];
}

int
spdk_scsi_dev_find_lowest_free_lun_id(struct spdk_scsi_dev *dev)
{
	return -1;
}

int
spdk_scsi_dev_add_lun(struct spdk_scsi_dev *dev, char *lun_name, int lun_id,
		      void (*hotremove_cb)(const struct spdk_scsi_lun *, void *),
		      void *hotremove_ctx)
{
	return -1;
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
		rc = spdk_cf_add_iscsi_tgt_node(sp);
		CU_ASSERT(rc < 0);
		section_index++;
	}

	spdk_conf_free(config);
}

static void
allow_ipv6_allowed(void)
{
	bool result;
	char *netmask;
	char *addr;

	netmask = "[2001:ad6:1234::]/48";
	addr = "2001:ad6:1234:5678:9abc::";

	result = spdk_iscsi_ipv6_netmask_allow_addr(netmask, addr);
	CU_ASSERT(result == true);

	result = spdk_iscsi_netmask_allow_addr(netmask, addr);
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

	result = spdk_iscsi_ipv6_netmask_allow_addr(netmask, addr);
	CU_ASSERT(result == false);

	result = spdk_iscsi_netmask_allow_addr(netmask, addr);
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

	result = spdk_iscsi_ipv4_netmask_allow_addr(netmask, addr);
	CU_ASSERT(result == true);

	result = spdk_iscsi_netmask_allow_addr(netmask, addr);
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

	result = spdk_iscsi_ipv4_netmask_allow_addr(netmask, addr);
	CU_ASSERT(result == false);

	result = spdk_iscsi_netmask_allow_addr(netmask, addr);
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
	struct spdk_iscsi_ig_map *ig_map;
	char *iqn, *addr;
	bool result;
	int rc;

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
	memset(&scsi_dev, 0, sizeof(struct spdk_scsi_dev));
	memset(&tgtnode, 0, sizeof(struct spdk_iscsi_tgt_node));
	TAILQ_INIT(&tgtnode.pg_map_head);
	tgtnode.name = "iqn.2017-10.spdk.io:0001";
	tgtnode.dev = &scsi_dev;

	pg_map = spdk_iscsi_tgt_node_add_pg_map(&tgtnode, &pg);
	CU_ASSERT(pg_map != NULL);

	ig_map = spdk_iscsi_pg_map_add_ig_map(pg_map, &ig);
	CU_ASSERT(ig_map != NULL);

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

	rc = spdk_iscsi_pg_map_delete_ig_map(pg_map, &ig);
	CU_ASSERT(rc == 0);

	rc = spdk_iscsi_tgt_node_delete_pg_map(&tgtnode, &pg);
	CU_ASSERT(rc == 0);
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
	struct spdk_iscsi_ig_map *ig_map;
	char *iqn, *addr;
	bool result;
	int rc;

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
	memset(&scsi_dev, 0, sizeof(struct spdk_scsi_dev));
	memset(&tgtnode, 0, sizeof(struct spdk_iscsi_tgt_node));
        TAILQ_INIT(&tgtnode.pg_map_head);
	tgtnode.name = "iqn.2017-10.spdk.io:0001";
	tgtnode.dev = &scsi_dev;

	pg_map = spdk_iscsi_tgt_node_add_pg_map(&tgtnode, &pg);
	CU_ASSERT(pg_map != NULL);

	ig_map = spdk_iscsi_pg_map_add_ig_map(pg_map, &ig);
	CU_ASSERT(ig_map != NULL);

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

	rc = spdk_iscsi_pg_map_delete_ig_map(pg_map, &ig);
	CU_ASSERT(rc == 0);

	rc = spdk_iscsi_tgt_node_delete_pg_map(&tgtnode, &pg);
	CU_ASSERT(rc == 0);
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
		CU_add_test(suite, "config file fail cases", config_file_fail_cases) == NULL
		|| CU_add_test(suite, "allow ipv6 allowed case", allow_ipv6_allowed) == NULL
		|| CU_add_test(suite, "allow ipv6 denied case", allow_ipv6_denied) == NULL
		|| CU_add_test(suite, "allow ipv4 allowed case", allow_ipv4_allowed) == NULL
		|| CU_add_test(suite, "allow ipv4 denied case", allow_ipv4_denied) == NULL
		|| CU_add_test(suite, "node access allowed case", node_access_allowed) == NULL
		|| CU_add_test(suite, "node access denied case (empty netmask)",
			       node_access_denied_by_empty_netmask) == NULL
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
