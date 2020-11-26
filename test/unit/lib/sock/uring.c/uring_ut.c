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

#include "sock/uring/uring.c"

DEFINE_STUB_V(spdk_net_impl_register, (struct spdk_net_impl *impl, int priority));
DEFINE_STUB(spdk_sock_close, int, (struct spdk_sock **s), 0);
DEFINE_STUB(__io_uring_get_cqe, int, (struct io_uring *ring, struct io_uring_cqe **cqe_ptr,
				      unsigned submit,
				      unsigned wait_nr, sigset_t *sigmask), 0);
DEFINE_STUB(io_uring_submit, int, (struct io_uring *ring), 0);
DEFINE_STUB(io_uring_get_sqe, struct io_uring_sqe *, (struct io_uring *ring), 0);
DEFINE_STUB(io_uring_queue_init, int, (unsigned entries, struct io_uring *ring, unsigned flags), 0);
DEFINE_STUB_V(io_uring_queue_exit, (struct io_uring *ring));

static void
_req_cb(void *cb_arg, int len)
{
	*(bool *)cb_arg = true;
	CU_ASSERT(len == 0);
}

static void
flush_client(void)
{
	struct spdk_uring_sock_group_impl group = {};
	struct spdk_uring_sock usock = {};
	struct spdk_sock *sock = &usock.base;
	struct spdk_sock_request *req1, *req2;
	bool cb_arg1, cb_arg2;
	int rc;

	/* Set up data structures */
	TAILQ_INIT(&sock->queued_reqs);
	TAILQ_INIT(&sock->pending_reqs);
	sock->group_impl = &group.base;

	req1 = calloc(1, sizeof(struct spdk_sock_request) + 3 * sizeof(struct iovec));
	SPDK_CU_ASSERT_FATAL(req1 != NULL);
	SPDK_SOCK_REQUEST_IOV(req1, 0)->iov_base = (void *)100;
	SPDK_SOCK_REQUEST_IOV(req1, 0)->iov_len = 64;
	SPDK_SOCK_REQUEST_IOV(req1, 1)->iov_base = (void *)200;
	SPDK_SOCK_REQUEST_IOV(req1, 1)->iov_len = 64;
	SPDK_SOCK_REQUEST_IOV(req1, 2)->iov_base = (void *)300;
	SPDK_SOCK_REQUEST_IOV(req1, 2)->iov_len = 64;
	req1->iovcnt = 3;
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

	/* Simple test - a request with a 3 element iovec
	 * that gets submitted in a single sendmsg. */
	spdk_sock_request_queue(sock, req1);
	MOCK_SET(sendmsg, 192);
	cb_arg1 = false;
	rc = _sock_flush_client(sock);
	CU_ASSERT(rc == 0);
	CU_ASSERT(cb_arg1 == true);
	CU_ASSERT(TAILQ_EMPTY(&sock->queued_reqs));

	/* Two requests, where both can fully send. */
	spdk_sock_request_queue(sock, req1);
	spdk_sock_request_queue(sock, req2);
	MOCK_SET(sendmsg, 256);
	cb_arg1 = false;
	cb_arg2 = false;
	rc = _sock_flush_client(sock);
	CU_ASSERT(rc == 0);
	CU_ASSERT(cb_arg1 == true);
	CU_ASSERT(cb_arg2 == true);
	CU_ASSERT(TAILQ_EMPTY(&sock->queued_reqs));

	/* Two requests. Only first one can send */
	spdk_sock_request_queue(sock, req1);
	spdk_sock_request_queue(sock, req2);
	MOCK_SET(sendmsg, 192);
	cb_arg1 = false;
	cb_arg2 = false;
	rc = _sock_flush_client(sock);
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
	rc = _sock_flush_client(sock);
	CU_ASSERT(rc == 0);
	CU_ASSERT(cb_arg1 == false);
	CU_ASSERT(TAILQ_FIRST(&sock->queued_reqs) == req1);

	/* Do a second flush that partial sends again. */
	MOCK_SET(sendmsg, 52);
	cb_arg1 = false;
	rc = _sock_flush_client(sock);
	CU_ASSERT(rc == 0);
	CU_ASSERT(cb_arg1 == false);
	CU_ASSERT(TAILQ_FIRST(&sock->queued_reqs) == req1);

	/* Flush the rest of the data */
	MOCK_SET(sendmsg, 130);
	cb_arg1 = false;
	rc = _sock_flush_client(sock);
	CU_ASSERT(rc == 0);
	CU_ASSERT(cb_arg1 == true);
	CU_ASSERT(TAILQ_EMPTY(&sock->queued_reqs));

	free(req1);
	free(req2);
}

static void
flush_server(void)
{
	struct spdk_uring_sock_group_impl group = {};
	struct spdk_uring_sock usock = {};
	struct spdk_sock *sock = &usock.base;
	struct spdk_sock_request *req1, *req2;
	bool cb_arg1, cb_arg2;
	int rc;

	/* Set up data structures */
	TAILQ_INIT(&sock->queued_reqs);
	TAILQ_INIT(&sock->pending_reqs);
	sock->group_impl = &group.base;
	usock.write_task.sock = &usock;
	usock.group = &group;

	req1 = calloc(1, sizeof(struct spdk_sock_request) + 2 * sizeof(struct iovec));
	SPDK_CU_ASSERT_FATAL(req1 != NULL);
	SPDK_SOCK_REQUEST_IOV(req1, 0)->iov_base = (void *)100;
	SPDK_SOCK_REQUEST_IOV(req1, 0)->iov_len = 64;
	SPDK_SOCK_REQUEST_IOV(req1, 1)->iov_base = (void *)200;
	SPDK_SOCK_REQUEST_IOV(req1, 1)->iov_len = 64;
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

	/* we should not call _sock_flush directly, since it will finally
	 * call liburing related funtions  */

	/* Simple test - a request with a 2 element iovec
	 * that is fully completed. */
	spdk_sock_request_queue(sock, req1);
	cb_arg1 = false;
	rc = spdk_sock_prep_reqs(sock, usock.write_task.iovs, 0, NULL);
	CU_ASSERT(rc == 2);
	sock_complete_reqs(sock, 128);
	CU_ASSERT(cb_arg1 == true);
	CU_ASSERT(TAILQ_EMPTY(&sock->queued_reqs));

	/* Two requests, where both can be fully completed. */
	spdk_sock_request_queue(sock, req1);
	spdk_sock_request_queue(sock, req2);
	cb_arg1 = false;
	cb_arg2 = false;
	rc = spdk_sock_prep_reqs(sock, usock.write_task.iovs, 0, NULL);
	CU_ASSERT(rc == 4);
	sock_complete_reqs(sock, 192);
	CU_ASSERT(cb_arg1 == true);
	CU_ASSERT(cb_arg2 == true);
	CU_ASSERT(TAILQ_EMPTY(&sock->queued_reqs));


	/* One request that is partially sent. */
	spdk_sock_request_queue(sock, req1);
	cb_arg1 = false;
	rc = spdk_sock_prep_reqs(sock, usock.write_task.iovs, 0, NULL);
	CU_ASSERT(rc == 2);
	sock_complete_reqs(sock, 92);
	CU_ASSERT(rc == 2);
	CU_ASSERT(cb_arg1 == false);
	CU_ASSERT(TAILQ_FIRST(&sock->queued_reqs) == req1);

	/* Get the second time partial sent result. */
	sock_complete_reqs(sock, 10);
	CU_ASSERT(cb_arg1 == false);
	CU_ASSERT(TAILQ_FIRST(&sock->queued_reqs) == req1);

	/* Data is finally sent. */
	sock_complete_reqs(sock, 26);
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

	suite = CU_add_suite("uring", NULL, NULL);


	CU_ADD_TEST(suite, flush_client);
	CU_ADD_TEST(suite, flush_server);

	CU_basic_set_mode(CU_BRM_VERBOSE);

	CU_basic_run_tests();

	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
