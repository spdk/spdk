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
	int rc;
	char *netmask;
	char *addr;

	netmask = "[2001:ad6:1234::]/48";
	addr = "2001:ad6:1234:5678:9abc::";

	rc = spdk_iscsi_tgt_node_allow_ipv6(netmask, addr);
	CU_ASSERT(rc != 0);

	rc = spdk_iscsi_tgt_node_allow_netmask(netmask, addr);
	CU_ASSERT(rc != 0);
}

static void
allow_ipv6_denied(void)
{
	int rc;
	char *netmask;
	char *addr;

	netmask = "[2001:ad6:1234::]/56";
	addr = "2001:ad6:1234:5678:9abc::";

	rc = spdk_iscsi_tgt_node_allow_ipv6(netmask, addr);
	CU_ASSERT(rc == 0);

	rc = spdk_iscsi_tgt_node_allow_netmask(netmask, addr);
	CU_ASSERT(rc == 0);
}

static void
allow_ipv4_allowed(void)
{
	int rc;
	char *netmask;
	char *addr;

	netmask = "192.168.2.0/24";
	addr = "192.168.2.1";

	rc = spdk_iscsi_tgt_node_allow_ipv4(netmask, addr);
	CU_ASSERT(rc != 0);

	rc = spdk_iscsi_tgt_node_allow_netmask(netmask, addr);
	CU_ASSERT(rc != 0);
}

static void
allow_ipv4_denied(void)
{
	int rc;
	char *netmask;
	char *addr;

	netmask = "192.168.2.0";
	addr  = "192.168.2.1";

	rc = spdk_iscsi_tgt_node_allow_ipv4(netmask, addr);
	CU_ASSERT(rc == 0);

	rc = spdk_iscsi_tgt_node_allow_netmask(netmask, addr);
	CU_ASSERT(rc == 0);
}

static void
node_access_allowed1(void)
{
	struct spdk_iscsi_tgt_node tgtnode;
	struct spdk_iscsi_portal_grp pg;
	struct spdk_iscsi_init_grp ig;
	struct spdk_iscsi_conn conn;
	struct spdk_iscsi_portal portal;
	char *initiators[] = {"iqn.2017-10.spdk.io:0001"};
	char *netmasks[] = {"192.168.2.0/24"};
	char *iqn, *addr;
	int rc;

	/* portal group initialization */
	memset(&pg, 0, sizeof(struct spdk_iscsi_portal_grp));
	pg.tag = 1;

	/* initiator group initialization */
	memset(&ig, 0, sizeof(struct spdk_iscsi_init_grp));
	ig.tag = 1;

	ig.ninitiators = 1;
	ig.initiators = &initiators[0];

	ig.nnetmasks = 1;
	ig.netmasks = &netmasks[0];

	/* target initialization */
	memset(&tgtnode, 0, sizeof(struct spdk_iscsi_tgt_node));
	tgtnode.maxmap = 1;
	tgtnode.name = "iqn.2017-10.spdk.io:0001";
	tgtnode.map[0].pg = &pg;
	tgtnode.map[0].ig = &ig;

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

	rc = spdk_iscsi_tgt_node_access(&conn, &tgtnode, iqn, addr);
	CU_ASSERT(rc == 1);
}

static void
node_access_allowed2(void)
{
	struct spdk_iscsi_tgt_node tgtnode;
	struct spdk_iscsi_portal_grp pg;
	struct spdk_iscsi_init_grp ig;
	struct spdk_iscsi_conn conn;
	struct spdk_iscsi_portal portal;
	char *initiators[] = {"iqn.2017-10.spdk.io:0001"};
	char *iqn, *addr;
	int rc;

	/* portal group initialization */
	memset(&pg, 0, sizeof(struct spdk_iscsi_portal_grp));
	pg.tag = 1;

	/* initiator group initialization */
	memset(&ig, 0, sizeof(struct spdk_iscsi_init_grp));
	ig.tag = 1;

	ig.ninitiators = 1;
	ig.initiators = &initiators[0];

	ig.nnetmasks = 0;
	ig.netmasks = NULL;

	/* target initialization */
	memset(&tgtnode, 0, sizeof(struct spdk_iscsi_tgt_node));
	tgtnode.maxmap = 1;
	tgtnode.name = "iqn.2017-10.spdk.io:0001";
	tgtnode.map[0].pg = &pg;
	tgtnode.map[0].ig = &ig;

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

	rc = spdk_iscsi_tgt_node_access(&conn, &tgtnode, iqn, addr);
	CU_ASSERT(rc == 1);
}

static void
node_access_denied(void)
{
	struct spdk_iscsi_tgt_node tgtnode;
	struct spdk_iscsi_portal_grp pg;
	struct spdk_iscsi_init_grp ig;
	struct spdk_iscsi_conn conn;
	struct spdk_iscsi_portal portal;
	char *initiators[] = {"iqn.2017-10.spdk.io:0001"};
	char *netmasks[] = {"192.168.2.0/24"};
	char *iqn, *addr;
	int rc;

	/* portal group initialization */
	memset(&pg, 0, sizeof(struct spdk_iscsi_portal_grp));
	pg.tag = 1;

	/* initiator group initialization */
	memset(&ig, 0, sizeof(struct spdk_iscsi_init_grp));
	ig.tag = 1;

	ig.ninitiators = 1;
	ig.initiators = &initiators[0];

	ig.nnetmasks = 1;
	ig.netmasks = &netmasks[0];

	/* target initialization */
	memset(&tgtnode, 0, sizeof(struct spdk_iscsi_tgt_node));
	tgtnode.maxmap = 1;
	tgtnode.name = "iqn.2017-10.spdk.io:0001";
	tgtnode.map[0].pg = &pg;
	tgtnode.map[0].ig = &ig;

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

	rc = spdk_iscsi_tgt_node_access(&conn, &tgtnode, iqn, addr);
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
		|| CU_add_test(suite, "node access allowed case1", node_access_allowed1) == NULL
		|| CU_add_test(suite, "node access allowed case2", node_access_allowed2) == NULL
		|| CU_add_test(suite, "node access denied case", node_access_denied) == NULL
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
