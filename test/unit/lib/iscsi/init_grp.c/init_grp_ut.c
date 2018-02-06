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

#include "spdk_cunit.h"
#include "CUnit/Basic.h"

#include "iscsi/init_grp.c"

SPDK_LOG_REGISTER_COMPONENT("iscsi", SPDK_LOG_ISCSI)

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
	struct spdk_iscsi_init_grp *ig, *tmp;
	int rc;

	ig = spdk_iscsi_init_grp_create(1);
	CU_ASSERT(ig != NULL);

	rc = spdk_iscsi_init_grp_register(ig);
	CU_ASSERT(rc == 0);

	ig = spdk_iscsi_init_grp_find_by_tag(1);
	CU_ASSERT(ig != NULL);

	tmp = spdk_iscsi_init_grp_unregister(1);
	CU_ASSERT(ig == tmp);
	spdk_iscsi_init_grp_destroy(ig);

	ig = spdk_iscsi_init_grp_find_by_tag(1);
	CU_ASSERT(ig == NULL);
}

static void
register_initiator_group_twice_case(void)
{
	struct spdk_iscsi_init_grp *ig, *tmp;
	int rc;

	ig = spdk_iscsi_init_grp_create(1);
	CU_ASSERT(ig != NULL);

	rc = spdk_iscsi_init_grp_register(ig);
	CU_ASSERT(rc == 0);

	rc = spdk_iscsi_init_grp_register(ig);
	CU_ASSERT(rc != 0);

	ig = spdk_iscsi_init_grp_find_by_tag(1);
	CU_ASSERT(ig != NULL);

	tmp = spdk_iscsi_init_grp_unregister(1);
	CU_ASSERT(tmp == ig);
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

static void
initiator_name_overwrite_all_to_any_case(void)
{
	int rc;
	struct spdk_iscsi_init_grp *ig;
	struct spdk_iscsi_initiator_name *iname;
	char *all = "ALL";
	char *any = "ANY";
	char *all_not = "!ALL";
	char *any_not = "!ANY";

	ig = spdk_iscsi_init_grp_create(1);
	CU_ASSERT(ig != NULL);

	rc = spdk_iscsi_init_grp_add_initiator(ig, all);
	CU_ASSERT(rc == 0);

	iname = spdk_iscsi_init_grp_find_initiator(ig, all);
	CU_ASSERT(iname == NULL);

	iname = spdk_iscsi_init_grp_find_initiator(ig, any);
	CU_ASSERT(iname != NULL);

	rc = spdk_iscsi_init_grp_delete_initiator(ig, any);
	CU_ASSERT(rc == 0);

	rc = spdk_iscsi_init_grp_add_initiator(ig, all_not);
	CU_ASSERT(rc == 0);

	iname = spdk_iscsi_init_grp_find_initiator(ig, all_not);
	CU_ASSERT(iname == NULL);

	iname = spdk_iscsi_init_grp_find_initiator(ig, any_not);
	CU_ASSERT(iname != NULL);

	rc = spdk_iscsi_init_grp_delete_initiator(ig, any_not);
	CU_ASSERT(rc == 0);

	spdk_iscsi_init_grp_destroy(ig);
}

static void
netmask_overwrite_all_to_any_case(void)
{
	int rc;
	struct spdk_iscsi_init_grp *ig;
	struct spdk_iscsi_initiator_netmask *imask;
	char *all = "ALL";
	char *any = "ANY";

	ig = spdk_iscsi_init_grp_create(1);
	CU_ASSERT(ig != NULL);

	rc = spdk_iscsi_init_grp_add_netmask(ig, all);
	CU_ASSERT(rc == 0);

	imask = spdk_iscsi_init_grp_find_netmask(ig, all);
	CU_ASSERT(imask == NULL);

	imask = spdk_iscsi_init_grp_find_netmask(ig, any);
	CU_ASSERT(imask != NULL);

	rc = spdk_iscsi_init_grp_delete_netmask(ig, any);
	CU_ASSERT(rc == 0);

	spdk_iscsi_init_grp_destroy(ig);
}

static void
add_delete_initiator_names_case(void)
{
	int rc, i;
	struct spdk_iscsi_init_grp *ig;
	struct spdk_iscsi_initiator_name *iname;
	char *names[3] = {"iqn.2018-02.spdk.io:0001", "iqn.2018-02.spdk.io:0002", "iqn.2018-02.spdk.io:0003"};

	ig = spdk_iscsi_init_grp_create(1);
	SPDK_CU_ASSERT_FATAL(ig != NULL);

	rc = spdk_iscsi_init_grp_add_initiators(ig, 3, names);
	CU_ASSERT(rc == 0);

	for (i = 0; i < 3; i++) {
		iname = spdk_iscsi_init_grp_find_initiator(ig, names[i]);
		CU_ASSERT(iname != NULL);
	}

	rc = spdk_iscsi_init_grp_delete_initiators(ig, 3, names);
	CU_ASSERT(rc == 0);

	if (ig != NULL) {
		CU_ASSERT(TAILQ_EMPTY(&ig->initiator_head));
	}

	spdk_iscsi_init_grp_destroy(ig);
}

static void
add_duplicated_initiator_names_case(void)
{
	int rc;
	struct spdk_iscsi_init_grp *ig;
	char *names[3] = {"iqn.2018-02.spdk.io:0001", "iqn.2018-02.spdk.io:0002", "iqn.2018-02.spdk.io:0001"};

	ig = spdk_iscsi_init_grp_create(1);
	SPDK_CU_ASSERT_FATAL(ig != NULL);

	rc = spdk_iscsi_init_grp_add_initiators(ig, 3, names);
	CU_ASSERT(rc != 0);

	if (ig != NULL) {
		CU_ASSERT(TAILQ_EMPTY(&ig->initiator_head));
	}

	spdk_iscsi_init_grp_destroy(ig);
}

static void
delete_nonexisting_initiator_names_case(void)
{
	int rc, i;
	struct spdk_iscsi_init_grp *ig;
	struct spdk_iscsi_initiator_name *iname;
	char *names1[3] = {"iqn.2018-02.spdk.io:0001", "iqn.2018-02.spdk.io:0002", "iqn.2018-02.spdk.io:0003"};
	char *names2[3] = {"iqn.2018-02.spdk.io:0001", "iqn.2018-02.spdk.io:0002", "iqn.2018-02.spdk.io:0004"};

	ig = spdk_iscsi_init_grp_create(1);
	SPDK_CU_ASSERT_FATAL(ig != NULL);

	rc = spdk_iscsi_init_grp_add_initiators(ig, 3, names1);
	CU_ASSERT(rc == 0);

	for (i = 0; i < 3; i++) {
		iname = spdk_iscsi_init_grp_find_initiator(ig, names1[i]);
		CU_ASSERT(iname != NULL);
	}

	rc = spdk_iscsi_init_grp_delete_initiators(ig, 3, names2);
	CU_ASSERT(rc != 0);

	for (i = 0; i < 3; i++) {
		iname = spdk_iscsi_init_grp_find_initiator(ig, names1[i]);
		CU_ASSERT(iname != NULL);
	}

	rc = spdk_iscsi_init_grp_delete_initiators(ig, 3, names1);
	CU_ASSERT(rc == 0);

	if (ig != NULL) {
		CU_ASSERT(TAILQ_EMPTY(&ig->initiator_head));
	}

	spdk_iscsi_init_grp_destroy(ig);
}

static void
add_delete_netmasks_case(void)
{
	int rc, i;
	struct spdk_iscsi_init_grp *ig;
	struct spdk_iscsi_initiator_netmask *netmask;
	char *netmasks[3] = {"192.168.2.0", "192.168.2.1", "192.168.2.2"};

	ig = spdk_iscsi_init_grp_create(1);
	SPDK_CU_ASSERT_FATAL(ig != NULL);

	rc = spdk_iscsi_init_grp_add_netmasks(ig, 3, netmasks);
	CU_ASSERT(rc == 0);

	for (i = 0; i < 3; i++) {
		netmask = spdk_iscsi_init_grp_find_netmask(ig, netmasks[i]);
		CU_ASSERT(netmask != NULL);
	}

	rc = spdk_iscsi_init_grp_delete_netmasks(ig, 3, netmasks);
	CU_ASSERT(rc == 0);

	if (ig != NULL) {
		CU_ASSERT(TAILQ_EMPTY(&ig->netmask_head));
	}

	spdk_iscsi_init_grp_destroy(ig);
}

static void
add_duplicated_netmasks_case(void)
{
	int rc;
	struct spdk_iscsi_init_grp *ig;
	char *netmasks[3] = {"192.168.2.0", "192.168.2.1", "192.168.2.0"};

	ig = spdk_iscsi_init_grp_create(1);
	SPDK_CU_ASSERT_FATAL(ig != NULL);

	rc = spdk_iscsi_init_grp_add_netmasks(ig, 3, netmasks);
	CU_ASSERT(rc != 0);

	if (ig != NULL) {
		CU_ASSERT(TAILQ_EMPTY(&ig->netmask_head));
	}

	spdk_iscsi_init_grp_destroy(ig);
}

static void
delete_nonexisting_netmasks_case(void)
{
	int rc, i;
	struct spdk_iscsi_init_grp *ig;
	struct spdk_iscsi_initiator_netmask *netmask;
	char *netmasks1[3] = {"192.168.2.0", "192.168.2.1", "192.168.2.2"};
	char *netmasks2[3] = {"192.168.2.0", "192.168.2.1", "192.168.2.3"};

	ig = spdk_iscsi_init_grp_create(1);
	SPDK_CU_ASSERT_FATAL(ig != NULL);

	rc = spdk_iscsi_init_grp_add_netmasks(ig, 3, netmasks1);
	CU_ASSERT(rc == 0);

	for (i = 0; i < 3; i++) {
		netmask = spdk_iscsi_init_grp_find_netmask(ig, netmasks1[i]);
		CU_ASSERT(netmask != NULL);
	}

	rc = spdk_iscsi_init_grp_delete_netmasks(ig, 3, netmasks2);
	CU_ASSERT(rc != 0);

	for (i = 0; i < 3; i++) {
		netmask = spdk_iscsi_init_grp_find_netmask(ig, netmasks1[i]);
		CU_ASSERT(netmask != NULL);
	}

	rc = spdk_iscsi_init_grp_delete_netmasks(ig, 3, netmasks1);
	CU_ASSERT(rc == 0);

	if (ig != NULL) {
		CU_ASSERT(TAILQ_EMPTY(&ig->netmask_head));
	}

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
		|| CU_add_test(suite, "register initiator group twice case",
			       register_initiator_group_twice_case) == NULL
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
		|| CU_add_test(suite, "overwrite all to any for name case",
			       initiator_name_overwrite_all_to_any_case) == NULL
		|| CU_add_test(suite, "overwrite all to any for netmask case",
			       netmask_overwrite_all_to_any_case) == NULL
		|| CU_add_test(suite, "add/delete initiator names case",
			       add_delete_initiator_names_case) == NULL
		|| CU_add_test(suite, "add duplicated initiator names case",
			       add_duplicated_initiator_names_case) == NULL
		|| CU_add_test(suite, "delete nonexisting initiator names case",
			       delete_nonexisting_initiator_names_case) == NULL
		|| CU_add_test(suite, "add/delete netmasks case",
			       add_delete_netmasks_case) == NULL
		|| CU_add_test(suite, "add duplicated netmasks case",
			       add_duplicated_netmasks_case) == NULL
		|| CU_add_test(suite, "delete nonexisting netmasks case",
			       delete_nonexisting_netmasks_case) == NULL
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
