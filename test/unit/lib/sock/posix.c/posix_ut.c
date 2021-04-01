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
#include "spdk/util.h"

#include "spdk_internal/mock.h"

#include "spdk_cunit.h"

#include "common/lib/test_env.c"
#include "sock/posix/posix.c"

DEFINE_STUB(spdk_sock_map_insert, int, (struct spdk_sock_map *map, int placement_id,
					struct spdk_sock_group_impl *group), 0);
DEFINE_STUB_V(spdk_sock_map_release, (struct spdk_sock_map *map, int placement_id));
DEFINE_STUB(spdk_sock_map_lookup, int, (struct spdk_sock_map *map, int placement_id,
					struct spdk_sock_group_impl **group), 0);
DEFINE_STUB(spdk_sock_map_find_free, int, (struct spdk_sock_map *map), -1);
DEFINE_STUB_V(spdk_sock_map_cleanup, (struct spdk_sock_map *map));

DEFINE_STUB_V(spdk_net_impl_register, (struct spdk_net_impl *impl, int priority));
DEFINE_STUB(spdk_sock_close, int, (struct spdk_sock **s), 0);

static void
_req_cb(void *cb_arg, int len)
{
	*(bool *)cb_arg = true;
	CU_ASSERT(len == 0);
}

static void
flush(void)
{
	struct spdk_posix_sock_group_impl group = {};
	struct spdk_posix_sock psock = {};
	struct spdk_sock *sock = &psock.base;
	struct spdk_sock_request *req1, *req2;
	bool cb_arg1, cb_arg2;
	int rc;

	/* Set up data structures */
	TAILQ_INIT(&sock->queued_reqs);
	TAILQ_INIT(&sock->pending_reqs);
	sock->group_impl = &group.base;

	req1 = calloc(1, sizeof(struct spdk_sock_request) + 2 * sizeof(struct iovec));
	SPDK_CU_ASSERT_FATAL(req1 != NULL);
	SPDK_SOCK_REQUEST_IOV(req1, 0)->iov_base = (void *)100;
	SPDK_SOCK_REQUEST_IOV(req1, 0)->iov_len = 32;
	SPDK_SOCK_REQUEST_IOV(req1, 1)->iov_base = (void *)200;
	SPDK_SOCK_REQUEST_IOV(req1, 1)->iov_len = 32;
	req1->iovcnt = 2;
	req1->cb_fn = _req_cb;
	req1->cb_arg = &cb_arg1;

	req2 = calloc(1, sizeof(struct spdk_sock_request) + 2 * sizeof(struct iovec));
	SPDK_CU_ASSERT_FATAL(req2 != NULL);
	SPDK_SOCK_REQUEST_IOV(req2, 0)->iov_base = (void *)100;
	SPDK_SOCK_REQUEST_IOV(req2, 0)->iov_len = 32;
	SPDK_SOCK_REQUEST_IOV(req2, 1)->iov_base = (void *)200;
	SPDK_SOCK_REQUEST_IOV(req2, 1)->iov_len = 32;
	req2->iovcnt = 2;
	req2->cb_fn = _req_cb;
	req2->cb_arg = &cb_arg2;

	/* Simple test - a request with a 2 element iovec
	 * that gets submitted in a single sendmsg. */
	spdk_sock_request_queue(sock, req1);
	MOCK_SET(sendmsg, 64);
	cb_arg1 = false;
	rc = _sock_flush(sock);
	CU_ASSERT(rc == 0);
	CU_ASSERT(cb_arg1 == true);
	CU_ASSERT(TAILQ_EMPTY(&sock->queued_reqs));

	/* Two requests, where both can fully send. */
	spdk_sock_request_queue(sock, req1);
	spdk_sock_request_queue(sock, req2);
	MOCK_SET(sendmsg, 128);
	cb_arg1 = false;
	cb_arg2 = false;
	rc = _sock_flush(sock);
	CU_ASSERT(rc == 0);
	CU_ASSERT(cb_arg1 == true);
	CU_ASSERT(cb_arg2 == true);
	CU_ASSERT(TAILQ_EMPTY(&sock->queued_reqs));

	/* Two requests. Only first one can send */
	spdk_sock_request_queue(sock, req1);
	spdk_sock_request_queue(sock, req2);
	MOCK_SET(sendmsg, 64);
	cb_arg1 = false;
	cb_arg2 = false;
	rc = _sock_flush(sock);
	CU_ASSERT(rc == 0);
	CU_ASSERT(cb_arg1 == true);
	CU_ASSERT(cb_arg2 == false);
	CU_ASSERT(TAILQ_FIRST(&sock->queued_reqs) == req2);
	TAILQ_REMOVE(&sock->queued_reqs, req2, internal.link);
	CU_ASSERT(TAILQ_EMPTY(&sock->queued_reqs));

	/* One request. Partial send. */
	spdk_sock_request_queue(sock, req1);
	MOCK_SET(sendmsg, 10);
	cb_arg1 = false;
	rc = _sock_flush(sock);
	CU_ASSERT(rc == 0);
	CU_ASSERT(cb_arg1 == false);
	CU_ASSERT(TAILQ_FIRST(&sock->queued_reqs) == req1);

	/* Do a second flush that partial sends again. */
	MOCK_SET(sendmsg, 24);
	cb_arg1 = false;
	rc = _sock_flush(sock);
	CU_ASSERT(rc == 0);
	CU_ASSERT(cb_arg1 == false);
	CU_ASSERT(TAILQ_FIRST(&sock->queued_reqs) == req1);

	/* Flush the rest of the data */
	MOCK_SET(sendmsg, 30);
	cb_arg1 = false;
	rc = _sock_flush(sock);
	CU_ASSERT(rc == 0);
	CU_ASSERT(cb_arg1 == true);
	CU_ASSERT(TAILQ_EMPTY(&sock->queued_reqs));

	free(req1);
	free(req2);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("posix", NULL, NULL);

	CU_ADD_TEST(suite, flush);

	CU_basic_set_mode(CU_BRM_VERBOSE);

	CU_basic_run_tests();

	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
