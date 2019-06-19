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
#include "common/lib/test_sock.c"

#include "nvme/nvme_tcp.c"

SPDK_LOG_REGISTER_COMPONENT("nvme", SPDK_LOG_NVME)

DEFINE_STUB(nvme_qpair_submit_request,
	    int, (struct spdk_nvme_qpair *qpair, struct nvme_request *req), 0);

static void
test_nvme_tcp_pdu_set_data_buf(void)
{
	struct nvme_tcp_pdu pdu;
	struct iovec iov[NVME_TCP_MAX_SGL_DESCRIPTORS];
	uint32_t data_len;
	uint64_t i;

	/* 1st case: input is a single SGL entry. */
	iov[0].iov_base = (void *)0xDEADBEEF;
	iov[0].iov_len = 4096;

	memset(&pdu, 0, sizeof(struct nvme_tcp_pdu));

	nvme_tcp_pdu_set_data_buf(&pdu, iov, 1, 1024, 512);

	CU_ASSERT(pdu.data_iovcnt == 1);
	CU_ASSERT((uint64_t)pdu.data_iov[0].iov_base == 0xDEADBEEF + 1024);
	CU_ASSERT(pdu.data_iov[0].iov_len == 512);

	/* 2nd case: simulate split on multiple SGL entries. */
	iov[0].iov_base = (void *)0xDEADBEEF;
	iov[0].iov_len = 4096;
	iov[1].iov_base = (void *)0xFEEDBEEF;
	iov[1].iov_len = 512 * 7;
	iov[2].iov_base = (void *)0xF00DF00D;
	iov[2].iov_len = 4096 * 2;

	memset(&pdu, 0, sizeof(struct nvme_tcp_pdu));

	nvme_tcp_pdu_set_data_buf(&pdu, iov, 3, 0, 2048);

	CU_ASSERT(pdu.data_iovcnt == 1);
	CU_ASSERT((uint64_t)pdu.data_iov[0].iov_base == 0xDEADBEEF);
	CU_ASSERT(pdu.data_iov[0].iov_len == 2048);

	memset(&pdu, 0, sizeof(struct nvme_tcp_pdu));

	nvme_tcp_pdu_set_data_buf(&pdu, iov, 3, 2048, 2048 + 512 * 3);

	CU_ASSERT(pdu.data_iovcnt == 2);
	CU_ASSERT((uint64_t)pdu.data_iov[0].iov_base == 0xDEADBEEF + 2048);
	CU_ASSERT(pdu.data_iov[0].iov_len == 2048);
	CU_ASSERT((uint64_t)pdu.data_iov[1].iov_base == 0xFEEDBEEF);
	CU_ASSERT(pdu.data_iov[1].iov_len == 512 * 3);

	memset(&pdu, 0, sizeof(struct nvme_tcp_pdu));

	nvme_tcp_pdu_set_data_buf(&pdu, iov, 3, 4096 + 512 * 3, 512 * 4 + 4096 * 2);

	CU_ASSERT(pdu.data_iovcnt == 2);
	CU_ASSERT((uint64_t)pdu.data_iov[0].iov_base == 0xFEEDBEEF + 512 * 3);
	CU_ASSERT(pdu.data_iov[0].iov_len == 512 * 4);
	CU_ASSERT((uint64_t)pdu.data_iov[1].iov_base == 0xF00DF00D);
	CU_ASSERT(pdu.data_iov[1].iov_len == 4096 * 2);

	/* 3rd case: Number of input SGL entries is equal to the number of PDU SGL
	 * entries.
	 */
	data_len = 0;
	for (i = 0; i < NVME_TCP_MAX_SGL_DESCRIPTORS; i++) {
		iov[i].iov_base = (void *)(0xDEADBEEF + i);
		iov[i].iov_len = 512 * (i + 1);
		data_len += 512 * (i + 1);
	}

	memset(&pdu, 0, sizeof(struct nvme_tcp_pdu));

	nvme_tcp_pdu_set_data_buf(&pdu, iov, NVME_TCP_MAX_SGL_DESCRIPTORS, 0, data_len);

	CU_ASSERT(pdu.data_iovcnt == NVME_TCP_MAX_SGL_DESCRIPTORS);
	for (i = 0; i < NVME_TCP_MAX_SGL_DESCRIPTORS; i++) {
		CU_ASSERT((uint64_t)pdu.data_iov[i].iov_base == 0xDEADBEEF + i);
		CU_ASSERT(pdu.data_iov[i].iov_len == 512 * (i + 1));
	}
}

static void
test_nvme_tcp_build_iovs(void)
{
	struct nvme_tcp_pdu pdu = {};
	struct iovec iovs[4] = {};
	uint32_t mapped_length = 0;
	int rc;

	pdu.hdr.common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD;
	pdu.hdr.common.hlen = sizeof(struct spdk_nvme_tcp_cmd);
	pdu.hdr.common.plen = pdu.hdr.common.hlen + SPDK_NVME_TCP_DIGEST_LEN + 4096 * 2 +
			      SPDK_NVME_TCP_DIGEST_LEN;
	pdu.data_len = 4096 * 2;
	pdu.padding_len = 0;

	pdu.data_iov[0].iov_base = (void *)0xDEADBEEF;
	pdu.data_iov[0].iov_len = 4096 * 2;
	pdu.data_iovcnt = 1;

	rc = nvme_tcp_build_iovs(iovs, 4, &pdu, true, true, &mapped_length);
	CU_ASSERT(rc == 3);
	CU_ASSERT(iovs[0].iov_base == (void *)&pdu.hdr.raw);
	CU_ASSERT(iovs[0].iov_len == sizeof(struct spdk_nvme_tcp_cmd) + SPDK_NVME_TCP_DIGEST_LEN);
	CU_ASSERT(iovs[1].iov_base == (void *)0xDEADBEEF);
	CU_ASSERT(iovs[1].iov_len == 4096 * 2);
	CU_ASSERT(iovs[2].iov_base == (void *)pdu.data_digest);
	CU_ASSERT(iovs[2].iov_len == SPDK_NVME_TCP_DIGEST_LEN);
	CU_ASSERT(mapped_length == sizeof(struct spdk_nvme_tcp_cmd) + SPDK_NVME_TCP_DIGEST_LEN +
		  4096 * 2 + SPDK_NVME_TCP_DIGEST_LEN);

	pdu.writev_offset += sizeof(struct spdk_nvme_tcp_cmd) + SPDK_NVME_TCP_DIGEST_LEN;

	rc = nvme_tcp_build_iovs(iovs, 6, &pdu, true, true, &mapped_length);
	CU_ASSERT(rc == 2);
	CU_ASSERT(iovs[0].iov_base == (void *)0xDEADBEEF);
	CU_ASSERT(iovs[0].iov_len == 4096 * 2);
	CU_ASSERT(iovs[1].iov_base == (void *)pdu.data_digest);
	CU_ASSERT(iovs[1].iov_len == SPDK_NVME_TCP_DIGEST_LEN);
	CU_ASSERT(mapped_length == 4096 * 2 + SPDK_NVME_TCP_DIGEST_LEN);

	pdu.writev_offset += 4096 * 2;

	rc = nvme_tcp_build_iovs(iovs, 6, &pdu, true, true, &mapped_length);
	CU_ASSERT(rc == 1);
	CU_ASSERT(iovs[0].iov_base == (void *)pdu.data_digest);
	CU_ASSERT(iovs[0].iov_len == SPDK_NVME_TCP_DIGEST_LEN);
	CU_ASSERT(mapped_length == SPDK_NVME_TCP_DIGEST_LEN);

	pdu.writev_offset += SPDK_NVME_TCP_DIGEST_LEN;

	rc = nvme_tcp_build_iovs(iovs, 6, &pdu, true, true, &mapped_length);
	CU_ASSERT(rc == 0);

	pdu.writev_offset = 0;

	rc = nvme_tcp_build_iovs(iovs, 2, &pdu, true, true, &mapped_length);
	CU_ASSERT(rc == 2);
	CU_ASSERT(iovs[0].iov_base == (void *)&pdu.hdr.raw);
	CU_ASSERT(iovs[0].iov_len == sizeof(struct spdk_nvme_tcp_cmd) + SPDK_NVME_TCP_DIGEST_LEN);
	CU_ASSERT(iovs[1].iov_base == (void *)0xDEADBEEF);
	CU_ASSERT(iovs[1].iov_len == 4096 * 2);
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

	if (CU_add_test(suite, "nvme_tcp_pdu_set_data_buf",
			test_nvme_tcp_pdu_set_data_buf) == NULL ||
	    CU_add_test(suite, "test_nvme_tcp_build_iovs",
			test_nvme_tcp_build_iovs) == NULL
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
