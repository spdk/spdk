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
#include "common/lib/test_env.c"
#include "nvme/nvme_tcp.c"

SPDK_LOG_REGISTER_COMPONENT("nvme", SPDK_LOG_NVME)

DEFINE_STUB(nvme_qpair_submit_request, int, (struct spdk_nvme_qpair *qpair,
		struct nvme_request *req), 0);
DEFINE_STUB(spdk_sock_recv, ssize_t, (struct spdk_sock *sock, void *buf, size_t len), 0);
DEFINE_STUB(spdk_sock_readv, ssize_t, (struct spdk_sock *sock, struct iovec *iov, int iovcnt), 0);
DEFINE_STUB(spdk_sock_writev, ssize_t, (struct spdk_sock *sock, struct iovec *iov, int iovcnt), 0);
DEFINE_STUB(nvme_request_check_timeout, int, (struct nvme_request *req, uint16_t cid,
		struct spdk_nvme_ctrlr_process *active_proc, uint64_t now_tick), 0);
DEFINE_STUB(spdk_nvme_ctrlr_get_current_process, struct spdk_nvme_ctrlr_process *,
	    (struct spdk_nvme_ctrlr *ctrlr),
	    0);

struct nvme_tcp_ut_bdev_io {
	struct iovec iovs[NVME_TCP_MAX_SGL_DESCRIPTORS];
	int iovpos;
};

/* essentially a simplification of bdev_nvme_next_sge and bdev_nvme_reset_sgl */
static void nvme_tcp_ut_reset_sgl(void *cb_arg, uint32_t offset)
{
	struct nvme_tcp_ut_bdev_io *bio = cb_arg;
	struct iovec *iov;

	for (bio->iovpos = 0; bio->iovpos < NVME_TCP_MAX_SGL_DESCRIPTORS; bio->iovpos++) {
		iov = &bio->iovs[bio->iovpos];
		/* Only provide offsets at the beginning of an iov */
		if (offset == 0) {
			break;
		}

		offset -= iov->iov_len;
	}

	SPDK_CU_ASSERT_FATAL(bio->iovpos < NVME_TCP_MAX_SGL_DESCRIPTORS);
}

static int nvme_tcp_ut_next_sge(void *cb_arg, void **address, uint32_t *length)
{
	struct nvme_tcp_ut_bdev_io *bio = cb_arg;
	struct iovec *iov;

	SPDK_CU_ASSERT_FATAL(bio->iovpos < NVME_TCP_MAX_SGL_DESCRIPTORS);

	iov = &bio->iovs[bio->iovpos];

	*address = iov->iov_base;
	*length = iov->iov_len;
	bio->iovpos++;

	return 0;
}

static void
test_nvme_tcp_build_sgl_request(void)
{
	struct nvme_tcp_qpair tqpair;
	struct nvme_tcp_req tcp_req = {0};
	struct nvme_request req = {{0}};
	struct nvme_tcp_ut_bdev_io bio;
	uint64_t i;
	int rc;

	tcp_req.req = &req;

	req.payload.reset_sgl_fn = nvme_tcp_ut_reset_sgl;
	req.payload.next_sge_fn = nvme_tcp_ut_next_sge;
	req.payload.contig_or_cb_arg = &bio;
	req.qpair = &tqpair.qpair;

	for (i = 0; i < NVME_TCP_MAX_SGL_DESCRIPTORS; i++) {
		bio.iovs[i].iov_base = (void *)(i * 0x1000);
		bio.iovs[i].iov_len = 0;
	}

	/* Test case 1: Single SGL. Expected: PASS */
	bio.iovpos = 0;
	req.payload_offset = 0;
	req.payload_size = 0x1000;
	bio.iovs[0].iov_len = 0x1000;
	rc = nvme_tcp_build_sgl_request(&tqpair, &tcp_req);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(bio.iovpos == 1);
	CU_ASSERT((uint64_t)tcp_req.iov[0].iov_base == (uint64_t)bio.iovs[0].iov_base);
	CU_ASSERT(tcp_req.iov[0].iov_len == bio.iovs[0].iov_len);
	CU_ASSERT(tcp_req.iovcnt == 1);

	/* Test case 2: Multiple SGL. Expected: PASS */
	bio.iovpos = 0;
	req.payload_offset = 0;
	req.payload_size = 0x4000;
	for (i = 0; i < 4; i++) {
		bio.iovs[i].iov_len = 0x1000;
	}
	rc = nvme_tcp_build_sgl_request(&tqpair, &tcp_req);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(bio.iovpos == 4);
	CU_ASSERT(tcp_req.iovcnt == 4);
	for (i = 0; i < 4; i++) {
		CU_ASSERT(tcp_req.iov[i].iov_len == bio.iovs[i].iov_len);
		CU_ASSERT((uint64_t)tcp_req.iov[i].iov_base == (uint64_t)bio.iovs[i].iov_base);
	}

	/* Test case 3: Payload is bigger than SGL. Expected: FAIL */
	bio.iovpos = 0;
	req.payload_offset = 0;
	req.payload_size = 0x17000;
	for (i = 0; i < NVME_TCP_MAX_SGL_DESCRIPTORS; i++) {
		bio.iovs[i].iov_len = 0x1000;
	}
	rc = nvme_tcp_build_sgl_request(&tqpair, &tcp_req);
	SPDK_CU_ASSERT_FATAL(rc != 0);
	CU_ASSERT(bio.iovpos == NVME_TCP_MAX_SGL_DESCRIPTORS);
	for (i = 0; i < NVME_TCP_MAX_SGL_DESCRIPTORS; i++) {
		CU_ASSERT(tcp_req.iov[i].iov_len == bio.iovs[i].iov_len);
		CU_ASSERT((uint64_t)tcp_req.iov[i].iov_base == (uint64_t)bio.iovs[i].iov_base);
	}
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("nvme_tcp", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (CU_add_test(suite, "build_sgl_request", test_nvme_tcp_build_sgl_request) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
