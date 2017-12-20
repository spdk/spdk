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
#include "spdk/event.h"

#include "CUnit/Basic.h"

#include "../common.c"
#include "iscsi/portal_grp.c"

struct spdk_iscsi_globals g_spdk_iscsi;

static int
test_setup(void)
{
	TAILQ_INIT(&g_spdk_iscsi.portal_head);
	return 0;
}

static void
portal_create_success_case1(void)
{
	struct spdk_iscsi_portal *p;

	const char *host = "192.168.2.0";
	const char *port = "3260";
	const char *cpumask = "1";

	p = spdk_iscsi_portal_create(host, port, cpumask);
	CU_ASSERT(p != NULL);

	spdk_iscsi_portal_destroy(p);
	CU_ASSERT(TAILQ_EMPTY(&g_spdk_iscsi.portal_head));
}

static void
portal_create_success_case2(void)
{
	struct spdk_iscsi_portal *p;

	const char *host = "[*]";
	const char *port = "3260";
	const char *cpumask = "1";

	p = spdk_iscsi_portal_create(host, port, cpumask);
	CU_ASSERT(p != NULL);

	spdk_iscsi_portal_destroy(p);
	CU_ASSERT(TAILQ_EMPTY(&g_spdk_iscsi.portal_head));
}

static void
portal_create_success_case3(void)
{
	struct spdk_iscsi_portal *p;

	const char *host = "*";
	const char *port = "3260";
	const char *cpumask = "1";

	p = spdk_iscsi_portal_create(host, port, cpumask);
	CU_ASSERT(p != NULL);

	spdk_iscsi_portal_destroy(p);
	CU_ASSERT(TAILQ_EMPTY(&g_spdk_iscsi.portal_head));
}

static void
portal_create_success_case4(void)
{
	struct spdk_iscsi_portal *p;

	const char *host = "192.168.2.0";
	const char *port = "3260";
	const char *cpumask = NULL;

	p = spdk_iscsi_portal_create(host, port, cpumask);
	CU_ASSERT(p != NULL);

	spdk_iscsi_portal_destroy(p);
	CU_ASSERT(TAILQ_EMPTY(&g_spdk_iscsi.portal_head));
}

static void
portal_create_failure_case1(void)
{
	struct spdk_iscsi_portal *p;

	const char *host = "192.168.2.0";
	const char *port = "3260";
	const char *cpumask = "0";

	p = spdk_iscsi_portal_create(host, port, cpumask);
	CU_ASSERT(p == NULL);
}

static void
portal_create_failure_case2(void)
{
	struct spdk_iscsi_portal *p1, *p2;

	const char *host = "192.168.2.0";
	const char *port = "3260";
	const char *cpumask = "1";

	p1 = spdk_iscsi_portal_create(host, port, cpumask);
	CU_ASSERT(p1 != NULL);

	p2 = spdk_iscsi_portal_create(host, port, cpumask);
	CU_ASSERT(p2 == NULL);

	spdk_iscsi_portal_destroy(p1);
	CU_ASSERT(TAILQ_EMPTY(&g_spdk_iscsi.portal_head));
}

static void
portal_parse_configline_success_case1(void)
{
	const char *string = "192.168.2.0:3260@1";
	const char *host = "192.168.2.0";
	const char *port = "3260";
	const char *cpumask = "1";
	struct parse_portal parse;
	int rc;

	rc = spdk_iscsi_portal_parse_configline(string, &parse);
	CU_ASSERT(rc == 0);
	CU_ASSERT(strcmp(parse.host, host) == 0);
	CU_ASSERT(strcmp(parse.port, port) == 0);
	CU_ASSERT(strcmp(parse.cpumask, cpumask) == 0);
}

static void
portal_parse_configline_success_case2(void)
{
	const char *string = "[2001:ad6:1234::]:3260@1";
	const char *host = "[2001:ad6:1234::]";
	const char *port = "3260";
	const char *cpumask = "1";
	struct parse_portal parse;
	int rc;

	rc = spdk_iscsi_portal_parse_configline(string, &parse);
	CU_ASSERT(rc == 0);
	CU_ASSERT(strcmp(parse.host, host) == 0);
	CU_ASSERT(strcmp(parse.port, port) == 0);
	CU_ASSERT(strcmp(parse.cpumask, cpumask) == 0);
}

static void
portal_parse_configline_success_case3(void)
{
	const char *string = "192.168.2.0:3260";
	const char *host = "192.168.2.0";
	const char *port = "3260";
	struct parse_portal parse;
	int rc;

	rc = spdk_iscsi_portal_parse_configline(string, &parse);
	CU_ASSERT(rc == 0);
	CU_ASSERT(strcmp(parse.host, host) == 0);
	CU_ASSERT(strcmp(parse.port, port) == 0);
	CU_ASSERT(parse.cpumask == NULL);
}

static void
portal_parse_configline_success_case4(void)
{
	const char *string = "[2001:ad6:1234::]:3260";
	const char *host = "[2001:ad6:1234::]";
	const char *port = "3260";
	struct parse_portal parse;
	int rc;

	rc = spdk_iscsi_portal_parse_configline(string, &parse);
	CU_ASSERT(rc == 0);
	CU_ASSERT(strcmp(parse.host, host) == 0);
	CU_ASSERT(strcmp(parse.port, port) == 0);
	CU_ASSERT(parse.cpumask == NULL);
}

static void
portal_parse_configline_success_case5(void)
{
	const char *string = "192.168.2.0";
	const char *host = "192.168.2.0";
	const char *port = "3260";
	struct parse_portal parse;
	int rc;

	rc = spdk_iscsi_portal_parse_configline(string, &parse);
	CU_ASSERT(rc == 0);
	CU_ASSERT(strcmp(parse.host, host) == 0);
	CU_ASSERT(strcmp(parse.port, port) == 0);
	CU_ASSERT(parse.cpumask == NULL);
}

static void
portal_parse_configline_success_case6(void)
{
	const char *string = "[2001:ad6:1234::]";
	const char *host = "[2001:ad6:1234::]";
	const char *port = "3260";
	struct parse_portal parse;
	int rc;

	rc = spdk_iscsi_portal_parse_configline(string, &parse);
	CU_ASSERT(rc == 0);
	CU_ASSERT(strcmp(parse.host, host) == 0);
	CU_ASSERT(strcmp(parse.port, port) == 0);
	CU_ASSERT(parse.cpumask == NULL);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("portal_grp_suite", test_setup, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "portal create success case 1",
			    portal_create_success_case1) == NULL
		|| CU_add_test(suite, "portal create success case 2",
			       portal_create_success_case2) == NULL
		|| CU_add_test(suite, "portal create success case 3",
			       portal_create_success_case3) == NULL
		|| CU_add_test(suite, "portal create success case 4",
			       portal_create_success_case4) == NULL
		|| CU_add_test(suite, "portal create failure case 1",
			       portal_create_failure_case1) == NULL
		|| CU_add_test(suite, "portal create failure case 2",
			       portal_create_failure_case2) == NULL
		|| CU_add_test(suite, "portal parse configline success case1",
			       portal_parse_configline_success_case1) == NULL
		|| CU_add_test(suite, "portal parse configline success case2",
			       portal_parse_configline_success_case2) == NULL
		|| CU_add_test(suite, "portal parse configline success case3",
			       portal_parse_configline_success_case3) == NULL
		|| CU_add_test(suite, "portal parse configline success case4",
			       portal_parse_configline_success_case4) == NULL
		|| CU_add_test(suite, "portal parse configline success case5",
			       portal_parse_configline_success_case5) == NULL
		|| CU_add_test(suite, "portal parse configline success case6",
			       portal_parse_configline_success_case6) == NULL
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
