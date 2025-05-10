/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/util.h"

#include "spdk_internal/mock.h"

#include "spdk_internal/cunit.h"

#include "common/lib/test_env.c"
#include "sock/posix/posix.c"

DEFINE_STUB(spdk_sock_map_insert, int, (struct spdk_sock_map *map, int placement_id,
					struct spdk_sock_group_impl *group), 0);
DEFINE_STUB_V(spdk_sock_map_release, (struct spdk_sock_map *map, int placement_id));
DEFINE_STUB(spdk_sock_map_lookup, int, (struct spdk_sock_map *map, int placement_id,
					struct spdk_sock_group_impl **group, struct spdk_sock_group_impl *hint), 0);
DEFINE_STUB(spdk_sock_map_find_free, int, (struct spdk_sock_map *map), -1);
DEFINE_STUB_V(spdk_sock_map_cleanup, (struct spdk_sock_map *map));

DEFINE_STUB_V(spdk_net_impl_register, (struct spdk_net_impl *impl));
DEFINE_STUB(spdk_sock_set_default_impl, int, (const char *impl_name), 0);
DEFINE_STUB(spdk_sock_close, int, (struct spdk_sock **s), 0);
DEFINE_STUB(spdk_sock_group_provide_buf, int, (struct spdk_sock_group *group, void *buf,
		size_t len, void *ctx), 0);
DEFINE_STUB(spdk_sock_group_get_buf, size_t, (struct spdk_sock_group *group, void **buf,
		void **ctx), 0);
DEFINE_STUB(spdk_sock_posix_fd_create, int, (struct addrinfo *res, struct spdk_sock_opts *opts,
		struct spdk_sock_impl_opts *impl_opts), 0);
DEFINE_STUB(spdk_sock_posix_fd_connect, int, (int fd, struct addrinfo *res,
		struct spdk_sock_opts *opts), 0);
DEFINE_STUB(spdk_sock_posix_fd_connect_async, int, (int fd, struct addrinfo *res,
		struct spdk_sock_opts *opts), 0);
DEFINE_STUB(spdk_sock_posix_fd_connect_poll_async, int, (int fd), 0);
DEFINE_STUB(spdk_sock_posix_getaddrinfo, struct addrinfo *, (const char *ip, int port), 0);

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
	struct spdk_posix_sock psock = {.ready = true};
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

ssize_t
recvmsg(int sockfd, struct msghdr *msg, int flags)
{
	struct sock_extended_err *serr;
	struct cmsghdr *cm;
	int rc;

	rc = MOCK_GET(recvmsg);
	if (rc < 0) {
		errno = -rc;
		return -1;
	}

	cm = CMSG_FIRSTHDR(msg);
	cm->cmsg_level = SOL_IP;
	cm->cmsg_type = IP_RECVERR;

	serr = (struct sock_extended_err *)CMSG_DATA(cm);
	serr->ee_errno = 0;
	serr->ee_origin = SO_EE_ORIGIN_ZEROCOPY;
	/* Use the mock queue to get the notification range. */
	serr->ee_info = MOCK_GET(recvmsg);
	serr->ee_data = MOCK_GET(recvmsg);

	return rc;
}

static void
flush_req_chunks_with_zero_copy_threshold(void)
{
	/* Verify if a fully sent request awaits zero copy completion, when one of the chunks
	 * was sent with a zcopy flag, but the last one not due to the threshold. */
	struct spdk_sock_impl_opts impl_opts = { .zerocopy_threshold = 50 };
	struct spdk_posix_sock_group_impl group = {};
	struct spdk_sock_request *req;
	struct spdk_posix_sock psock = {.ready = true};
	struct spdk_sock *sock;
	bool req_completed;
	int rc;

	psock.zcopy = true;
	psock.sendmsg_idx = UINT32_MAX;
	sock = &psock.base;
	sock->group_impl = &group.base;
	sock->impl_opts = impl_opts;
	TAILQ_INIT(&sock->queued_reqs);
	TAILQ_INIT(&sock->pending_reqs);

	req = calloc(1, sizeof(struct spdk_sock_request) + sizeof(struct iovec));
	SPDK_CU_ASSERT_FATAL(req != NULL);
	req_completed = false;
	SPDK_SOCK_REQUEST_IOV(req, 0)->iov_base = (void *)100;
	SPDK_SOCK_REQUEST_IOV(req, 0)->iov_len = 100;
	req->iovcnt = 1;
	req->cb_fn = _req_cb;
	req->cb_arg = &req_completed;

	spdk_sock_request_queue(sock, req);

	/* Send first chunk above zcopy threshold. */
	MOCK_SET(sendmsg, 75);
	rc = posix_sock_flush(sock);
	CU_ASSERT(rc == 0);
	/* Sent partially, request is not completed. */
	CU_ASSERT(req_completed == false);

	/* Send last remaining chunk below zcopy threshold. */
	MOCK_SET(sendmsg, 25);
	/* Notification not yet arrived. */
	MOCK_SET(recvmsg, -EAGAIN);
	rc = posix_sock_flush(sock);
	CU_ASSERT(rc == 0);
	/* Sent fully, but zcopy not yet arrived, so request is not completed. */
	CU_ASSERT(req_completed == false);

	/* No mock for sendmsg, we sent all. */
	MOCK_ENQUEUE(recvmsg, 1); /* Notification arrived. */
	MOCK_ENQUEUE(recvmsg, 0); /* Pass notification range low. */
	MOCK_ENQUEUE(recvmsg, 0); /* Pass notification range high. */
	MOCK_ENQUEUE(recvmsg, -EAGAIN); /* No more messages. */
	rc = posix_sock_flush(sock);
	CU_ASSERT(rc == 0);
	/* Notification arrived, request sent fully and should be completed. */
	CU_ASSERT(req_completed == true);

	free(req);
}

static void
flush_two_reqs_chunks_with_zero_copy_threshold(void)
{
	/* Verify if zcopy notification for partially sent request chunk is not missed,
	 * when the chunk was sent together with the other request. */
	struct spdk_sock_impl_opts impl_opts = { .zerocopy_threshold = 50 };
	struct spdk_posix_sock_group_impl group = {};
	struct spdk_sock_request *req1, *req2;
	struct spdk_posix_sock psock = {.ready = true};
	struct spdk_sock *sock;
	bool req1_completed, req2_completed;
	int rc;

	psock.zcopy = true;
	psock.sendmsg_idx = UINT32_MAX;
	sock = &psock.base;
	sock->group_impl = &group.base;
	sock->impl_opts = impl_opts;
	TAILQ_INIT(&sock->queued_reqs);
	TAILQ_INIT(&sock->pending_reqs);

	req1 = calloc(1, sizeof(struct spdk_sock_request) + 1 * sizeof(struct iovec));
	SPDK_CU_ASSERT_FATAL(req1 != NULL);
	req1_completed = false;
	SPDK_SOCK_REQUEST_IOV(req1, 0)->iov_base = (void *)100;
	SPDK_SOCK_REQUEST_IOV(req1, 0)->iov_len = 100;
	req1->iovcnt = 1;
	req1->cb_fn = _req_cb;
	req1->cb_arg = &req1_completed;

	spdk_sock_request_queue(sock, req1);

	req2 = calloc(1, sizeof(struct spdk_sock_request) + 1 * sizeof(struct iovec));
	SPDK_CU_ASSERT_FATAL(req1 != NULL);
	req2_completed = false;
	SPDK_SOCK_REQUEST_IOV(req2, 0)->iov_base = (void *)200;
	SPDK_SOCK_REQUEST_IOV(req2, 0)->iov_len = 100;
	req2->iovcnt = 1;
	req2->cb_fn = _req_cb;
	req2->cb_arg = &req2_completed;

	spdk_sock_request_queue(sock, req2);
	/* Send req1 completely, req2 partially, both with zcopy. */
	MOCK_SET(sendmsg, 100 + 75);
	/* No zcopy notification for req1. */
	MOCK_SET(recvmsg, -EAGAIN);
	rc = posix_sock_flush(sock);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req1_completed == false);
	CU_ASSERT(req2_completed == false);

	/* Send next chunk of req2. */
	MOCK_SET(sendmsg, 20);
	/* Zcopy notification for full req1 and req2 chunk arrived. */
	MOCK_ENQUEUE(recvmsg, 1); /* Notification arrived. */
	MOCK_ENQUEUE(recvmsg, 0); /* Pass notification range low. */
	MOCK_ENQUEUE(recvmsg, 0); /* Pass notification range high. */
	MOCK_ENQUEUE(recvmsg, -EAGAIN); /* No more messages. */
	rc = posix_sock_flush(sock);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req1_completed == true);
	CU_ASSERT(req2_completed == false);

	/* Send last chunk of req2. */
	MOCK_SET(sendmsg, 5);
	/* No need to recvmsg, notification for req2 zcopy chunk already received. */
	rc = posix_sock_flush(sock);
	CU_ASSERT(rc == 0);
	/* Req2 should be completed within this flush. */
	CU_ASSERT(req2_completed == true);

	free(req1);
	free(req2);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("posix", NULL, NULL);

	CU_ADD_TEST(suite, flush);
	CU_ADD_TEST(suite, flush_req_chunks_with_zero_copy_threshold);
	CU_ADD_TEST(suite, flush_two_reqs_chunks_with_zero_copy_threshold);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);

	CU_cleanup_registry();

	return num_failures;
}
