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

#include "spdk_cunit.h"

#include "common/lib/ut_multithread.c"
#include "common/lib/test_sock.c"

#include "../common.c"
#include "iscsi/portal_grp.c"
#include "unit/lib/json_mock.c"

#include "spdk_internal/thread.h"

DEFINE_STUB(spdk_iscsi_conn_construct, int,
	    (struct spdk_iscsi_portal *portal, struct spdk_sock *sock),
	    0);

struct spdk_iscsi_globals g_spdk_iscsi;

static int
test_setup(void)
{
	TAILQ_INIT(&g_spdk_iscsi.portal_head);
	TAILQ_INIT(&g_spdk_iscsi.pg_head);
	pthread_mutex_init(&g_spdk_iscsi.mutex, NULL);
	return 0;
}

static void
portal_create_ipv4_normal_case(void)
{
	struct spdk_iscsi_portal *p;

	const char *host = "192.168.2.0";
	const char *port = "3260";

	p = spdk_iscsi_portal_create(host, port);
	CU_ASSERT(p != NULL);

	spdk_iscsi_portal_destroy(p);
	CU_ASSERT(TAILQ_EMPTY(&g_spdk_iscsi.portal_head));
}

static void
portal_create_ipv6_normal_case(void)
{
	struct spdk_iscsi_portal *p;

	const char *host = "[2001:ad6:1234::]";
	const char *port = "3260";

	p = spdk_iscsi_portal_create(host, port);
	CU_ASSERT(p != NULL);

	spdk_iscsi_portal_destroy(p);
	CU_ASSERT(TAILQ_EMPTY(&g_spdk_iscsi.portal_head));
}

static void
portal_create_ipv4_wildcard_case(void)
{
	struct spdk_iscsi_portal *p;

	const char *host = "*";
	const char *port = "3260";

	p = spdk_iscsi_portal_create(host, port);
	CU_ASSERT(p != NULL);

	spdk_iscsi_portal_destroy(p);
	CU_ASSERT(TAILQ_EMPTY(&g_spdk_iscsi.portal_head));
}

static void
portal_create_ipv6_wildcard_case(void)
{
	struct spdk_iscsi_portal *p;

	const char *host = "[*]";
	const char *port = "3260";

	p = spdk_iscsi_portal_create(host, port);
	CU_ASSERT(p != NULL);

	spdk_iscsi_portal_destroy(p);
	CU_ASSERT(TAILQ_EMPTY(&g_spdk_iscsi.portal_head));
}

static void
portal_create_twice_case(void)
{
	struct spdk_iscsi_portal *p1, *p2;

	const char *host = "192.168.2.0";
	const char *port = "3260";

	p1 = spdk_iscsi_portal_create(host, port);
	CU_ASSERT(p1 != NULL);

	p2 = spdk_iscsi_portal_create(host, port);
	CU_ASSERT(p2 == NULL);

	spdk_iscsi_portal_destroy(p1);
	CU_ASSERT(TAILQ_EMPTY(&g_spdk_iscsi.portal_head));
}

static void
parse_portal_ipv4_normal_case(void)
{
	const char *string = "192.168.2.0:3260";
	const char *host_str = "192.168.2.0";
	const char *port_str = "3260";
	struct spdk_iscsi_portal *p = NULL;
	int rc;

	rc = iscsi_parse_portal(string, &p, 0);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(p != NULL);
	CU_ASSERT(strcmp(p->host, host_str) == 0);
	CU_ASSERT(strcmp(p->port, port_str) == 0);

	spdk_iscsi_portal_destroy(p);
	CU_ASSERT(TAILQ_EMPTY(&g_spdk_iscsi.portal_head));

}

static void
parse_portal_ipv6_normal_case(void)
{
	const char *string = "[2001:ad6:1234::]:3260";
	const char *host_str = "[2001:ad6:1234::]";
	const char *port_str = "3260";
	struct spdk_iscsi_portal *p = NULL;
	int rc;

	rc = iscsi_parse_portal(string, &p, 0);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(p != NULL);
	CU_ASSERT(strcmp(p->host, host_str) == 0);
	CU_ASSERT(strcmp(p->port, port_str) == 0);

	spdk_iscsi_portal_destroy(p);
	CU_ASSERT(TAILQ_EMPTY(&g_spdk_iscsi.portal_head));
}

static void
parse_portal_ipv4_skip_port_case(void)
{
	const char *string = "192.168.2.0";
	const char *host_str = "192.168.2.0";
	const char *port_str = "3260";
	struct spdk_iscsi_portal *p = NULL;
	int rc;

	rc = iscsi_parse_portal(string, &p, 0);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(p != NULL);
	CU_ASSERT(strcmp(p->host, host_str) == 0);
	CU_ASSERT(strcmp(p->port, port_str) == 0);

	spdk_iscsi_portal_destroy(p);
	CU_ASSERT(TAILQ_EMPTY(&g_spdk_iscsi.portal_head));
}

static void
parse_portal_ipv6_skip_port_case(void)
{
	const char *string = "[2001:ad6:1234::]";
	const char *host_str = "[2001:ad6:1234::]";
	const char *port_str = "3260";
	struct spdk_iscsi_portal *p = NULL;
	int rc;

	rc = iscsi_parse_portal(string, &p, 0);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(p != NULL);
	CU_ASSERT(strcmp(p->host, host_str) == 0);
	CU_ASSERT(strcmp(p->port, port_str) == 0);

	spdk_iscsi_portal_destroy(p);
	CU_ASSERT(TAILQ_EMPTY(&g_spdk_iscsi.portal_head));
}

static void
portal_grp_register_unregister_case(void)
{
	struct spdk_iscsi_portal *p;
	struct spdk_iscsi_portal_grp *pg1, *pg2;
	int rc;
	const char *host = "192.168.2.0";
	const char *port = "3260";

	pg1 = spdk_iscsi_portal_grp_create(1);
	CU_ASSERT(pg1 != NULL);

	p = spdk_iscsi_portal_create(host, port);
	CU_ASSERT(p != NULL);

	spdk_iscsi_portal_grp_add_portal(pg1, p);

	rc = spdk_iscsi_portal_grp_register(pg1);
	CU_ASSERT(rc == 0);

	pg2 = spdk_iscsi_portal_grp_unregister(1);
	CU_ASSERT(pg2 != NULL);
	CU_ASSERT(pg1 == pg2);

	CU_ASSERT(TAILQ_EMPTY(&g_spdk_iscsi.pg_head));

	spdk_iscsi_portal_grp_destroy(pg1);

	CU_ASSERT(TAILQ_EMPTY(&g_spdk_iscsi.portal_head));
}

static void
portal_grp_register_twice_case(void)
{
	struct spdk_iscsi_portal *p;
	struct spdk_iscsi_portal_grp *pg1, *pg2;
	int rc;
	const char *host = "192.168.2.0";
	const char *port = "3260";

	pg1 = spdk_iscsi_portal_grp_create(1);
	CU_ASSERT(pg1 != NULL);

	p = spdk_iscsi_portal_create(host, port);
	CU_ASSERT(p != NULL);

	spdk_iscsi_portal_grp_add_portal(pg1, p);

	rc = spdk_iscsi_portal_grp_register(pg1);
	CU_ASSERT(rc == 0);

	rc = spdk_iscsi_portal_grp_register(pg1);
	CU_ASSERT(rc != 0);

	pg2 = spdk_iscsi_portal_grp_unregister(1);
	CU_ASSERT(pg2 != NULL);
	CU_ASSERT(pg1 == pg2);

	CU_ASSERT(TAILQ_EMPTY(&g_spdk_iscsi.pg_head));

	spdk_iscsi_portal_grp_destroy(pg1);

	CU_ASSERT(TAILQ_EMPTY(&g_spdk_iscsi.portal_head));
}

static int
ut_poll_group_create(void *io_device, void *ctx_buf)
{
	return 0;
}

static void
ut_poll_group_destroy(void *io_device, void *ctx_buf)
{
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

	spdk_io_device_register(&g_spdk_iscsi, ut_poll_group_create, ut_poll_group_destroy,
				sizeof(struct spdk_iscsi_poll_group), "ut_portal_grp");

	/* internal of iscsi_create_portal_group */
	pg1 = spdk_iscsi_portal_grp_create(1);
	CU_ASSERT(pg1 != NULL);

	p = spdk_iscsi_portal_create(host, port);
	CU_ASSERT(p != NULL);

	spdk_iscsi_portal_grp_add_portal(pg1, p);

	MOCK_SET(spdk_sock_listen, &sock);
	rc = spdk_iscsi_portal_grp_open(pg1);
	CU_ASSERT(rc == 0);
	MOCK_CLEAR_P(spdk_sock_listen);

	rc = spdk_iscsi_portal_grp_register(pg1);
	CU_ASSERT(rc == 0);

	/* internal of delete_portal_group */
	pg2 = spdk_iscsi_portal_grp_unregister(1);
	CU_ASSERT(pg2 != NULL);
	CU_ASSERT(pg1 == pg2);

	spdk_iscsi_portal_grp_release(pg2);

	poll_thread(0);

	CU_ASSERT(TAILQ_EMPTY(&g_spdk_iscsi.portal_head));
	CU_ASSERT(TAILQ_EMPTY(&g_spdk_iscsi.pg_head));

	spdk_io_device_unregister(&g_spdk_iscsi, NULL);

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

	spdk_io_device_register(&g_spdk_iscsi, ut_poll_group_create, ut_poll_group_destroy,
				sizeof(struct spdk_iscsi_poll_group), "ut_portal_grp");

	/* internal of iscsi_create_portal_group related */
	pg1 = spdk_iscsi_portal_grp_create(1);
	CU_ASSERT(pg1 != NULL);

	p = spdk_iscsi_portal_create(host, port1);
	CU_ASSERT(p != NULL);

	spdk_iscsi_portal_grp_add_portal(pg1, p);

	MOCK_SET(spdk_sock_listen, &sock);
	rc = spdk_iscsi_portal_grp_open(pg1);
	CU_ASSERT(rc == 0);

	rc = spdk_iscsi_portal_grp_register(pg1);
	CU_ASSERT(rc == 0);

	/* internal of iscsi_create_portal_group related */
	pg2 = spdk_iscsi_portal_grp_create(2);
	CU_ASSERT(pg2 != NULL);

	p = spdk_iscsi_portal_create(host, port2);
	CU_ASSERT(p != NULL);

	spdk_iscsi_portal_grp_add_portal(pg2, p);

	rc = spdk_iscsi_portal_grp_open(pg2);
	CU_ASSERT(rc == 0);

	rc = spdk_iscsi_portal_grp_register(pg2);
	CU_ASSERT(rc == 0);

	/* internal of destroy_portal_group related */
	iscsi_portal_grp_close(pg1);
	iscsi_portal_grp_close(pg2);

	poll_thread(0);

	spdk_iscsi_portal_grps_destroy();

	CU_ASSERT(TAILQ_EMPTY(&g_spdk_iscsi.portal_head));
	CU_ASSERT(TAILQ_EMPTY(&g_spdk_iscsi.pg_head));

	MOCK_CLEAR_P(spdk_sock_listen);

	spdk_io_device_unregister(&g_spdk_iscsi, NULL);

	free_threads();
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
		CU_add_test(suite, "portal create ipv4 normal case",
			    portal_create_ipv4_normal_case) == NULL
		|| CU_add_test(suite, "portal create ipv6 normal case",
			       portal_create_ipv6_normal_case) == NULL
		|| CU_add_test(suite, "portal create ipv4 wildcard case",
			       portal_create_ipv4_wildcard_case) == NULL
		|| CU_add_test(suite, "portal create ipv6 wildcard case",
			       portal_create_ipv6_wildcard_case) == NULL
		|| CU_add_test(suite, "portal create twice case",
			       portal_create_twice_case) == NULL
		|| CU_add_test(suite, "parse portal ipv4 normal case",
			       parse_portal_ipv4_normal_case) == NULL
		|| CU_add_test(suite, "parse portal ipv6 normal case",
			       parse_portal_ipv6_normal_case) == NULL
		|| CU_add_test(suite, "parse portal ipv4 skip port case",
			       parse_portal_ipv4_skip_port_case) == NULL
		|| CU_add_test(suite, "parse portal ipv6 skip port case",
			       parse_portal_ipv6_skip_port_case) == NULL
		|| CU_add_test(suite, "portal group register/unregister case",
			       portal_grp_register_unregister_case) == NULL
		|| CU_add_test(suite, "portal group register twice case",
			       portal_grp_register_twice_case) == NULL
		|| CU_add_test(suite, "portal group add/delete case",
			       portal_grp_add_delete_case) == NULL
		|| CU_add_test(suite, "portal group add/delete twice case",
			       portal_grp_add_delete_twice_case) == NULL
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
