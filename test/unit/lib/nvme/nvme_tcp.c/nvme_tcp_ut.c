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

DEFINE_STUB(spdk_nvme_poll_group_process_completions, int64_t, (struct spdk_nvme_poll_group *group,
		uint32_t completions_per_qpair, spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb), 0);

DEFINE_STUB(nvme_poll_group_connect_qpair, int, (struct spdk_nvme_qpair *qpair), 0);

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
	struct spdk_nvme_ctrlr ctrlr = {0};
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

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
