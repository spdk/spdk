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

#include "common/lib/ut_multithread.c"
#include "common/lib/test_sock.c"

#include "../common.c"
#include "iscsi/portal_grp.c"
#include "unit/lib/json_mock.c"

DEFINE_STUB(iscsi_conn_construct, int,
	    (struct spdk_iscsi_portal *portal, struct spdk_sock *sock),
	    0);
DEFINE_STUB(iscsi_check_chap_params, bool,
	    (bool disable, bool require, bool mutual, int group),
	    false);

struct spdk_iscsi_globals g_iscsi;

static int
test_setup(void)
{
	TAILQ_INIT(&g_iscsi.portal_head);
	TAILQ_INIT(&g_iscsi.pg_head);
	pthread_mutex_init(&g_iscsi.mutex, NULL);
	return 0;
}

static void
portal_create_ipv4_normal_case(void)
{
	struct spdk_iscsi_portal *p;

	const char *host = "192.168.2.0";
	const char *port = "3260";

	p = iscsi_portal_create(host, port);
	CU_ASSERT(p != NULL);

	iscsi_portal_destroy(p);
	CU_ASSERT(TAILQ_EMPTY(&g_iscsi.portal_head));
}

static void
portal_create_ipv6_normal_case(void)
{
	struct spdk_iscsi_portal *p;

	const char *host = "[2001:ad6:1234::]";
	const char *port = "3260";

	p = iscsi_portal_create(host, port);
	CU_ASSERT(p != NULL);

	iscsi_portal_destroy(p);
	CU_ASSERT(TAILQ_EMPTY(&g_iscsi.portal_head));
}

static void
portal_create_ipv4_wildcard_case(void)
{
	struct spdk_iscsi_portal *p;

	const char *host = "*";
	const char *port = "3260";

	p = iscsi_portal_create(host, port);
	CU_ASSERT(p != NULL);

	iscsi_portal_destroy(p);
	CU_ASSERT(TAILQ_EMPTY(&g_iscsi.portal_head));
}

static void
portal_create_ipv6_wildcard_case(void)
{
	struct spdk_iscsi_portal *p;

	const char *host = "[*]";
	const char *port = "3260";

	p = iscsi_portal_create(host, port);
	CU_ASSERT(p != NULL);

	iscsi_portal_destroy(p);
	CU_ASSERT(TAILQ_EMPTY(&g_iscsi.portal_head));
}

static void
portal_create_twice_case(void)
{
	struct spdk_iscsi_portal *p1, *p2;

	const char *host = "192.168.2.0";
	const char *port = "3260";

	p1 = iscsi_portal_create(host, port);
	CU_ASSERT(p1 != NULL);

	p2 = iscsi_portal_create(host, port);
	CU_ASSERT(p2 == NULL);

	iscsi_portal_destroy(p1);
	CU_ASSERT(TAILQ_EMPTY(&g_iscsi.portal_head));
}

static void
portal_grp_register_unregister_case(void)
{
	struct spdk_iscsi_portal *p;
	struct spdk_iscsi_portal_grp *pg1, *pg2;
	int rc;
	const char *host = "192.168.2.0";
	const char *port = "3260";

	pg1 = iscsi_portal_grp_create(1, false);
	CU_ASSERT(pg1 != NULL);

	p = iscsi_portal_create(host, port);
	CU_ASSERT(p != NULL);

	iscsi_portal_grp_add_portal(pg1, p);

	rc = iscsi_portal_grp_register(pg1);
	CU_ASSERT(rc == 0);

	pg2 = iscsi_portal_grp_unregister(1);
	CU_ASSERT(pg2 != NULL);
	CU_ASSERT(pg1 == pg2);

	CU_ASSERT(TAILQ_EMPTY(&g_iscsi.pg_head));

	iscsi_portal_grp_destroy(pg1);

	CU_ASSERT(TAILQ_EMPTY(&g_iscsi.portal_head));
}

static void
portal_grp_register_twice_case(void)
{
	struct spdk_iscsi_portal *p;
	struct spdk_iscsi_portal_grp *pg1, *pg2;
	int rc;
	const char *host = "192.168.2.0";
	const char *port = "3260";

	pg1 = iscsi_portal_grp_create(1, false);
	CU_ASSERT(pg1 != NULL);

	p = iscsi_portal_create(host, port);
	CU_ASSERT(p != NULL);

	iscsi_portal_grp_add_portal(pg1, p);

	rc = iscsi_portal_grp_register(pg1);
	CU_ASSERT(rc == 0);

	rc = iscsi_portal_grp_register(pg1);
	CU_ASSERT(rc != 0);

	pg2 = iscsi_portal_grp_unregister(1);
	CU_ASSERT(pg2 != NULL);
	CU_ASSERT(pg1 == pg2);

	CU_ASSERT(TAILQ_EMPTY(&g_iscsi.pg_head));

	iscsi_portal_grp_destroy(pg1);

	CU_ASSERT(TAILQ_EMPTY(&g_iscsi.portal_head));
}

static void
portal_grp_add_delete_case(void)
{
	struct spdk_sock sock = {};
	struct spdk_iscsi_portal_grp *pg1, *pg2;
	struct spdk_iscsi_portal *p;
	int rc;

	const char *host = "192.168.2.0";
	const char *port = "3260";

	allocate_threads(1);
	set_thread(0);

	/* internal of iscsi_create_portal_group */
	pg1 = iscsi_portal_grp_create(1, false);
	CU_ASSERT(pg1 != NULL);

	p = iscsi_portal_create(host, port);
	CU_ASSERT(p != NULL);

	iscsi_portal_grp_add_portal(pg1, p);

	MOCK_SET(spdk_sock_listen, &sock);
	rc = iscsi_portal_grp_open(pg1, false);
	CU_ASSERT(rc == 0);
	MOCK_CLEAR_P(spdk_sock_listen);

	rc = iscsi_portal_grp_register(pg1);
	CU_ASSERT(rc == 0);

	/* internal of delete_portal_group */
	pg2 = iscsi_portal_grp_unregister(1);
	CU_ASSERT(pg2 != NULL);
	CU_ASSERT(pg1 == pg2);

	iscsi_portal_grp_release(pg2);

	poll_thread(0);

	CU_ASSERT(TAILQ_EMPTY(&g_iscsi.portal_head));
	CU_ASSERT(TAILQ_EMPTY(&g_iscsi.pg_head));

	free_threads();
}

static void
portal_grp_add_delete_twice_case(void)
{
	struct spdk_sock sock = {};
	struct spdk_iscsi_portal_grp *pg1, *pg2;
	struct spdk_iscsi_portal *p;
	int rc;

	const char *host = "192.168.2.0";
	const char *port1 = "3260", *port2 = "3261";

	allocate_threads(1);
	set_thread(0);

	/* internal of iscsi_create_portal_group related */
	pg1 = iscsi_portal_grp_create(1, false);
	CU_ASSERT(pg1 != NULL);

	p = iscsi_portal_create(host, port1);
	CU_ASSERT(p != NULL);

	iscsi_portal_grp_add_portal(pg1, p);

	MOCK_SET(spdk_sock_listen, &sock);
	rc = iscsi_portal_grp_open(pg1, false);
	CU_ASSERT(rc == 0);

	rc = iscsi_portal_grp_register(pg1);
	CU_ASSERT(rc == 0);

	/* internal of iscsi_create_portal_group related */
	pg2 = iscsi_portal_grp_create(2, false);
	CU_ASSERT(pg2 != NULL);

	p = iscsi_portal_create(host, port2);
	CU_ASSERT(p != NULL);

	iscsi_portal_grp_add_portal(pg2, p);

	rc = iscsi_portal_grp_open(pg2, false);
	CU_ASSERT(rc == 0);

	rc = iscsi_portal_grp_register(pg2);
	CU_ASSERT(rc == 0);

	/* internal of destroy_portal_group related */
	iscsi_portal_grp_close(pg1);
	iscsi_portal_grp_close(pg2);

	poll_thread(0);

	iscsi_portal_grps_destroy();

	CU_ASSERT(TAILQ_EMPTY(&g_iscsi.portal_head));
	CU_ASSERT(TAILQ_EMPTY(&g_iscsi.pg_head));

	MOCK_CLEAR_P(spdk_sock_listen);

	free_threads();
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("portal_grp_suite", test_setup, NULL);

	CU_ADD_TEST(suite, portal_create_ipv4_normal_case);
	CU_ADD_TEST(suite, portal_create_ipv6_normal_case);
	CU_ADD_TEST(suite, portal_create_ipv4_wildcard_case);
	CU_ADD_TEST(suite, portal_create_ipv6_wildcard_case);
	CU_ADD_TEST(suite, portal_create_twice_case);
	CU_ADD_TEST(suite, portal_grp_register_unregister_case);
	CU_ADD_TEST(suite, portal_grp_register_twice_case);
	CU_ADD_TEST(suite, portal_grp_add_delete_case);
	CU_ADD_TEST(suite, portal_grp_add_delete_twice_case);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
