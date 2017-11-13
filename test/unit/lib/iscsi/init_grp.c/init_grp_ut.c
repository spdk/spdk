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

#include "CUnit/Basic.h"

#include "iscsi/init_grp.c"

struct spdk_iscsi_globals g_spdk_iscsi;

const char *config_file;

static int
test_setup(void)
{
	TAILQ_INIT(&g_spdk_iscsi.ig_head);
	return 0;
}

static void
create_from_config_file_cases(void)
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
		snprintf(section_name, sizeof(section_name), "IG_Valid%d", section_index);

		sp = spdk_conf_find_section(config, section_name);
		if (sp == NULL) {
			break;
		}

		rc = spdk_iscsi_init_grp_create_from_configfile(sp);
		CU_ASSERT(rc == 0);

		spdk_iscsi_init_grp_array_destroy();

		section_index++;
	}

	section_index = 0;
	while (true) {
		snprintf(section_name, sizeof(section_name), "IG_Invalid%d", section_index);

		sp = spdk_conf_find_section(config, section_name);
		if (sp == NULL) {
			break;
		}

		rc = spdk_iscsi_init_grp_create_from_configfile(sp);
		CU_ASSERT(rc != 0);

		spdk_iscsi_init_grp_array_destroy();

		section_index++;
	}

	spdk_conf_free(config);
}


static void
create_initiator_group_success_case(void)
{
	struct spdk_iscsi_init_grp *ig;

	ig = spdk_iscsi_init_grp_create(1);
	CU_ASSERT(ig != NULL);

	spdk_iscsi_init_grp_destroy(ig);
}

static void
find_initiator_group_success_case(void)
{
	struct spdk_iscsi_init_grp *ig;

	ig = spdk_iscsi_init_grp_create(1);
	CU_ASSERT(ig != NULL);

	spdk_iscsi_init_grp_register(ig);

	ig = spdk_iscsi_init_grp_find_by_tag(1);
	CU_ASSERT(ig != NULL);

	spdk_initiator_group_unregister(ig);
	spdk_iscsi_init_grp_destroy(ig);

	ig = spdk_iscsi_init_grp_find_by_tag(1);
	CU_ASSERT(ig == NULL);
}

static void
create_initiator_group_fail_case(void)
{
	struct spdk_iscsi_init_grp *ig;

	ig = spdk_iscsi_init_grp_create(1);
	CU_ASSERT(ig != NULL);

	spdk_iscsi_init_grp_register(ig);

	ig = spdk_iscsi_init_grp_create(1);
	CU_ASSERT(ig == NULL);

	ig = spdk_iscsi_init_grp_find_by_tag(1);
	CU_ASSERT(ig != NULL);

	spdk_initiator_group_unregister(ig);
	spdk_iscsi_init_grp_destroy(ig);

	ig = spdk_iscsi_init_grp_find_by_tag(1);
	CU_ASSERT(ig == NULL);
}

static void
add_initiator_name_success_case(void)
{
	int rc;
	struct spdk_iscsi_init_grp *ig;
	struct spdk_iscsi_initiator_name *iname;
	char *name1 = "iqn.2017-10.spdk.io:0001";
	char *name2 = "iqn.2017-10.spdk.io:0002";

	ig = spdk_iscsi_init_grp_create(1);
	CU_ASSERT(ig != NULL);

	/* add two different names to the empty name list */
	rc = spdk_iscsi_init_grp_add_initiator(ig, name1);
	CU_ASSERT(rc == 0);

	rc = spdk_iscsi_init_grp_add_initiator(ig, name2);
	CU_ASSERT(rc == 0);

	/* check if two names are added correctly. */
	iname = spdk_iscsi_init_grp_find_initiator(ig, name1);
	CU_ASSERT(iname != NULL);

	iname = spdk_iscsi_init_grp_find_initiator(ig, name2);
	CU_ASSERT(iname != NULL);

	/* restore the initial state */
	rc = spdk_iscsi_init_grp_delete_initiator(ig, name1);
	CU_ASSERT(rc == 0);

	iname = spdk_iscsi_init_grp_find_initiator(ig, name1);
	CU_ASSERT(iname == NULL);

	rc = spdk_iscsi_init_grp_delete_initiator(ig, name2);
	CU_ASSERT(rc == 0);

	iname = spdk_iscsi_init_grp_find_initiator(ig, name2);
	CU_ASSERT(iname == NULL);

	spdk_iscsi_init_grp_destroy(ig);
}

static void
add_initiator_name_fail_case(void)
{
	int rc;
	struct spdk_iscsi_init_grp *ig;
	struct spdk_iscsi_initiator_name *iname;
	char *name1 = "iqn.2017-10.spdk.io:0001";

	ig = spdk_iscsi_init_grp_create(1);
	CU_ASSERT(ig != NULL);

	/* add an name to the full name list */
	ig->ninitiators = MAX_INITIATOR;

	rc = spdk_iscsi_init_grp_add_initiator(ig, name1);
	CU_ASSERT(rc != 0);

	ig->ninitiators = 0;

	/* add the same name to the name list twice */
	rc = spdk_iscsi_init_grp_add_initiator(ig, name1);
	CU_ASSERT(rc == 0);

	rc = spdk_iscsi_init_grp_add_initiator(ig, name1);
	CU_ASSERT(rc != 0);

	/* restore the initial state */
	rc = spdk_iscsi_init_grp_delete_initiator(ig, name1);
	CU_ASSERT(rc == 0);

	iname = spdk_iscsi_init_grp_find_initiator(ig, name1);
	CU_ASSERT(iname == NULL);

	spdk_iscsi_init_grp_destroy(ig);
}

static void
delete_all_initiator_names_success_case(void)
{
	int rc;
	struct spdk_iscsi_init_grp *ig;
	struct spdk_iscsi_initiator_name *iname;
	char *name1 = "iqn.2017-10.spdk.io:0001";
	char *name2 = "iqn.2017-10.spdk.io:0002";

	ig = spdk_iscsi_init_grp_create(1);
	CU_ASSERT(ig != NULL);

	/* add two different names to the empty name list */
	rc = spdk_iscsi_init_grp_add_initiator(ig, name1);
	CU_ASSERT(rc == 0);

	rc = spdk_iscsi_init_grp_add_initiator(ig, name2);
	CU_ASSERT(rc == 0);

	/* delete all initiator names */
	spdk_iscsi_init_grp_delete_all_initiators(ig);

	/* check if two names are deleted correctly. */
	iname = spdk_iscsi_init_grp_find_initiator(ig, name1);
	CU_ASSERT(iname == NULL);

	iname = spdk_iscsi_init_grp_find_initiator(ig, name2);
	CU_ASSERT(iname == NULL);

	/* restore the initial state */
	spdk_iscsi_init_grp_destroy(ig);
}

static void
add_netmask_success_case(void)
{
	int rc;
	struct spdk_iscsi_init_grp *ig;
	struct spdk_iscsi_initiator_netmask *imask;
	char *netmask1 = "192.168.2.0";
	char *netmask2 = "192.168.2.1";

	ig = spdk_iscsi_init_grp_create(1);
	CU_ASSERT(ig != NULL);

	/* add two different netmasks to the empty netmask list */
	rc = spdk_iscsi_init_grp_add_netmask(ig, netmask1);
	CU_ASSERT(rc == 0);

	rc = spdk_iscsi_init_grp_add_netmask(ig, netmask2);
	CU_ASSERT(rc == 0);

	/* check if two netmasks are added correctly. */
	imask = spdk_iscsi_init_grp_find_netmask(ig, netmask1);
	CU_ASSERT(imask != NULL);

	imask = spdk_iscsi_init_grp_find_netmask(ig, netmask2);
	CU_ASSERT(imask != NULL);

	/* restore the initial state */
	rc = spdk_iscsi_init_grp_delete_netmask(ig, netmask1);
	CU_ASSERT(rc == 0);

	imask = spdk_iscsi_init_grp_find_netmask(ig, netmask1);
	CU_ASSERT(imask == NULL);

	rc = spdk_iscsi_init_grp_delete_netmask(ig, netmask2);
	CU_ASSERT(rc == 0);

	imask = spdk_iscsi_init_grp_find_netmask(ig, netmask2);
	CU_ASSERT(imask == NULL);

	spdk_iscsi_init_grp_destroy(ig);
}

static void
add_netmask_fail_case(void)
{
	int rc;
	struct spdk_iscsi_init_grp *ig;
	struct spdk_iscsi_initiator_netmask *imask;
	char *netmask1 = "192.168.2.0";

	ig = spdk_iscsi_init_grp_create(1);
	CU_ASSERT(ig != NULL);

	/* add an netmask to the full netmask list */
	ig->nnetmasks = MAX_NETMASK;

	rc = spdk_iscsi_init_grp_add_netmask(ig, netmask1);
	CU_ASSERT(rc != 0);

	ig->nnetmasks = 0;

	/* add the same netmask to the netmask list twice */
	rc = spdk_iscsi_init_grp_add_netmask(ig, netmask1);
	CU_ASSERT(rc == 0);

	rc = spdk_iscsi_init_grp_add_netmask(ig, netmask1);
	CU_ASSERT(rc != 0);

	/* restore the initial state */
	rc = spdk_iscsi_init_grp_delete_netmask(ig, netmask1);
	CU_ASSERT(rc == 0);

	imask = spdk_iscsi_init_grp_find_netmask(ig, netmask1);
	CU_ASSERT(imask == NULL);

	spdk_iscsi_init_grp_destroy(ig);
}

static void
delete_all_netmasks_success_case(void)
{
	int rc;
	struct spdk_iscsi_init_grp *ig;
	struct spdk_iscsi_initiator_netmask *imask;
	char *netmask1 = "192.168.2.0";
	char *netmask2 = "192.168.2.1";

	ig = spdk_iscsi_init_grp_create(1);
	CU_ASSERT(ig != NULL);

	/* add two different netmasks to the empty netmask list */
	rc = spdk_iscsi_init_grp_add_netmask(ig, netmask1);
	CU_ASSERT(rc == 0);

	rc = spdk_iscsi_init_grp_add_netmask(ig, netmask2);
	CU_ASSERT(rc == 0);

	/* delete all netmasks */
	spdk_iscsi_init_grp_delete_all_netmasks(ig);

	/* check if two netmasks are deleted correctly. */
	imask = spdk_iscsi_init_grp_find_netmask(ig, netmask1);
	CU_ASSERT(imask == NULL);

	imask = spdk_iscsi_init_grp_find_netmask(ig, netmask2);
	CU_ASSERT(imask == NULL);

	/* restore the initial state */
	spdk_iscsi_init_grp_destroy(ig);
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

	suite = CU_add_suite("init_grp_suite", test_setup, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "create from config file cases",
			    create_from_config_file_cases) == NULL
		|| CU_add_test(suite, "create initiator group success case",
			       create_initiator_group_success_case) == NULL
		|| CU_add_test(suite, "find initiator group success case",
			       find_initiator_group_success_case) == NULL
		|| CU_add_test(suite, "create initiator group fail case",
			       create_initiator_group_fail_case) == NULL
		|| CU_add_test(suite, "add initiator name success case",
			       add_initiator_name_success_case) == NULL
		|| CU_add_test(suite, "add initiator name fail case",
			       add_initiator_name_fail_case) == NULL
		|| CU_add_test(suite, "delete all initiator names success case",
			       delete_all_initiator_names_success_case) == NULL
		|| CU_add_test(suite, "add initiator netmask success case",
			       add_netmask_success_case) == NULL
		|| CU_add_test(suite, "add initiator netmask fail case",
			       add_netmask_fail_case) == NULL
		|| CU_add_test(suite, "delete all initiator netmasks success case",
			       delete_all_netmasks_success_case) == NULL

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
