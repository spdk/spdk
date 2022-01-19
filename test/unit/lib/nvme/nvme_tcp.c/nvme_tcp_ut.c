/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

#include "common/lib/test_sock.c"

#include "nvme/nvme_tcp.c"
#include "common/lib/nvme/common_stubs.h"

SPDK_LOG_REGISTER_COMPONENT(nvme)

DEFINE_STUB(nvme_qpair_submit_request,
	    int, (struct spdk_nvme_qpair *qpair, struct nvme_request *req), 0);

DEFINE_STUB(spdk_sock_set_priority,
	    int, (struct spdk_sock *sock, int priority), 0);

DEFINE_STUB(spdk_nvme_poll_group_remove, int, (struct spdk_nvme_poll_group *group,
		struct spdk_nvme_qpair *qpair), 0);
DEFINE_STUB(spdk_sock_get_optimal_sock_group,
	    int,
	    (struct spdk_sock *sock, struct spdk_sock_group **group),
	    0);

DEFINE_STUB(spdk_sock_group_get_ctx,
	    void *,
	    (struct spdk_sock_group *group),
	    NULL);

DEFINE_STUB(spdk_nvme_poll_group_process_completions, int64_t, (struct spdk_nvme_poll_group *group,
		uint32_t completions_per_qpair, spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb), 0);

DEFINE_STUB(nvme_poll_group_connect_qpair, int, (struct spdk_nvme_qpair *qpair), 0);
DEFINE_STUB_V(nvme_qpair_resubmit_requests, (struct spdk_nvme_qpair *qpair, uint32_t num_requests));

static void
test_nvme_tcp_pdu_set_data_buf(void)
{
	struct nvme_tcp_pdu pdu = {};
	struct iovec iov[NVME_TCP_MAX_SGL_DESCRIPTORS] = {};
	uint32_t data_len;
	uint64_t i;

	/* 1st case: input is a single SGL entry. */
	iov[0].iov_base = (void *)0xDEADBEEF;
	iov[0].iov_len = 4096;

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

	nvme_tcp_pdu_set_data_buf(&pdu, iov, 3, 0, 2048);

	CU_ASSERT(pdu.data_iovcnt == 1);
	CU_ASSERT((uint64_t)pdu.data_iov[0].iov_base == 0xDEADBEEF);
	CU_ASSERT(pdu.data_iov[0].iov_len == 2048);

	nvme_tcp_pdu_set_data_buf(&pdu, iov, 3, 2048, 2048 + 512 * 3);

	CU_ASSERT(pdu.data_iovcnt == 2);
	CU_ASSERT((uint64_t)pdu.data_iov[0].iov_base == 0xDEADBEEF + 2048);
	CU_ASSERT(pdu.data_iov[0].iov_len == 2048);
	CU_ASSERT((uint64_t)pdu.data_iov[1].iov_base == 0xFEEDBEEF);
	CU_ASSERT(pdu.data_iov[1].iov_len == 512 * 3);

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
	const uintptr_t pdu_iov_len = 4096;
	struct nvme_tcp_pdu pdu = {};
	struct iovec iovs[5] = {};
	uint32_t mapped_length = 0;
	int rc;

	pdu.hdr.common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD;
	pdu.hdr.common.hlen = sizeof(struct spdk_nvme_tcp_cmd);
	pdu.hdr.common.plen = pdu.hdr.common.hlen + SPDK_NVME_TCP_DIGEST_LEN + pdu_iov_len * 2 +
			      SPDK_NVME_TCP_DIGEST_LEN;
	pdu.data_len = pdu_iov_len * 2;
	pdu.padding_len = 0;

	pdu.data_iov[0].iov_base = (void *)0xDEADBEEF;
	pdu.data_iov[0].iov_len = pdu_iov_len;
	pdu.data_iov[1].iov_base = (void *)(0xDEADBEEF + pdu_iov_len);
	pdu.data_iov[1].iov_len = pdu_iov_len;
	pdu.data_iovcnt = 2;

	rc = nvme_tcp_build_iovs(iovs, 5, &pdu, true, true, &mapped_length);
	CU_ASSERT(rc == 4);
	CU_ASSERT(iovs[0].iov_base == (void *)&pdu.hdr.raw);
	CU_ASSERT(iovs[0].iov_len == sizeof(struct spdk_nvme_tcp_cmd) + SPDK_NVME_TCP_DIGEST_LEN);
	CU_ASSERT(iovs[1].iov_base == (void *)0xDEADBEEF);
	CU_ASSERT(iovs[1].iov_len == pdu_iov_len);
	CU_ASSERT(iovs[2].iov_base == (void *)(0xDEADBEEF + pdu_iov_len));
	CU_ASSERT(iovs[2].iov_len == pdu_iov_len);
	CU_ASSERT(iovs[3].iov_base == (void *)pdu.data_digest);
	CU_ASSERT(iovs[3].iov_len == SPDK_NVME_TCP_DIGEST_LEN);
	CU_ASSERT(mapped_length == sizeof(struct spdk_nvme_tcp_cmd) + SPDK_NVME_TCP_DIGEST_LEN +
		  pdu_iov_len * 2 + SPDK_NVME_TCP_DIGEST_LEN);

	/* Add a new data_iov entry, update pdu iov count and data length */
	pdu.data_iov[2].iov_base = (void *)(0xBAADF00D);
	pdu.data_iov[2].iov_len = 123;
	pdu.data_iovcnt = 3;
	pdu.data_len += 123;
	pdu.hdr.common.plen += 123;

	rc = nvme_tcp_build_iovs(iovs, 5, &pdu, true, true, &mapped_length);
	CU_ASSERT(rc == 5);
	CU_ASSERT(iovs[0].iov_base == (void *)&pdu.hdr.raw);
	CU_ASSERT(iovs[0].iov_len == sizeof(struct spdk_nvme_tcp_cmd) + SPDK_NVME_TCP_DIGEST_LEN);
	CU_ASSERT(iovs[1].iov_base == (void *)0xDEADBEEF);
	CU_ASSERT(iovs[1].iov_len == pdu_iov_len);
	CU_ASSERT(iovs[2].iov_base == (void *)(0xDEADBEEF + pdu_iov_len));
	CU_ASSERT(iovs[2].iov_len == pdu_iov_len);
	CU_ASSERT(iovs[3].iov_base == (void *)(0xBAADF00D));
	CU_ASSERT(iovs[3].iov_len == 123);
	CU_ASSERT(iovs[4].iov_base == (void *)pdu.data_digest);
	CU_ASSERT(iovs[4].iov_len == SPDK_NVME_TCP_DIGEST_LEN);
	CU_ASSERT(mapped_length == sizeof(struct spdk_nvme_tcp_cmd) + SPDK_NVME_TCP_DIGEST_LEN +
		  pdu_iov_len * 2 + SPDK_NVME_TCP_DIGEST_LEN + 123);
}

struct nvme_tcp_ut_bdev_io {
	struct iovec iovs[NVME_TCP_MAX_SGL_DESCRIPTORS];
	int iovpos;
};

/* essentially a simplification of bdev_nvme_next_sge and bdev_nvme_reset_sgl */
static void
nvme_tcp_ut_reset_sgl(void *cb_arg, uint32_t offset)
{
	struct nvme_tcp_ut_bdev_io *bio = cb_arg;
	struct iovec *iov;

	for (bio->iovpos = 0; bio->iovpos < NVME_TCP_MAX_SGL_DESCRIPTORS; bio->iovpos++) {
		iov = &bio->iovs[bio->iovpos];
		/* Offset must be aligned with the start of any SGL entry */
		if (offset == 0) {
			break;
		}

		SPDK_CU_ASSERT_FATAL(offset >= iov->iov_len);
		offset -= iov->iov_len;
	}

	SPDK_CU_ASSERT_FATAL(offset == 0);
	SPDK_CU_ASSERT_FATAL(bio->iovpos < NVME_TCP_MAX_SGL_DESCRIPTORS);
}

static int
nvme_tcp_ut_next_sge(void *cb_arg, void **address, uint32_t *length)
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
	struct spdk_nvme_ctrlr ctrlr = {{0}};
	struct nvme_tcp_req tcp_req = {0};
	struct nvme_request req = {{0}};
	struct nvme_tcp_ut_bdev_io bio;
	uint64_t i;
	int rc;

	ctrlr.max_sges = NVME_TCP_MAX_SGL_DESCRIPTORS;
	tqpair.qpair.ctrlr = &ctrlr;
	tcp_req.req = &req;

	req.payload.reset_sgl_fn = nvme_tcp_ut_reset_sgl;
	req.payload.next_sge_fn = nvme_tcp_ut_next_sge;
	req.payload.contig_or_cb_arg = &bio;
	req.qpair = &tqpair.qpair;

	for (i = 0; i < NVME_TCP_MAX_SGL_DESCRIPTORS; i++) {
		bio.iovs[i].iov_base = (void *)(0xFEEDB000 + i * 0x1000);
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

static void
test_nvme_tcp_pdu_set_data_buf_with_md(void)
{
	struct nvme_tcp_pdu pdu = {};
	struct iovec iovs[7] = {};
	struct spdk_dif_ctx dif_ctx = {};
	int rc;

	pdu.dif_ctx = &dif_ctx;

	rc = spdk_dif_ctx_init(&dif_ctx, 520, 8, true, false, SPDK_DIF_DISABLE, 0,
			       0, 0, 0, 0, 0);
	CU_ASSERT(rc == 0);

	/* Single iovec case */
	iovs[0].iov_base = (void *)0xDEADBEEF;
	iovs[0].iov_len = 2080;

	nvme_tcp_pdu_set_data_buf(&pdu, iovs, 1, 0, 500);

	CU_ASSERT(dif_ctx.data_offset == 0);
	CU_ASSERT(pdu.data_len == 500);
	CU_ASSERT(pdu.data_iovcnt == 1);
	CU_ASSERT(pdu.data_iov[0].iov_base == (void *)0xDEADBEEF);
	CU_ASSERT(pdu.data_iov[0].iov_len == 500);

	nvme_tcp_pdu_set_data_buf(&pdu, iovs, 1, 500, 1000);

	CU_ASSERT(dif_ctx.data_offset == 500);
	CU_ASSERT(pdu.data_len == 1000);
	CU_ASSERT(pdu.data_iovcnt == 1);
	CU_ASSERT(pdu.data_iov[0].iov_base == (void *)(0xDEADBEEF + 500));
	CU_ASSERT(pdu.data_iov[0].iov_len == 1016);

	nvme_tcp_pdu_set_data_buf(&pdu, iovs, 1, 1500, 548);

	CU_ASSERT(dif_ctx.data_offset == 1500);
	CU_ASSERT(pdu.data_len == 548);
	CU_ASSERT(pdu.data_iovcnt == 1);
	CU_ASSERT(pdu.data_iov[0].iov_base == (void *)(0xDEADBEEF + 1516));
	CU_ASSERT(pdu.data_iov[0].iov_len == 564);

	/* Multiple iovecs case */
	iovs[0].iov_base = (void *)0xDEADBEEF;
	iovs[0].iov_len = 256;
	iovs[1].iov_base = (void *)((uint8_t *)(0xDEADBEEF + 0x1000));
	iovs[1].iov_len = 256 + 1;
	iovs[2].iov_base = (void *)((uint8_t *)(0xDEADBEEF + 0x2000));
	iovs[2].iov_len = 4;
	iovs[3].iov_base = (void *)((uint8_t *)(0xDEADBEEF + 0x3000));
	iovs[3].iov_len = 3 + 123;
	iovs[4].iov_base = (void *)((uint8_t *)(0xDEADBEEF + 0x4000));
	iovs[4].iov_len = 389 + 6;
	iovs[5].iov_base = (void *)((uint8_t *)(0xDEADBEEF + 0x5000));
	iovs[5].iov_len = 2 + 512 + 8 + 432;
	iovs[6].iov_base = (void *)((uint8_t *)(0xDEADBEEF + 0x6000));
	iovs[6].iov_len = 80 + 8;

	nvme_tcp_pdu_set_data_buf(&pdu, iovs, 7, 0, 500);

	CU_ASSERT(dif_ctx.data_offset == 0);
	CU_ASSERT(pdu.data_len == 500);
	CU_ASSERT(pdu.data_iovcnt == 2);
	CU_ASSERT(pdu.data_iov[0].iov_base == (void *)0xDEADBEEF);
	CU_ASSERT(pdu.data_iov[0].iov_len == 256);
	CU_ASSERT(pdu.data_iov[1].iov_base == (void *)(0xDEADBEEF + 0x1000));
	CU_ASSERT(pdu.data_iov[1].iov_len == 244);

	nvme_tcp_pdu_set_data_buf(&pdu, iovs, 7, 500, 1000);

	CU_ASSERT(dif_ctx.data_offset == 500);
	CU_ASSERT(pdu.data_len == 1000);
	CU_ASSERT(pdu.data_iovcnt == 5);
	CU_ASSERT(pdu.data_iov[0].iov_base == (void *)(0xDEADBEEF + 0x1000 + 244));
	CU_ASSERT(pdu.data_iov[0].iov_len == 13);
	CU_ASSERT(pdu.data_iov[1].iov_base == (void *)(0xDEADBEEF + 0x2000));
	CU_ASSERT(pdu.data_iov[1].iov_len == 4);
	CU_ASSERT(pdu.data_iov[2].iov_base == (void *)(0xDEADBEEF + 0x3000));
	CU_ASSERT(pdu.data_iov[2].iov_len == 3 + 123);
	CU_ASSERT(pdu.data_iov[3].iov_base == (void *)(0xDEADBEEF + 0x4000));
	CU_ASSERT(pdu.data_iov[3].iov_len == 395);
	CU_ASSERT(pdu.data_iov[4].iov_base == (void *)(0xDEADBEEF + 0x5000));
	CU_ASSERT(pdu.data_iov[4].iov_len == 478);

	nvme_tcp_pdu_set_data_buf(&pdu, iovs, 7, 1500, 548);

	CU_ASSERT(dif_ctx.data_offset == 1500);
	CU_ASSERT(pdu.data_len == 548);
	CU_ASSERT(pdu.data_iovcnt == 2);
	CU_ASSERT(pdu.data_iov[0].iov_base == (void *)(0xDEADBEEF + 0x5000 + 478));
	CU_ASSERT(pdu.data_iov[0].iov_len == 476);
	CU_ASSERT(pdu.data_iov[1].iov_base == (void *)(0xDEADBEEF + 0x6000));
	CU_ASSERT(pdu.data_iov[1].iov_len == 88);
}

static void
test_nvme_tcp_build_iovs_with_md(void)
{
	struct nvme_tcp_pdu pdu = {};
	struct iovec iovs[11] = {};
	struct spdk_dif_ctx dif_ctx = {};
	uint32_t mapped_length = 0;
	int rc;

	rc = spdk_dif_ctx_init(&dif_ctx, 520, 8, true, false, SPDK_DIF_DISABLE, 0,
			       0, 0, 0, 0, 0);
	CU_ASSERT(rc == 0);

	pdu.dif_ctx = &dif_ctx;

	pdu.hdr.common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD;
	pdu.hdr.common.hlen = sizeof(struct spdk_nvme_tcp_cmd);
	pdu.hdr.common.plen = pdu.hdr.common.hlen + SPDK_NVME_TCP_DIGEST_LEN + 512 * 8 +
			      SPDK_NVME_TCP_DIGEST_LEN;
	pdu.data_len = 512 * 8;
	pdu.padding_len = 0;

	pdu.data_iov[0].iov_base = (void *)0xDEADBEEF;
	pdu.data_iov[0].iov_len = (512 + 8) * 8;
	pdu.data_iovcnt = 1;

	rc = nvme_tcp_build_iovs(iovs, 11, &pdu, true, true, &mapped_length);
	CU_ASSERT(rc == 10);
	CU_ASSERT(iovs[0].iov_base == (void *)&pdu.hdr.raw);
	CU_ASSERT(iovs[0].iov_len == sizeof(struct spdk_nvme_tcp_cmd) + SPDK_NVME_TCP_DIGEST_LEN);
	CU_ASSERT(iovs[1].iov_base == (void *)0xDEADBEEF);
	CU_ASSERT(iovs[1].iov_len == 512);
	CU_ASSERT(iovs[2].iov_base == (void *)(0xDEADBEEF + 520));
	CU_ASSERT(iovs[2].iov_len == 512);
	CU_ASSERT(iovs[3].iov_base == (void *)(0xDEADBEEF + 520 * 2));
	CU_ASSERT(iovs[3].iov_len == 512);
	CU_ASSERT(iovs[4].iov_base == (void *)(0xDEADBEEF + 520 * 3));
	CU_ASSERT(iovs[4].iov_len == 512);
	CU_ASSERT(iovs[5].iov_base == (void *)(0xDEADBEEF + 520 * 4));
	CU_ASSERT(iovs[5].iov_len == 512);
	CU_ASSERT(iovs[6].iov_base == (void *)(0xDEADBEEF + 520 * 5));
	CU_ASSERT(iovs[6].iov_len == 512);
	CU_ASSERT(iovs[7].iov_base == (void *)(0xDEADBEEF + 520 * 6));
	CU_ASSERT(iovs[7].iov_len == 512);
	CU_ASSERT(iovs[8].iov_base == (void *)(0xDEADBEEF + 520 * 7));
	CU_ASSERT(iovs[8].iov_len == 512);
	CU_ASSERT(iovs[9].iov_base == (void *)pdu.data_digest);
	CU_ASSERT(iovs[9].iov_len == SPDK_NVME_TCP_DIGEST_LEN);
	CU_ASSERT(mapped_length == sizeof(struct spdk_nvme_tcp_cmd) + SPDK_NVME_TCP_DIGEST_LEN +
		  512 * 8 + SPDK_NVME_TCP_DIGEST_LEN);
}

/* Just define, nothing to do */
static void
ut_nvme_complete_request(void *arg, const struct spdk_nvme_cpl *cpl)
{
	return;
}

static void
test_nvme_tcp_req_complete_safe(void)
{
	bool rc;
	struct nvme_tcp_req	tcp_req = {0};
	struct nvme_request	req = {{0}};
	struct nvme_tcp_qpair	tqpair = {{0}};

	tcp_req.req = &req;
	tcp_req.req->qpair = &tqpair.qpair;
	tcp_req.req->cb_fn = ut_nvme_complete_request;
	tcp_req.tqpair = &tqpair;
	tcp_req.state = NVME_TCP_REQ_ACTIVE;
	TAILQ_INIT(&tcp_req.tqpair->outstanding_reqs);

	/* Test case 1: send operation and transfer completed. Expect: PASS */
	tcp_req.state = NVME_TCP_REQ_ACTIVE;
	tcp_req.ordering.bits.send_ack = 1;
	tcp_req.ordering.bits.data_recv = 1;
	TAILQ_INSERT_TAIL(&tcp_req.tqpair->outstanding_reqs, &tcp_req, link);

	rc = nvme_tcp_req_complete_safe(&tcp_req);
	CU_ASSERT(rc == true);

	/* Test case 2: send operation not completed. Expect: FAIL */
	tcp_req.ordering.raw = 0;
	tcp_req.state = NVME_TCP_REQ_ACTIVE;
	TAILQ_INSERT_TAIL(&tcp_req.tqpair->outstanding_reqs, &tcp_req, link);

	rc = nvme_tcp_req_complete_safe(&tcp_req);
	SPDK_CU_ASSERT_FATAL(rc != true);
	TAILQ_REMOVE(&tcp_req.tqpair->outstanding_reqs, &tcp_req, link);

	/* Test case 3: in completion context. Expect: PASS */
	tqpair.qpair.in_completion_context = 1;
	tqpair.async_complete = 0;
	tcp_req.ordering.bits.send_ack = 1;
	tcp_req.ordering.bits.data_recv = 1;
	tcp_req.state = NVME_TCP_REQ_ACTIVE;
	TAILQ_INSERT_TAIL(&tcp_req.tqpair->outstanding_reqs, &tcp_req, link);

	rc = nvme_tcp_req_complete_safe(&tcp_req);
	CU_ASSERT(rc == true);
	CU_ASSERT(tcp_req.tqpair->async_complete == 0);

	/* Test case 4: in async complete. Expect: PASS */
	tqpair.qpair.in_completion_context = 0;
	tcp_req.ordering.bits.send_ack = 1;
	tcp_req.ordering.bits.data_recv = 1;
	tcp_req.state = NVME_TCP_REQ_ACTIVE;
	TAILQ_INSERT_TAIL(&tcp_req.tqpair->outstanding_reqs, &tcp_req, link);

	rc = nvme_tcp_req_complete_safe(&tcp_req);
	CU_ASSERT(rc == true);
	CU_ASSERT(tcp_req.tqpair->async_complete);
}

static void
test_nvme_tcp_req_init(void)
{
	struct nvme_tcp_qpair tqpair = {};
	struct nvme_request req = {};
	struct nvme_tcp_req tcp_req = {0};
	struct spdk_nvme_ctrlr ctrlr = {{0}};
	struct nvme_tcp_ut_bdev_io bio = {};
	int rc;

	tqpair.qpair.ctrlr = &ctrlr;
	req.qpair = &tqpair.qpair;

	tcp_req.cid = 1;
	req.payload.next_sge_fn = nvme_tcp_ut_next_sge;
	req.payload.contig_or_cb_arg = &bio;
	req.payload_offset = 0;
	req.payload_size = 4096;
	ctrlr.max_sges = NVME_TCP_MAX_SGL_DESCRIPTORS;
	ctrlr.ioccsz_bytes = 1024;
	bio.iovpos = 0;
	bio.iovs[0].iov_len = 8192;
	bio.iovs[0].iov_base = (void *)0xDEADBEEF;

	/* Test case1: payload type SGL. Expect: PASS */
	req.cmd.opc = SPDK_NVME_DATA_HOST_TO_CONTROLLER;
	req.payload.reset_sgl_fn = nvme_tcp_ut_reset_sgl;

	rc = nvme_tcp_req_init(&tqpair, &req, &tcp_req);
	CU_ASSERT(rc == 0);
	CU_ASSERT(tcp_req.req == &req);
	CU_ASSERT(tcp_req.in_capsule_data == true);
	CU_ASSERT(tcp_req.iovcnt == 1);
	CU_ASSERT(tcp_req.iov[0].iov_len == req.payload_size);
	CU_ASSERT(tcp_req.iov[0].iov_base == bio.iovs[0].iov_base);
	CU_ASSERT(req.cmd.cid == tcp_req.cid);
	CU_ASSERT(req.cmd.psdt == SPDK_NVME_PSDT_SGL_MPTR_CONTIG);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.length == req.payload_size);
	CU_ASSERT(req.cmd.dptr.sgl1.address == 0);

	/* Test case2: payload type CONTIG. Expect: PASS */
	memset(&req.cmd, 0, sizeof(req.cmd));
	memset(&tcp_req, 0, sizeof(tcp_req));
	tcp_req.cid = 1;
	req.payload.reset_sgl_fn = NULL;
	req.cmd.opc = SPDK_NVME_DATA_HOST_TO_CONTROLLER;

	rc = nvme_tcp_req_init(&tqpair, &req, &tcp_req);
	CU_ASSERT(rc == 0);
	CU_ASSERT(tcp_req.req == &req);
	CU_ASSERT(tcp_req.in_capsule_data == true);
	CU_ASSERT(tcp_req.iov[0].iov_len == req.payload_size);
	CU_ASSERT(tcp_req.iov[0].iov_base == &bio);
	CU_ASSERT(tcp_req.iovcnt == 1);
	CU_ASSERT(req.cmd.cid == tcp_req.cid);
	CU_ASSERT(req.cmd.psdt == SPDK_NVME_PSDT_SGL_MPTR_CONTIG);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.length == req.payload_size);
	CU_ASSERT(req.cmd.dptr.sgl1.address == 0);

}

static void
test_nvme_tcp_req_get(void)
{
	struct nvme_tcp_req tcp_req = {0};
	struct nvme_tcp_qpair tqpair = {};
	struct nvme_tcp_pdu send_pdu = {};

	tcp_req.pdu = &send_pdu;
	tcp_req.state = NVME_TCP_REQ_FREE;

	TAILQ_INIT(&tqpair.free_reqs);
	TAILQ_INIT(&tqpair.outstanding_reqs);
	TAILQ_INSERT_HEAD(&tqpair.free_reqs, &tcp_req, link);

	CU_ASSERT(nvme_tcp_req_get(&tqpair) == &tcp_req);
	CU_ASSERT(tcp_req.state == NVME_TCP_REQ_ACTIVE);
	CU_ASSERT(tcp_req.datao == 0);
	CU_ASSERT(tcp_req.req == NULL);
	CU_ASSERT(tcp_req.in_capsule_data == false);
	CU_ASSERT(tcp_req.r2tl_remain == 0);
	CU_ASSERT(tcp_req.iovcnt == 0);
	CU_ASSERT(tcp_req.ordering.raw == 0);
	CU_ASSERT(!TAILQ_EMPTY(&tqpair.outstanding_reqs));
	CU_ASSERT(TAILQ_EMPTY(&tqpair.free_reqs));

	/* No tcp request available, expect fail */
	SPDK_CU_ASSERT_FATAL(nvme_tcp_req_get(&tqpair) == NULL);
}

static void
test_nvme_tcp_qpair_capsule_cmd_send(void)
{
	struct nvme_tcp_qpair tqpair = {};
	struct spdk_nvme_tcp_stat stats = {};
	struct nvme_tcp_req tcp_req = {};
	struct nvme_tcp_pdu pdu = {};
	struct nvme_request req = {};
	char iov_base0[4096];
	char iov_base1[4096];
	uint32_t plen;
	uint8_t pdo;

	memset(iov_base0, 0xFF, 4096);
	memset(iov_base1, 0xFF, 4096);
	tcp_req.req = &req;
	tcp_req.pdu = &pdu;
	TAILQ_INIT(&tqpair.send_queue);
	tqpair.stats = &stats;

	tcp_req.iov[0].iov_base = (void *)iov_base0;
	tcp_req.iov[0].iov_len = 4096;
	tcp_req.iov[1].iov_base = (void *)iov_base1;
	tcp_req.iov[1].iov_len = 4096;
	tcp_req.iovcnt = 2;
	tcp_req.req->payload_size = 8192;
	tcp_req.in_capsule_data = true;
	tqpair.cpda = NVME_TCP_HPDA_DEFAULT;

	/* Test case 1: host hdgst and ddgst enable. Expect: PASS */
	tqpair.flags.host_hdgst_enable = 1;
	tqpair.flags.host_ddgst_enable = 1;
	pdo = plen = sizeof(struct spdk_nvme_tcp_cmd) +
		     SPDK_NVME_TCP_DIGEST_LEN;
	plen += tcp_req.req->payload_size;
	plen += SPDK_NVME_TCP_DIGEST_LEN;

	nvme_tcp_qpair_capsule_cmd_send(&tqpair, &tcp_req);
	TAILQ_REMOVE(&tqpair.send_queue, &pdu, tailq);
	CU_ASSERT(pdu.hdr.capsule_cmd.common.flags
		  & SPDK_NVME_TCP_CH_FLAGS_HDGSTF);
	CU_ASSERT(pdu.hdr.capsule_cmd.common.flags
		  & SPDK_NVME_TCP_CH_FLAGS_DDGSTF);
	CU_ASSERT(pdu.hdr.capsule_cmd.common.pdu_type ==
		  SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD);
	CU_ASSERT(pdu.hdr.capsule_cmd.common.pdo == pdo);
	CU_ASSERT(pdu.hdr.capsule_cmd.common.plen == plen);
	CU_ASSERT(pdu.data_iov[0].iov_base == tcp_req.iov[0].iov_base);
	CU_ASSERT(pdu.data_iov[0].iov_len == tcp_req.iov[0].iov_len);
	CU_ASSERT(pdu.data_iov[1].iov_base == tcp_req.iov[1].iov_base);
	CU_ASSERT(pdu.data_iov[1].iov_len == tcp_req.iov[0].iov_len);

	/* Test case 2: host hdgst and ddgst disable. Expect: PASS */
	memset(&pdu, 0, sizeof(pdu));
	tqpair.flags.host_hdgst_enable = 0;
	tqpair.flags.host_ddgst_enable = 0;

	pdo = plen = sizeof(struct spdk_nvme_tcp_cmd);
	plen += tcp_req.req->payload_size;

	nvme_tcp_qpair_capsule_cmd_send(&tqpair, &tcp_req);
	TAILQ_REMOVE(&tqpair.send_queue, &pdu, tailq);
	CU_ASSERT(pdu.hdr.capsule_cmd.common.flags == 0)
	CU_ASSERT(pdu.hdr.capsule_cmd.common.pdu_type ==
		  SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD);
	CU_ASSERT(pdu.hdr.capsule_cmd.common.pdo == pdo);
	CU_ASSERT(pdu.hdr.capsule_cmd.common.plen == plen);
	CU_ASSERT(pdu.data_iov[0].iov_base == tcp_req.iov[0].iov_base);
	CU_ASSERT(pdu.data_iov[0].iov_len == tcp_req.iov[0].iov_len);
	CU_ASSERT(pdu.data_iov[1].iov_base == tcp_req.iov[1].iov_base);
	CU_ASSERT(pdu.data_iov[1].iov_len == tcp_req.iov[0].iov_len);

	/* Test case 3: padding available. Expect: PASS */
	memset(&pdu, 0, sizeof(pdu));
	tqpair.flags.host_hdgst_enable = 1;
	tqpair.flags.host_ddgst_enable = 1;
	tqpair.cpda = SPDK_NVME_TCP_CPDA_MAX;

	pdo = plen = (SPDK_NVME_TCP_CPDA_MAX + 1) << 2;
	plen += tcp_req.req->payload_size;
	plen += SPDK_NVME_TCP_DIGEST_LEN;

	nvme_tcp_qpair_capsule_cmd_send(&tqpair, &tcp_req);
	TAILQ_REMOVE(&tqpair.send_queue, &pdu, tailq);
	CU_ASSERT(pdu.hdr.capsule_cmd.common.flags
		  & SPDK_NVME_TCP_CH_FLAGS_HDGSTF);
	CU_ASSERT(pdu.hdr.capsule_cmd.common.flags
		  & SPDK_NVME_TCP_CH_FLAGS_DDGSTF);
	CU_ASSERT(pdu.hdr.capsule_cmd.common.pdu_type ==
		  SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD);
	CU_ASSERT(pdu.hdr.capsule_cmd.common.pdo == pdo);
	CU_ASSERT(pdu.hdr.capsule_cmd.common.plen == plen);
	CU_ASSERT(pdu.data_iov[0].iov_base == tcp_req.iov[0].iov_base);
	CU_ASSERT(pdu.data_iov[0].iov_len == tcp_req.iov[0].iov_len);
	CU_ASSERT(pdu.data_iov[1].iov_base == tcp_req.iov[1].iov_base);
	CU_ASSERT(pdu.data_iov[1].iov_len == tcp_req.iov[0].iov_len);
}

/* Just define, nothing to do */
static void
ut_nvme_tcp_qpair_xfer_complete_cb(void *cb_arg)
{
	return;
}

static void
test_nvme_tcp_qpair_write_pdu(void)
{
	struct nvme_tcp_qpair tqpair = {};
	struct spdk_nvme_tcp_stat stats = {};
	struct nvme_tcp_pdu pdu = {};
	void *cb_arg = (void *)0xDEADBEEF;
	char iov_base0[4096];
	char iov_base1[4096];

	memset(iov_base0, 0xFF, 4096);
	memset(iov_base1, 0xFF, 4096);
	pdu.data_len = 4096 * 2;
	pdu.padding_len = 0;
	pdu.data_iov[0].iov_base = (void *)iov_base0;
	pdu.data_iov[0].iov_len = 4096;
	pdu.data_iov[1].iov_base = (void *)iov_base1;
	pdu.data_iov[1].iov_len = 4096;
	pdu.data_iovcnt = 2;
	TAILQ_INIT(&tqpair.send_queue);

	/* Test case1: host hdgst and ddgst enable Expect: PASS */
	memset(pdu.hdr.raw, 0, SPDK_NVME_TCP_TERM_REQ_PDU_MAX_SIZE);
	memset(pdu.data_digest, 0, SPDK_NVME_TCP_DIGEST_LEN);

	pdu.hdr.common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD;
	pdu.hdr.common.hlen = sizeof(struct spdk_nvme_tcp_cmd);
	pdu.hdr.common.plen = pdu.hdr.common.hlen +
			      SPDK_NVME_TCP_DIGEST_LEN * 2 ;
	pdu.hdr.common.plen += pdu.data_len;
	tqpair.flags.host_hdgst_enable = 1;
	tqpair.flags.host_ddgst_enable = 1;
	tqpair.stats = &stats;

	nvme_tcp_qpair_write_pdu(&tqpair,
				 &pdu,
				 ut_nvme_tcp_qpair_xfer_complete_cb,
				 cb_arg);
	TAILQ_REMOVE(&tqpair.send_queue, &pdu, tailq);
	/* Check the crc data of header digest filled into raw */
	CU_ASSERT(pdu.hdr.raw[pdu.hdr.common.hlen]);
	CU_ASSERT(pdu.data_digest[0]);
	CU_ASSERT(pdu.sock_req.iovcnt == 4);
	CU_ASSERT(pdu.iov[0].iov_base == &pdu.hdr.raw);
	CU_ASSERT(pdu.iov[0].iov_len == (sizeof(struct spdk_nvme_tcp_cmd) +
					 SPDK_NVME_TCP_DIGEST_LEN));
	CU_ASSERT(pdu.iov[1].iov_base == pdu.data_iov[0].iov_base);
	CU_ASSERT(pdu.iov[1].iov_len == pdu.data_iov[0].iov_len);
	CU_ASSERT(pdu.iov[2].iov_base == pdu.data_iov[1].iov_base);
	CU_ASSERT(pdu.iov[2].iov_len == pdu.data_iov[1].iov_len);
	CU_ASSERT(pdu.iov[3].iov_base == &pdu.data_digest);
	CU_ASSERT(pdu.iov[3].iov_len == SPDK_NVME_TCP_DIGEST_LEN);
	CU_ASSERT(pdu.cb_fn == ut_nvme_tcp_qpair_xfer_complete_cb);
	CU_ASSERT(pdu.cb_arg == cb_arg);
	CU_ASSERT(pdu.qpair == &tqpair);
	CU_ASSERT(pdu.sock_req.cb_arg == (void *)&pdu);

	/* Test case2: host hdgst and ddgst disable Expect: PASS */
	memset(pdu.hdr.raw, 0, SPDK_NVME_TCP_TERM_REQ_PDU_MAX_SIZE);
	memset(pdu.data_digest, 0, SPDK_NVME_TCP_DIGEST_LEN);

	pdu.hdr.common.hlen = sizeof(struct spdk_nvme_tcp_cmd);
	pdu.hdr.common.plen = pdu.hdr.common.hlen  + pdu.data_len;
	tqpair.flags.host_hdgst_enable = 0;
	tqpair.flags.host_ddgst_enable = 0;

	nvme_tcp_qpair_write_pdu(&tqpair,
				 &pdu,
				 ut_nvme_tcp_qpair_xfer_complete_cb,
				 cb_arg);
	TAILQ_REMOVE(&tqpair.send_queue, &pdu, tailq);
	CU_ASSERT(pdu.hdr.raw[pdu.hdr.common.hlen] == 0);
	CU_ASSERT(pdu.data_digest[0] == 0);
	CU_ASSERT(pdu.sock_req.iovcnt == 3);
	CU_ASSERT(pdu.iov[0].iov_base == &pdu.hdr.raw);
	CU_ASSERT(pdu.iov[0].iov_len == sizeof(struct spdk_nvme_tcp_cmd));
	CU_ASSERT(pdu.iov[1].iov_base == pdu.data_iov[0].iov_base);
	CU_ASSERT(pdu.iov[1].iov_len == pdu.data_iov[0].iov_len);
	CU_ASSERT(pdu.iov[2].iov_base == pdu.data_iov[1].iov_base);
	CU_ASSERT(pdu.iov[2].iov_len == pdu.data_iov[1].iov_len);
	CU_ASSERT(pdu.cb_fn == ut_nvme_tcp_qpair_xfer_complete_cb);
	CU_ASSERT(pdu.cb_arg == cb_arg);
	CU_ASSERT(pdu.qpair == &tqpair);
	CU_ASSERT(pdu.sock_req.cb_arg == (void *)&pdu);
}

static void
test_nvme_tcp_qpair_set_recv_state(void)
{
	struct nvme_tcp_qpair tqpair = {};
	enum nvme_tcp_pdu_recv_state state;
	struct nvme_tcp_pdu recv_pdu = {};

	tqpair.recv_pdu = &recv_pdu;

	/* case1: The recv state of tqpair is same with the state to be set */
	tqpair.recv_state = NVME_TCP_PDU_RECV_STATE_ERROR;
	state = NVME_TCP_PDU_RECV_STATE_ERROR;
	nvme_tcp_qpair_set_recv_state(&tqpair, state);
	CU_ASSERT(tqpair.recv_state == state);

	/* case2: The recv state of tqpair is different with the state to be set */
	/* state is NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY or NVME_TCP_PDU_RECV_STATE_ERROR, tqpair->recv_pdu will be cleared */
	tqpair.recv_pdu->cb_arg = (void *)0xDEADBEEF;
	state = NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY;
	nvme_tcp_qpair_set_recv_state(&tqpair, state);
	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
	CU_ASSERT(tqpair.recv_pdu->cb_arg == (void *)0x0);

	tqpair.recv_pdu->cb_arg = (void *)0xDEADBEEF;
	state = NVME_TCP_PDU_RECV_STATE_ERROR;
	nvme_tcp_qpair_set_recv_state(&tqpair, state);
	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_ERROR);
	CU_ASSERT(tqpair.recv_pdu->cb_arg == (void *)0x0);

	/* state is NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH or NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH or NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD or default */
	state = NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH;
	nvme_tcp_qpair_set_recv_state(&tqpair, state);
	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH);

	state = NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH;
	nvme_tcp_qpair_set_recv_state(&tqpair, state);
	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH);

	state = NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD;
	nvme_tcp_qpair_set_recv_state(&tqpair, state);
	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD);

	state = 0xff;
	nvme_tcp_qpair_set_recv_state(&tqpair, state);
	CU_ASSERT(tqpair.recv_state == 0xff);
}

static void
test_nvme_tcp_alloc_reqs(void)
{
	struct nvme_tcp_qpair tqpair = {};
	int rc = 0;

	/* case1: single entry. Expect: PASS */
	tqpair.num_entries = 1;
	rc = nvme_tcp_alloc_reqs(&tqpair);
	CU_ASSERT(rc == 0);
	CU_ASSERT(tqpair.tcp_reqs[0].cid == 0);
	CU_ASSERT(tqpair.tcp_reqs[0].tqpair == &tqpair);
	CU_ASSERT(tqpair.tcp_reqs[0].pdu == &tqpair.send_pdus[0]);
	CU_ASSERT(tqpair.send_pdu == &tqpair.send_pdus[tqpair.num_entries]);
	free(tqpair.tcp_reqs);
	spdk_free(tqpair.send_pdus);

	/* case2: multiple entries. Expect: PASS */
	tqpair.num_entries = 5;
	rc = nvme_tcp_alloc_reqs(&tqpair);
	CU_ASSERT(rc == 0);
	for (int i = 0; i < tqpair.num_entries; i++) {
		CU_ASSERT(tqpair.tcp_reqs[i].cid == i);
		CU_ASSERT(tqpair.tcp_reqs[i].tqpair == &tqpair);
		CU_ASSERT(tqpair.tcp_reqs[i].pdu == &tqpair.send_pdus[i]);
	}
	CU_ASSERT(tqpair.send_pdu == &tqpair.send_pdus[tqpair.num_entries]);

	/* case3: Test nvme_tcp_free_reqs test. Expect: PASS */
	nvme_tcp_free_reqs(&tqpair);
	CU_ASSERT(tqpair.tcp_reqs == NULL);
	CU_ASSERT(tqpair.send_pdus == NULL);
}

static void
test_nvme_tcp_parse_addr(void)
{
	struct sockaddr_storage dst_addr;
	int rc = 0;

	memset(&dst_addr, 0, sizeof(dst_addr));
	/* case1: getaddrinfo failed */
	rc = nvme_tcp_parse_addr(&dst_addr, AF_INET, NULL, NULL);
	CU_ASSERT(rc != 0);

	/* case2: res->ai_addrlen < sizeof(*sa). Expect: Pass. */
	rc = nvme_tcp_parse_addr(&dst_addr, AF_INET, "12.34.56.78", "23");
	CU_ASSERT(rc == 0);
	CU_ASSERT(dst_addr.ss_family == AF_INET);
}

static void
test_nvme_tcp_qpair_send_h2c_term_req(void)
{
	struct nvme_tcp_qpair tqpair = {};
	struct spdk_nvme_tcp_stat stats = {};
	struct nvme_tcp_pdu pdu = {}, recv_pdu = {}, send_pdu = {};
	enum spdk_nvme_tcp_term_req_fes fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
	uint32_t error_offset = 1;

	tqpair.send_pdu = &send_pdu;
	tqpair.recv_pdu = &recv_pdu;
	tqpair.stats = &stats;
	TAILQ_INIT(&tqpair.send_queue);
	/* case1: hlen < SPDK_NVME_TCP_TERM_REQ_ERROR_DATA_MAX_SIZE, Expect: copy_len == hlen */
	pdu.hdr.common.hlen = 64;
	nvme_tcp_qpair_send_h2c_term_req(&tqpair, &pdu, fes, error_offset);
	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_ERROR);
	CU_ASSERT(tqpair.send_pdu->hdr.term_req.common.hlen == sizeof(struct spdk_nvme_tcp_term_req_hdr));
	CU_ASSERT(tqpair.send_pdu->hdr.term_req.common.plen == tqpair.send_pdu->hdr.term_req.common.hlen +
		  pdu.hdr.common.hlen);
	CU_ASSERT(tqpair.send_pdu->hdr.term_req.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ);

	/* case2: hlen > SPDK_NVME_TCP_TERM_REQ_ERROR_DATA_MAX_SIZE, Expect: copy_len == SPDK_NVME_TCP_TERM_REQ_ERROR_DATA_MAX_SIZE */
	pdu.hdr.common.hlen = 255;
	nvme_tcp_qpair_send_h2c_term_req(&tqpair, &pdu, fes, error_offset);
	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_ERROR);
	CU_ASSERT(tqpair.send_pdu->hdr.term_req.common.hlen == sizeof(struct spdk_nvme_tcp_term_req_hdr));
	CU_ASSERT(tqpair.send_pdu->hdr.term_req.common.plen == (unsigned)
		  tqpair.send_pdu->hdr.term_req.common.hlen + SPDK_NVME_TCP_TERM_REQ_ERROR_DATA_MAX_SIZE);
	CU_ASSERT(tqpair.send_pdu->hdr.term_req.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ);
}

static void
test_nvme_tcp_pdu_ch_handle(void)
{
	struct nvme_tcp_qpair tqpair = {};
	struct spdk_nvme_tcp_stat stats = {};
	struct nvme_tcp_pdu send_pdu = {}, recv_pdu = {};

	tqpair.send_pdu = &send_pdu;
	tqpair.recv_pdu = &recv_pdu;
	tqpair.stats = &stats;
	TAILQ_INIT(&tqpair.send_queue);
	/* case 1: Already received IC_RESP PDU. Expect: fail */
	tqpair.recv_pdu->hdr.common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_IC_RESP;
	tqpair.state = NVME_TCP_QPAIR_STATE_INITIALIZING;
	nvme_tcp_pdu_ch_handle(&tqpair);
	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_ERROR);
	CU_ASSERT(tqpair.send_pdu->hdr.term_req.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ);
	CU_ASSERT(tqpair.send_pdu->hdr.term_req.common.hlen == sizeof(struct spdk_nvme_tcp_term_req_hdr));
	CU_ASSERT(tqpair.send_pdu->hdr.term_req.common.plen == tqpair.send_pdu->hdr.term_req.common.hlen);

	/* case 2: Expected PDU header length and received are different. Expect: fail */
	tqpair.recv_pdu->hdr.common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_IC_RESP;
	tqpair.state = NVME_TCP_QPAIR_STATE_INVALID;
	tqpair.recv_pdu->hdr.common.plen = sizeof(struct spdk_nvme_tcp_ic_resp);
	tqpair.recv_pdu->hdr.common.hlen = 0;
	nvme_tcp_pdu_ch_handle(&tqpair);
	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_ERROR);
	CU_ASSERT(tqpair.send_pdu->hdr.term_req.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ);
	CU_ASSERT(tqpair.send_pdu->hdr.term_req.common.hlen == sizeof(struct spdk_nvme_tcp_term_req_hdr));
	CU_ASSERT(tqpair.send_pdu->hdr.term_req.common.plen == tqpair.send_pdu->hdr.term_req.common.hlen);
	CU_ASSERT(tqpair.send_pdu->hdr.term_req.fei[0] == 2);

	/* case 3: The TCP/IP tqpair connection is not negotiated. Expect: fail */
	tqpair.recv_pdu->hdr.common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_CAPSULE_RESP;
	tqpair.state = NVME_TCP_QPAIR_STATE_INVALID;
	tqpair.recv_pdu->hdr.common.plen = sizeof(struct spdk_nvme_tcp_ic_resp);
	tqpair.recv_pdu->hdr.common.hlen = 0;
	nvme_tcp_pdu_ch_handle(&tqpair);
	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_ERROR);
	CU_ASSERT(tqpair.send_pdu->hdr.term_req.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ);
	CU_ASSERT(tqpair.send_pdu->hdr.term_req.common.hlen == sizeof(struct spdk_nvme_tcp_term_req_hdr));
	CU_ASSERT(tqpair.send_pdu->hdr.term_req.common.plen == tqpair.send_pdu->hdr.term_req.common.hlen);

	/* case 4: Unexpected PDU type. Expect: fail */
	tqpair.recv_pdu->hdr.common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_IC_REQ;
	tqpair.state = NVME_TCP_QPAIR_STATE_RUNNING;
	tqpair.recv_pdu->hdr.common.plen = 0;
	tqpair.recv_pdu->hdr.common.hlen = sizeof(struct spdk_nvme_tcp_ic_resp);
	nvme_tcp_pdu_ch_handle(&tqpair);
	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_ERROR);
	CU_ASSERT(tqpair.send_pdu->hdr.term_req.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ);
	CU_ASSERT(tqpair.send_pdu->hdr.term_req.common.hlen == sizeof(struct spdk_nvme_tcp_term_req_hdr));
	CU_ASSERT(tqpair.send_pdu->hdr.term_req.common.plen == tqpair.send_pdu->hdr.term_req.common.hlen +
		  (unsigned)SPDK_NVME_TCP_TERM_REQ_ERROR_DATA_MAX_SIZE);

	/* case 5: plen error. Expect: fail */
	tqpair.recv_pdu->hdr.common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_IC_RESP;
	tqpair.state = NVME_TCP_QPAIR_STATE_INVALID;
	tqpair.recv_pdu->hdr.common.plen = 0;
	tqpair.recv_pdu->hdr.common.hlen = sizeof(struct spdk_nvme_tcp_ic_resp);
	nvme_tcp_pdu_ch_handle(&tqpair);
	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_ERROR);
	CU_ASSERT(tqpair.send_pdu->hdr.term_req.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ);
	CU_ASSERT(tqpair.send_pdu->hdr.term_req.common.hlen == sizeof(struct spdk_nvme_tcp_term_req_hdr));
	CU_ASSERT(tqpair.send_pdu->hdr.term_req.common.plen == tqpair.send_pdu->hdr.term_req.common.hlen +
		  (unsigned)SPDK_NVME_TCP_TERM_REQ_ERROR_DATA_MAX_SIZE);
	CU_ASSERT(tqpair.send_pdu->hdr.term_req.fei[0] == 4);

	tqpair.recv_pdu->hdr.common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_CAPSULE_RESP;
	tqpair.state = NVME_TCP_QPAIR_STATE_RUNNING;
	tqpair.recv_pdu->hdr.common.flags = SPDK_NVME_TCP_CH_FLAGS_HDGSTF;
	tqpair.recv_pdu->hdr.common.plen = 0;
	tqpair.recv_pdu->hdr.common.hlen = sizeof(struct spdk_nvme_tcp_rsp);
	nvme_tcp_pdu_ch_handle(&tqpair);
	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_ERROR);
	CU_ASSERT(tqpair.send_pdu->hdr.term_req.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ);
	CU_ASSERT(tqpair.send_pdu->hdr.term_req.common.hlen == sizeof(struct spdk_nvme_tcp_term_req_hdr));
	CU_ASSERT(tqpair.send_pdu->hdr.term_req.common.plen == tqpair.send_pdu->hdr.term_req.common.hlen +
		  (unsigned)sizeof(struct spdk_nvme_tcp_term_req_hdr));
	CU_ASSERT(tqpair.send_pdu->hdr.term_req.fei[0] == 4);

	tqpair.recv_pdu->hdr.common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_C2H_DATA;
	tqpair.state = NVME_TCP_QPAIR_STATE_RUNNING;
	tqpair.recv_pdu->hdr.common.plen = 0;
	tqpair.recv_pdu->hdr.common.pdo = 64;
	tqpair.recv_pdu->hdr.common.hlen = sizeof(struct spdk_nvme_tcp_c2h_data_hdr);
	nvme_tcp_pdu_ch_handle(&tqpair);
	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_ERROR);
	CU_ASSERT(tqpair.send_pdu->hdr.term_req.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ);
	CU_ASSERT(tqpair.send_pdu->hdr.term_req.common.hlen == sizeof(struct spdk_nvme_tcp_term_req_hdr));
	CU_ASSERT(tqpair.send_pdu->hdr.term_req.common.plen == tqpair.send_pdu->hdr.term_req.common.hlen +
		  (unsigned)sizeof(struct spdk_nvme_tcp_term_req_hdr));
	CU_ASSERT(tqpair.send_pdu->hdr.term_req.fei[0] == 4);

	tqpair.recv_pdu->hdr.common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ;
	tqpair.state = NVME_TCP_QPAIR_STATE_RUNNING;
	tqpair.recv_pdu->hdr.common.plen = 0;
	tqpair.recv_pdu->hdr.common.hlen = sizeof(struct spdk_nvme_tcp_term_req_hdr);
	nvme_tcp_pdu_ch_handle(&tqpair);
	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_ERROR);
	CU_ASSERT(tqpair.send_pdu->hdr.term_req.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ);
	CU_ASSERT(tqpair.send_pdu->hdr.term_req.common.hlen == sizeof(struct spdk_nvme_tcp_term_req_hdr));
	CU_ASSERT(tqpair.send_pdu->hdr.term_req.common.plen == tqpair.send_pdu->hdr.term_req.common.hlen +
		  (unsigned)sizeof(struct spdk_nvme_tcp_term_req_hdr));
	CU_ASSERT(tqpair.send_pdu->hdr.term_req.fei[0] == 4);

	tqpair.recv_pdu->hdr.common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_R2T;
	tqpair.state = NVME_TCP_QPAIR_STATE_RUNNING;
	tqpair.recv_pdu->hdr.common.flags = SPDK_NVME_TCP_CH_FLAGS_HDGSTF;
	tqpair.recv_pdu->hdr.common.plen = 0;
	tqpair.recv_pdu->hdr.common.hlen = sizeof(struct spdk_nvme_tcp_r2t_hdr);
	nvme_tcp_pdu_ch_handle(&tqpair);
	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_ERROR);
	CU_ASSERT(tqpair.send_pdu->hdr.term_req.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ);
	CU_ASSERT(tqpair.send_pdu->hdr.term_req.common.hlen == sizeof(struct spdk_nvme_tcp_term_req_hdr));
	CU_ASSERT(tqpair.send_pdu->hdr.term_req.common.plen == tqpair.send_pdu->hdr.term_req.common.hlen +
		  (unsigned)sizeof(struct spdk_nvme_tcp_r2t_hdr));
	CU_ASSERT(tqpair.send_pdu->hdr.term_req.fei[0] == 4);

	/* case 6: Expect:  PASS */
	tqpair.recv_pdu->hdr.common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_IC_RESP;
	tqpair.state = NVME_TCP_QPAIR_STATE_INVALID;
	tqpair.recv_pdu->hdr.common.plen = sizeof(struct spdk_nvme_tcp_ic_resp);
	tqpair.recv_pdu->hdr.common.hlen = sizeof(struct spdk_nvme_tcp_ic_resp);
	nvme_tcp_pdu_ch_handle(&tqpair);
	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH);
	CU_ASSERT(tqpair.recv_pdu->psh_len == tqpair.recv_pdu->hdr.common.hlen - sizeof(
			  struct spdk_nvme_tcp_common_pdu_hdr));
}

DEFINE_RETURN_MOCK(spdk_sock_connect_ext, struct spdk_sock *);
struct spdk_sock *
spdk_sock_connect_ext(const char *ip, int port,
		      char *_impl_name, struct spdk_sock_opts *opts)
{
	HANDLE_RETURN_MOCK(spdk_sock_connect_ext);
	CU_ASSERT(port == 23);
	CU_ASSERT(opts->opts_size == sizeof(*opts));
	CU_ASSERT(opts->priority == 1);
	CU_ASSERT(opts->zcopy == true);
	CU_ASSERT(!strcmp(ip, "192.168.1.78"));
	return (struct spdk_sock *)0xDDADBEEF;
}

static void
test_nvme_tcp_qpair_connect_sock(void)
{
	struct spdk_nvme_ctrlr ctrlr = {};
	struct nvme_tcp_qpair tqpair = {};
	int rc;

	tqpair.qpair.trtype = SPDK_NVME_TRANSPORT_TCP;
	tqpair.qpair.id = 1;
	tqpair.qpair.poll_group = (void *)0xDEADBEEF;
	ctrlr.trid.priority = 1;
	ctrlr.trid.adrfam = SPDK_NVMF_ADRFAM_IPV4;
	memcpy(ctrlr.trid.traddr, "192.168.1.78", sizeof("192.168.1.78"));
	memcpy(ctrlr.trid.trsvcid, "23", sizeof("23"));
	memcpy(ctrlr.opts.src_addr, "192.168.1.77", sizeof("192.168.1.77"));
	memcpy(ctrlr.opts.src_svcid, "23", sizeof("23"));

	rc = nvme_tcp_qpair_connect_sock(&ctrlr, &tqpair.qpair);
	CU_ASSERT(rc == 0);

	/* Unsupported family of the transport address */
	ctrlr.trid.adrfam = SPDK_NVMF_ADRFAM_IB;

	rc = nvme_tcp_qpair_connect_sock(&ctrlr, &tqpair.qpair);
	SPDK_CU_ASSERT_FATAL(rc == -1);

	/* Invalid dst_port, INT_MAX is 2147483647 */
	ctrlr.trid.adrfam = SPDK_NVMF_ADRFAM_IPV4;
	memcpy(ctrlr.trid.trsvcid, "2147483647", sizeof("2147483647"));

	rc = nvme_tcp_qpair_connect_sock(&ctrlr, &tqpair.qpair);
	SPDK_CU_ASSERT_FATAL(rc == -1);

	/* Parse invalid address */
	memcpy(ctrlr.trid.trsvcid, "23", sizeof("23"));
	memcpy(ctrlr.trid.traddr, "192.168.1.256", sizeof("192.168.1.256"));

	rc = nvme_tcp_qpair_connect_sock(&ctrlr, &tqpair.qpair);
	SPDK_CU_ASSERT_FATAL(rc != 0);
}

static void
test_nvme_tcp_qpair_icreq_send(void)
{
	struct nvme_tcp_qpair tqpair = {};
	struct spdk_nvme_tcp_stat stats = {};
	struct spdk_nvme_ctrlr ctrlr = {};
	struct nvme_tcp_pdu pdu = {};
	struct nvme_tcp_poll_group poll_group = {};
	struct spdk_nvme_tcp_ic_req *ic_req = NULL;
	int rc;

	tqpair.send_pdu = &pdu;
	tqpair.qpair.ctrlr = &ctrlr;
	tqpair.qpair.poll_group = &poll_group.group;
	tqpair.stats = &stats;
	ic_req = &pdu.hdr.ic_req;

	tqpair.state = NVME_TCP_QPAIR_STATE_RUNNING;
	tqpair.qpair.ctrlr->opts.header_digest = true;
	tqpair.qpair.ctrlr->opts.data_digest = true;
	TAILQ_INIT(&tqpair.send_queue);

	rc = nvme_tcp_qpair_icreq_send(&tqpair);
	CU_ASSERT(rc == 0);
	CU_ASSERT(ic_req->common.hlen == sizeof(*ic_req));
	CU_ASSERT(ic_req->common.plen == sizeof(*ic_req));
	CU_ASSERT(ic_req->common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_IC_REQ);
	CU_ASSERT(ic_req->pfv == 0);
	CU_ASSERT(ic_req->maxr2t == NVME_TCP_MAX_R2T_DEFAULT - 1);
	CU_ASSERT(ic_req->hpda == NVME_TCP_HPDA_DEFAULT);
	CU_ASSERT(ic_req->dgst.bits.hdgst_enable == true);
	CU_ASSERT(ic_req->dgst.bits.ddgst_enable == true);
}

static void
test_nvme_tcp_c2h_payload_handle(void)
{
	struct nvme_tcp_qpair tqpair = {};
	struct spdk_nvme_tcp_stat stats = {};
	struct nvme_tcp_pdu pdu = {};
	struct nvme_tcp_req tcp_req = {};
	struct nvme_request	req = {};
	struct nvme_tcp_pdu recv_pdu = {};
	uint32_t reaped = 1;

	tcp_req.req = &req;
	tcp_req.req->qpair = &tqpair.qpair;
	tcp_req.req->cb_fn = ut_nvme_complete_request;
	tcp_req.tqpair = &tqpair;
	tcp_req.cid = 1;
	tqpair.stats = &stats;

	TAILQ_INIT(&tcp_req.tqpair->outstanding_reqs);

	pdu.req = &tcp_req;
	pdu.hdr.c2h_data.common.flags = SPDK_NVME_TCP_C2H_DATA_FLAGS_SUCCESS |
					SPDK_NVME_TCP_C2H_DATA_FLAGS_LAST_PDU;
	pdu.data_len = 1024;

	tqpair.qpair.id = 1;
	tqpair.recv_pdu = &recv_pdu;

	/* case 1: nvme_tcp_c2h_data_payload_handle: tcp_req->datao != tcp_req->req->payload_size */
	tcp_req.datao = 1024;
	tcp_req.req->payload_size = 2048;
	tcp_req.state = NVME_TCP_REQ_ACTIVE;
	tcp_req.ordering.bits.send_ack = 1;
	memset(&tcp_req.rsp, 0, sizeof(tcp_req.rsp));
	tcp_req.ordering.bits.data_recv = 0;
	tqpair.recv_state = NVME_TCP_PDU_RECV_STATE_ERROR;
	TAILQ_INSERT_TAIL(&tcp_req.tqpair->outstanding_reqs, &tcp_req, link);

	nvme_tcp_c2h_data_payload_handle(&tqpair, &pdu, &reaped);

	CU_ASSERT(tcp_req.rsp.status.p == 0);
	CU_ASSERT(tcp_req.rsp.cid == tcp_req.cid);
	CU_ASSERT(tcp_req.rsp.sqid == tqpair.qpair.id);
	CU_ASSERT(tcp_req.ordering.bits.data_recv == 1);
	CU_ASSERT(reaped == 2);

	/* case 2: nvme_tcp_c2h_data_payload_handle: tcp_req->datao == tcp_req->req->payload_size */
	tcp_req.datao = 1024;
	tcp_req.req->payload_size = 1024;
	tcp_req.state = NVME_TCP_REQ_ACTIVE;
	tcp_req.ordering.bits.send_ack = 1;
	memset(&tcp_req.rsp, 0, sizeof(tcp_req.rsp));
	tcp_req.ordering.bits.data_recv = 0;
	tqpair.recv_state = NVME_TCP_PDU_RECV_STATE_ERROR;
	TAILQ_INSERT_TAIL(&tcp_req.tqpair->outstanding_reqs, &tcp_req, link);

	nvme_tcp_c2h_data_payload_handle(&tqpair, &pdu, &reaped);

	CU_ASSERT(tcp_req.rsp.status.p == 1);
	CU_ASSERT(tcp_req.rsp.cid == tcp_req.cid);
	CU_ASSERT(tcp_req.rsp.sqid == tqpair.qpair.id);
	CU_ASSERT(tcp_req.ordering.bits.data_recv == 1);
	CU_ASSERT(reaped == 3);

	/* case 3: nvme_tcp_c2h_data_payload_handle: flag does not have SPDK_NVME_TCP_C2H_DATA_FLAGS_SUCCESS */
	pdu.hdr.c2h_data.common.flags = SPDK_NVME_TCP_C2H_DATA_FLAGS_LAST_PDU;
	tcp_req.datao = 1024;
	tcp_req.req->payload_size = 1024;
	tcp_req.state = NVME_TCP_REQ_ACTIVE;
	tcp_req.ordering.bits.send_ack = 1;
	memset(&tcp_req.rsp, 0, sizeof(tcp_req.rsp));
	tcp_req.ordering.bits.data_recv = 0;
	tqpair.recv_state = NVME_TCP_PDU_RECV_STATE_ERROR;
	TAILQ_INSERT_TAIL(&tcp_req.tqpair->outstanding_reqs, &tcp_req, link);

	nvme_tcp_c2h_data_payload_handle(&tqpair, &pdu, &reaped);

	CU_ASSERT(reaped == 3);

	/* case 4: nvme_tcp_c2h_term_req_payload_handle: recv_state is NVME_TCP_PDU_RECV_STATE_ERROR */
	pdu.hdr.term_req.fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
	nvme_tcp_c2h_term_req_payload_handle(&tqpair, &pdu);

	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_ERROR);
}

static void
test_nvme_tcp_icresp_handle(void)
{
	struct nvme_tcp_qpair tqpair = {};
	struct spdk_nvme_tcp_stat stats = {};
	struct nvme_tcp_pdu pdu = {};
	struct nvme_tcp_pdu send_pdu = {};
	struct nvme_tcp_pdu recv_pdu = {};

	tqpair.send_pdu = &send_pdu;
	tqpair.recv_pdu = &recv_pdu;
	tqpair.stats = &stats;
	TAILQ_INIT(&tqpair.send_queue);

	/* case 1: Expected ICResp PFV and got are different. */
	pdu.hdr.ic_resp.pfv = 1;

	nvme_tcp_icresp_handle(&tqpair, &pdu);

	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_ERROR);

	/* case 2: Expected ICResp maxh2cdata and got are different. */
	pdu.hdr.ic_resp.pfv = 0;
	pdu.hdr.ic_resp.maxh2cdata = 2048;

	nvme_tcp_icresp_handle(&tqpair, &pdu);

	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_ERROR);

	/* case 3: Expected ICResp cpda and got are different. */
	pdu.hdr.ic_resp.maxh2cdata = NVME_TCP_PDU_H2C_MIN_DATA_SIZE;
	pdu.hdr.ic_resp.cpda = 64;

	nvme_tcp_icresp_handle(&tqpair, &pdu);

	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_ERROR);

	/* case 4: waiting icreq ack. */
	pdu.hdr.ic_resp.maxh2cdata = NVME_TCP_PDU_H2C_MIN_DATA_SIZE;
	pdu.hdr.ic_resp.cpda = 30;
	pdu.hdr.ic_resp.dgst.bits.hdgst_enable = true;
	pdu.hdr.ic_resp.dgst.bits.ddgst_enable = true;
	tqpair.flags.icreq_send_ack = 0;

	nvme_tcp_icresp_handle(&tqpair, &pdu);

	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
	CU_ASSERT(tqpair.state == NVME_TCP_QPAIR_STATE_INITIALIZING);
	CU_ASSERT(tqpair.maxh2cdata == pdu.hdr.ic_resp.maxh2cdata);
	CU_ASSERT(tqpair.cpda == pdu.hdr.ic_resp.cpda);
	CU_ASSERT(tqpair.flags.host_hdgst_enable == pdu.hdr.ic_resp.dgst.bits.hdgst_enable);
	CU_ASSERT(tqpair.flags.host_ddgst_enable == pdu.hdr.ic_resp.dgst.bits.ddgst_enable);

	/* case 5: Expect: PASS. */
	tqpair.flags.icreq_send_ack = 1;

	nvme_tcp_icresp_handle(&tqpair, &pdu);

	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
	CU_ASSERT(tqpair.state == NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_SEND);
	CU_ASSERT(tqpair.maxh2cdata == pdu.hdr.ic_resp.maxh2cdata);
	CU_ASSERT(tqpair.cpda == pdu.hdr.ic_resp.cpda);
	CU_ASSERT(tqpair.flags.host_hdgst_enable == pdu.hdr.ic_resp.dgst.bits.hdgst_enable);
	CU_ASSERT(tqpair.flags.host_ddgst_enable == pdu.hdr.ic_resp.dgst.bits.ddgst_enable);
}

static void
test_nvme_tcp_pdu_payload_handle(void)
{
	struct nvme_tcp_qpair	tqpair = {};
	struct spdk_nvme_tcp_stat	stats = {};
	struct nvme_tcp_pdu	recv_pdu = {};
	struct nvme_tcp_req	tcp_req = {};
	struct nvme_request	req = {};
	uint32_t		reaped = 0;

	tqpair.recv_pdu = &recv_pdu;
	tcp_req.tqpair = &tqpair;
	tcp_req.req = &req;
	tcp_req.req->qpair = &tqpair.qpair;
	tqpair.stats = &stats;

	tqpair.recv_state = NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD;
	tqpair.qpair.id = 1;
	recv_pdu.ddgst_enable = false;
	recv_pdu.req = &tcp_req;
	recv_pdu.hdr.c2h_data.common.flags = SPDK_NVME_TCP_C2H_DATA_FLAGS_SUCCESS |
					     SPDK_NVME_TCP_C2H_DATA_FLAGS_LAST_PDU;
	recv_pdu.data_len = 1024;
	tcp_req.ordering.bits.data_recv = 0;
	tcp_req.req->cb_fn = ut_nvme_complete_request;
	tcp_req.cid = 1;
	TAILQ_INIT(&tcp_req.tqpair->outstanding_reqs);
	TAILQ_INSERT_TAIL(&tcp_req.tqpair->outstanding_reqs, &tcp_req, link);

	/* C2H_DATA */
	recv_pdu.hdr.common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_C2H_DATA;
	tcp_req.datao = 1024;
	tcp_req.req->payload_size = 2048;
	tcp_req.state = NVME_TCP_REQ_ACTIVE;
	tcp_req.ordering.bits.send_ack = 1;

	recv_pdu.req = &tcp_req;
	nvme_tcp_pdu_payload_handle(&tqpair, &reaped);
	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
	CU_ASSERT(tcp_req.rsp.status.p == 0);
	CU_ASSERT(tcp_req.rsp.cid == 1);
	CU_ASSERT(tcp_req.rsp.sqid == 1);
	CU_ASSERT(tcp_req.ordering.bits.data_recv == 1);
	CU_ASSERT(reaped == 1);

	/* TermResp */
	recv_pdu.hdr.common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ;
	recv_pdu.hdr.term_req.fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
	tqpair.recv_state = NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD;

	recv_pdu.req = &tcp_req;
	nvme_tcp_pdu_payload_handle(&tqpair, &reaped);
	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_ERROR);
}

static void
test_nvme_tcp_capsule_resp_hdr_handle(void)
{
	struct nvme_tcp_qpair	tqpair = {};
	struct spdk_nvme_tcp_stat	stats = {};
	struct nvme_request	req = {};
	struct spdk_nvme_cpl	rccqe_tgt = {};
	struct nvme_tcp_req	*tcp_req = NULL;
	uint32_t		reaped = 0;
	int			rc;

	/* Initialize requests and pdus */
	tqpair.num_entries = 1;
	tqpair.stats = &stats;
	req.qpair = &tqpair.qpair;

	rc = nvme_tcp_alloc_reqs(&tqpair);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	tcp_req = nvme_tcp_req_get(&tqpair);
	SPDK_CU_ASSERT_FATAL(tcp_req != NULL);
	rc = nvme_tcp_req_init(&tqpair, &req, tcp_req);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	tcp_req->ordering.bits.send_ack = 1;
	tqpair.recv_state = NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH;
	/* tqpair.recv_pdu will be reseted after handling */
	memset(&rccqe_tgt, 0xff, sizeof(rccqe_tgt));
	rccqe_tgt.cid = 0;
	memcpy(&tqpair.recv_pdu->hdr.capsule_resp.rccqe, &rccqe_tgt, sizeof(rccqe_tgt));

	nvme_tcp_capsule_resp_hdr_handle(&tqpair, tqpair.recv_pdu, &reaped);
	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
	CU_ASSERT(!memcmp(&tcp_req->rsp, &rccqe_tgt, sizeof(rccqe_tgt)));
	CU_ASSERT(tcp_req->ordering.bits.data_recv == 1);
	CU_ASSERT(reaped == 1);
	CU_ASSERT(TAILQ_EMPTY(&tcp_req->tqpair->outstanding_reqs));

	/* Get tcp request error, expect fail */
	reaped = 0;
	tqpair.recv_pdu->hdr.capsule_resp.rccqe.cid = 1;
	tqpair.recv_state = NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH;

	nvme_tcp_capsule_resp_hdr_handle(&tqpair, tqpair.recv_pdu, &reaped);
	CU_ASSERT(reaped == 0);
	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_ERROR);
	nvme_tcp_free_reqs(&tqpair);
}

static void
test_nvme_tcp_ctrlr_connect_qpair(void)
{
	struct spdk_nvme_ctrlr ctrlr = {};
	struct spdk_nvme_qpair *qpair;
	struct nvme_tcp_qpair *tqpair;
	struct nvme_tcp_pdu pdu = {};
	struct nvme_tcp_pdu recv_pdu = {};
	struct spdk_nvme_tcp_ic_req *ic_req = NULL;
	int rc;

	tqpair = calloc(1, sizeof(*tqpair));
	tqpair->qpair.trtype = SPDK_NVME_TRANSPORT_TCP;
	tqpair->recv_pdu = &recv_pdu;
	qpair = &tqpair->qpair;
	tqpair->sock = (struct spdk_sock *)0xDEADBEEF;
	tqpair->send_pdu = &pdu;
	tqpair->qpair.ctrlr = &ctrlr;
	tqpair->qpair.state = NVME_QPAIR_CONNECTING;
	ic_req = &pdu.hdr.ic_req;

	tqpair->recv_pdu->hdr.common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_IC_RESP;
	tqpair->recv_pdu->hdr.common.plen = sizeof(struct spdk_nvme_tcp_ic_resp);
	tqpair->recv_pdu->hdr.common.hlen = sizeof(struct spdk_nvme_tcp_ic_resp);
	tqpair->recv_pdu->ch_valid_bytes = 8;
	tqpair->recv_pdu->psh_valid_bytes = tqpair->recv_pdu->hdr.common.hlen;
	tqpair->recv_pdu->hdr.ic_resp.maxh2cdata = 4096;
	tqpair->recv_pdu->hdr.ic_resp.cpda = 1;
	tqpair->flags.icreq_send_ack = 1;
	tqpair->qpair.ctrlr->opts.header_digest = true;
	tqpair->qpair.ctrlr->opts.data_digest = true;
	TAILQ_INIT(&tqpair->send_queue);

	rc = nvme_tcp_ctrlr_connect_qpair(&ctrlr, qpair);
	CU_ASSERT(rc == 0);

	while (nvme_qpair_get_state(qpair) == NVME_QPAIR_CONNECTING) {
		rc = nvme_tcp_qpair_process_completions(qpair, 0);
		CU_ASSERT(rc >= 0);
	}

	CU_ASSERT(tqpair->maxr2t == NVME_TCP_MAX_R2T_DEFAULT);
	CU_ASSERT(tqpair->state == NVME_TCP_QPAIR_STATE_RUNNING);
	CU_ASSERT(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH);
	CU_ASSERT(ic_req->common.hlen == sizeof(*ic_req));
	CU_ASSERT(ic_req->common.plen == sizeof(*ic_req));
	CU_ASSERT(ic_req->common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_IC_REQ);
	CU_ASSERT(ic_req->pfv == 0);
	CU_ASSERT(ic_req->maxr2t == NVME_TCP_MAX_R2T_DEFAULT - 1);
	CU_ASSERT(ic_req->hpda == NVME_TCP_HPDA_DEFAULT);
	CU_ASSERT(ic_req->dgst.bits.hdgst_enable == true);
	CU_ASSERT(ic_req->dgst.bits.ddgst_enable == true);

	nvme_tcp_ctrlr_delete_io_qpair(&ctrlr, qpair);
}

static void
test_nvme_tcp_ctrlr_disconnect_qpair(void)
{
	struct spdk_nvme_ctrlr ctrlr = {};
	struct spdk_nvme_qpair *qpair;
	struct nvme_tcp_qpair tqpair = {
		.qpair.trtype = SPDK_NVME_TRANSPORT_TCP,
	};
	struct nvme_tcp_poll_group tgroup = {};
	struct nvme_tcp_pdu pdu = {};

	qpair = &tqpair.qpair;
	qpair->poll_group = &tgroup.group;
	tqpair.sock = (struct spdk_sock *)0xDEADBEEF;
	tqpair.needs_poll = true;
	TAILQ_INIT(&tgroup.needs_poll);
	TAILQ_INIT(&tqpair.send_queue);
	TAILQ_INSERT_TAIL(&tgroup.needs_poll, &tqpair, link);
	TAILQ_INSERT_TAIL(&tqpair.send_queue, &pdu, tailq);

	nvme_tcp_ctrlr_disconnect_qpair(&ctrlr, qpair);

	CU_ASSERT(tqpair.needs_poll == false);
	CU_ASSERT(tqpair.sock == NULL);
	CU_ASSERT(TAILQ_EMPTY(&tqpair.send_queue) == true);
}

static void
test_nvme_tcp_ctrlr_create_io_qpair(void)
{
	struct spdk_nvme_qpair *qpair = NULL;
	struct spdk_nvme_ctrlr ctrlr = {};
	uint16_t qid = 1;
	const struct spdk_nvme_io_qpair_opts opts = {
		.io_queue_size = 1,
		.qprio = SPDK_NVME_QPRIO_URGENT,
		.io_queue_requests = 1,
	};
	struct nvme_tcp_qpair *tqpair;

	ctrlr.trid.priority = 1;
	ctrlr.trid.adrfam = SPDK_NVMF_ADRFAM_IPV4;
	memcpy(ctrlr.trid.traddr, "192.168.1.78", sizeof("192.168.1.78"));
	memcpy(ctrlr.trid.trsvcid, "23", sizeof("23"));
	memcpy(ctrlr.opts.src_addr, "192.168.1.77", sizeof("192.168.1.77"));
	memcpy(ctrlr.opts.src_svcid, "23", sizeof("23"));

	qpair = nvme_tcp_ctrlr_create_io_qpair(&ctrlr, qid, &opts);
	tqpair = nvme_tcp_qpair(qpair);

	CU_ASSERT(qpair != NULL);
	CU_ASSERT(qpair->id == 1);
	CU_ASSERT(qpair->ctrlr == &ctrlr);
	CU_ASSERT(qpair->qprio == SPDK_NVME_QPRIO_URGENT);
	CU_ASSERT(qpair->trtype == SPDK_NVME_TRANSPORT_TCP);
	CU_ASSERT(qpair->poll_group == (void *)0xDEADBEEF);
	CU_ASSERT(tqpair->num_entries = 1);

	free(tqpair->tcp_reqs);
	spdk_free(tqpair->send_pdus);
	free(tqpair);
}

static void
test_nvme_tcp_ctrlr_delete_io_qpair(void)
{
	struct spdk_nvme_ctrlr *ctrlr = (struct spdk_nvme_ctrlr *)0xdeadbeef;
	struct spdk_nvme_qpair *qpair;
	struct nvme_tcp_qpair *tqpair;
	struct nvme_tcp_req tcp_req = {};
	struct nvme_request	req = {};
	int rc;

	tqpair = calloc(1, sizeof(struct nvme_tcp_qpair));
	tqpair->tcp_reqs = calloc(1, sizeof(struct nvme_tcp_req));
	tqpair->send_pdus = calloc(1, sizeof(struct nvme_tcp_pdu));
	tqpair->qpair.trtype = SPDK_NVME_TRANSPORT_TCP;
	qpair = &tqpair->qpair;
	tcp_req.req = &req;
	tcp_req.req->qpair = &tqpair->qpair;
	tcp_req.req->cb_fn = ut_nvme_complete_request;
	tcp_req.tqpair = tqpair;
	tcp_req.state = NVME_TCP_REQ_ACTIVE;
	TAILQ_INIT(&tqpair->outstanding_reqs);
	TAILQ_INSERT_TAIL(&tcp_req.tqpair->outstanding_reqs, &tcp_req, link);

	rc = nvme_tcp_ctrlr_delete_io_qpair(ctrlr, qpair);

	CU_ASSERT(rc == 0);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("nvme_tcp", NULL, NULL);
	CU_ADD_TEST(suite, test_nvme_tcp_pdu_set_data_buf);
	CU_ADD_TEST(suite, test_nvme_tcp_build_iovs);
	CU_ADD_TEST(suite, test_nvme_tcp_build_sgl_request);
	CU_ADD_TEST(suite, test_nvme_tcp_pdu_set_data_buf_with_md);
	CU_ADD_TEST(suite, test_nvme_tcp_build_iovs_with_md);
	CU_ADD_TEST(suite, test_nvme_tcp_req_complete_safe);
	CU_ADD_TEST(suite, test_nvme_tcp_req_get);
	CU_ADD_TEST(suite, test_nvme_tcp_req_init);
	CU_ADD_TEST(suite, test_nvme_tcp_qpair_capsule_cmd_send);
	CU_ADD_TEST(suite, test_nvme_tcp_qpair_write_pdu);
	CU_ADD_TEST(suite, test_nvme_tcp_qpair_set_recv_state);
	CU_ADD_TEST(suite, test_nvme_tcp_alloc_reqs);
	CU_ADD_TEST(suite, test_nvme_tcp_parse_addr);
	CU_ADD_TEST(suite, test_nvme_tcp_qpair_send_h2c_term_req);
	CU_ADD_TEST(suite, test_nvme_tcp_pdu_ch_handle);
	CU_ADD_TEST(suite, test_nvme_tcp_qpair_connect_sock);
	CU_ADD_TEST(suite, test_nvme_tcp_qpair_icreq_send);
	CU_ADD_TEST(suite, test_nvme_tcp_c2h_payload_handle);
	CU_ADD_TEST(suite, test_nvme_tcp_icresp_handle);
	CU_ADD_TEST(suite, test_nvme_tcp_pdu_payload_handle);
	CU_ADD_TEST(suite, test_nvme_tcp_capsule_resp_hdr_handle);
	CU_ADD_TEST(suite, test_nvme_tcp_ctrlr_connect_qpair);
	CU_ADD_TEST(suite, test_nvme_tcp_ctrlr_disconnect_qpair);
	CU_ADD_TEST(suite, test_nvme_tcp_ctrlr_create_io_qpair);
	CU_ADD_TEST(suite, test_nvme_tcp_ctrlr_delete_io_qpair);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
