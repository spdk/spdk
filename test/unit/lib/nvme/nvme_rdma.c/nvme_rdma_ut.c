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
#include "nvme/nvme_rdma.c"

SPDK_LOG_REGISTER_COMPONENT("nvme", SPDK_LOG_NVME)

DEFINE_STUB(nvme_qpair_submit_request, int, (struct spdk_nvme_qpair *qpair,
		struct nvme_request *req), 0);

DEFINE_STUB(nvme_qpair_init, int, (struct spdk_nvme_qpair *qpair, uint16_t id,
				   struct spdk_nvme_ctrlr *ctrlr, enum spdk_nvme_qprio qprio, uint32_t num_requests), 0);

DEFINE_STUB_V(nvme_qpair_deinit, (struct spdk_nvme_qpair *qpair));

DEFINE_STUB(nvme_ctrlr_probe, int, (const struct spdk_nvme_transport_id *trid, void *devhandle,
				    spdk_nvme_probe_cb probe_cb, void *cb_ctx), 0);

DEFINE_STUB(nvme_ctrlr_get_cap, int, (struct spdk_nvme_ctrlr *ctrlr,
				      union spdk_nvme_cap_register *cap), 0);

DEFINE_STUB(nvme_ctrlr_get_vs, int, (struct spdk_nvme_ctrlr *ctrlr,
				     union spdk_nvme_vs_register *vs), 0);

DEFINE_STUB_V(nvme_ctrlr_init_cap, (struct spdk_nvme_ctrlr *ctrlr,
				    const union spdk_nvme_cap_register *cap, const union spdk_nvme_vs_register *vs));

DEFINE_STUB(nvme_ctrlr_construct, int, (struct spdk_nvme_ctrlr *ctrlr), 0);

DEFINE_STUB_V(nvme_ctrlr_destruct, (struct spdk_nvme_ctrlr *ctrlr));

DEFINE_STUB(nvme_ctrlr_add_process, int, (struct spdk_nvme_ctrlr *ctrlr, void *devhandle), 0);

DEFINE_STUB_V(nvme_ctrlr_connected, (struct spdk_nvme_ctrlr *ctrlr));

DEFINE_STUB(nvme_ctrlr_cmd_identify, int, (struct spdk_nvme_ctrlr *ctrlr, uint8_t cns,
		uint16_t cntid, uint32_t nsid, void *payload, size_t payload_size, spdk_nvme_cmd_cb cb_fn,
		void *cb_arg), 0);

DEFINE_STUB_V(spdk_nvme_ctrlr_get_default_ctrlr_opts, (struct spdk_nvme_ctrlr_opts *opts,
		size_t opts_size));

DEFINE_STUB_V(nvme_completion_poll_cb, (void *arg, const struct spdk_nvme_cpl *cpl));

DEFINE_STUB(spdk_nvme_ctrlr_get_current_process, struct spdk_nvme_ctrlr_process *,
	    (struct spdk_nvme_ctrlr *ctrlr), NULL);

DEFINE_STUB(spdk_nvme_wait_for_completion, int, (struct spdk_nvme_qpair *qpair,
		struct nvme_completion_poll_status *status), 0);

DEFINE_STUB(spdk_nvme_wait_for_completion_robust_lock, int, (struct spdk_nvme_qpair *qpair,
		struct nvme_completion_poll_status *status, pthread_mutex_t *robust_mutex), 0);

DEFINE_STUB(spdk_mem_map_set_translation, int, (struct spdk_mem_map *map, uint64_t vaddr,
		uint64_t size, uint64_t translation), 0);

DEFINE_STUB(spdk_mem_map_clear_translation, int, (struct spdk_mem_map *map, uint64_t vaddr,
		uint64_t size), 0);

DEFINE_STUB(spdk_mem_map_alloc, struct spdk_mem_map *, (uint64_t default_translation,
		const struct spdk_mem_map_ops *ops, void *cb_ctx), NULL);

DEFINE_STUB_V(spdk_mem_map_free, (struct spdk_mem_map **pmap));

DEFINE_STUB(nvme_fabric_qpair_connect, int, (struct spdk_nvme_qpair *qpair, uint32_t num_entries),
	    0);

DEFINE_STUB(nvme_transport_ctrlr_set_reg_4, int, (struct spdk_nvme_ctrlr *ctrlr, uint32_t offset,
		uint32_t value), 0);

DEFINE_STUB(nvme_fabric_ctrlr_set_reg_4, int, (struct spdk_nvme_ctrlr *ctrlr, uint32_t offset,
		uint32_t value), 0);

DEFINE_STUB(nvme_fabric_ctrlr_set_reg_8, int, (struct spdk_nvme_ctrlr *ctrlr, uint32_t offset,
		uint64_t value), 0);

DEFINE_STUB(nvme_fabric_ctrlr_get_reg_4, int, (struct spdk_nvme_ctrlr *ctrlr, uint32_t offset,
		uint32_t *value), 0);

DEFINE_STUB(nvme_fabric_ctrlr_get_reg_8, int, (struct spdk_nvme_ctrlr *ctrlr, uint32_t offset,
		uint64_t *value), 0);

DEFINE_STUB_V(nvme_ctrlr_destruct_finish, (struct spdk_nvme_ctrlr *ctrlr));

DEFINE_STUB(nvme_request_check_timeout, int, (struct nvme_request *req, uint16_t cid,
		struct spdk_nvme_ctrlr_process *active_proc, uint64_t now_tick), 0);

DEFINE_STUB(nvme_fabric_ctrlr_discover, int, (struct spdk_nvme_ctrlr *ctrlr, void *cb_ctx,
		spdk_nvme_probe_cb probe_cb), 0);

/* used to mock out having to split an SGL over a memory region */
uint64_t g_mr_size;
struct ibv_mr g_nvme_rdma_mr;

uint64_t
spdk_mem_map_translate(const struct spdk_mem_map *map, uint64_t vaddr, uint64_t *size)
{
	if (g_mr_size != 0) {
		*(uint32_t *)size = g_mr_size;
	}

	return (uint64_t)&g_nvme_rdma_mr;
}

struct nvme_rdma_ut_bdev_io {
	struct iovec iovs[NVME_RDMA_MAX_SGL_DESCRIPTORS];
	int iovpos;
};

/* essentially a simplification of bdev_nvme_next_sge and bdev_nvme_reset_sgl */
static void nvme_rdma_ut_reset_sgl(void *cb_arg, uint32_t offset)
{
	struct nvme_rdma_ut_bdev_io *bio = cb_arg;
	struct iovec *iov;

	for (bio->iovpos = 0; bio->iovpos < NVME_RDMA_MAX_SGL_DESCRIPTORS; bio->iovpos++) {
		iov = &bio->iovs[bio->iovpos];
		/* Only provide offsets at the beginning of an iov */
		if (offset == 0) {
			break;
		}

		offset -= iov->iov_len;
	}

	SPDK_CU_ASSERT_FATAL(bio->iovpos < NVME_RDMA_MAX_SGL_DESCRIPTORS);
}

static int nvme_rdma_ut_next_sge(void *cb_arg, void **address, uint32_t *length)
{
	struct nvme_rdma_ut_bdev_io *bio = cb_arg;
	struct iovec *iov;

	SPDK_CU_ASSERT_FATAL(bio->iovpos < NVME_RDMA_MAX_SGL_DESCRIPTORS);

	iov = &bio->iovs[bio->iovpos];

	*address = iov->iov_base;
	*length = iov->iov_len;
	bio->iovpos++;

	return 0;
}

static void
test_nvme_rdma_build_sgl_request(void)
{
	struct nvme_rdma_qpair rqpair;
	struct spdk_nvme_ctrlr ctrlr = {0};
	struct spdk_nvmf_cmd cmd = {{0}};
	struct spdk_nvme_rdma_req rdma_req = {0};
	struct nvme_request req = {{0}};
	struct nvme_rdma_ut_bdev_io bio;
	struct spdk_nvme_rdma_mr_map rmap = {0};
	struct spdk_mem_map *map = NULL;
	uint64_t i;
	int rc;

	rmap.map = map;

	ctrlr.max_sges = NVME_RDMA_MAX_SGL_DESCRIPTORS;

	rqpair.mr_map = &rmap;
	rqpair.qpair.ctrlr = &ctrlr;
	rqpair.cmds = &cmd;
	cmd.sgl[0].address = 0x1111;

	rdma_req.id = 0;
	rdma_req.req = &req;

	req.payload.reset_sgl_fn = nvme_rdma_ut_reset_sgl;
	req.payload.next_sge_fn = nvme_rdma_ut_next_sge;
	req.payload.contig_or_cb_arg = &bio;
	req.qpair = &rqpair.qpair;

	g_nvme_rdma_mr.rkey = 1;

	for (i = 0; i < NVME_RDMA_MAX_SGL_DESCRIPTORS; i++) {
		bio.iovs[i].iov_base = (void *)i;
		bio.iovs[i].iov_len = 0;
	}

	/* Test case 1: single SGL. Expected: PASS */
	bio.iovpos = 0;
	req.payload_offset = 0;
	req.payload_size = 0x1000;
	bio.iovs[0].iov_len = 0x1000;
	rc = nvme_rdma_build_sgl_request(&rqpair, &rdma_req);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(bio.iovpos == 1);
	CU_ASSERT(req.cmd.dptr.sgl1.keyed.type == SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK);
	CU_ASSERT(req.cmd.dptr.sgl1.keyed.subtype == SPDK_NVME_SGL_SUBTYPE_ADDRESS);
	CU_ASSERT(req.cmd.dptr.sgl1.keyed.length == req.payload_size);
	CU_ASSERT(req.cmd.dptr.sgl1.keyed.key == g_nvme_rdma_mr.rkey);
	CU_ASSERT(req.cmd.dptr.sgl1.address == (uint64_t)bio.iovs[0].iov_base);
	CU_ASSERT(rdma_req.send_sgl[0].length == sizeof(struct spdk_nvme_cmd));

	/* Test case 2: multiple SGL. Expected: PASS */
	bio.iovpos = 0;
	req.payload_offset = 0;
	req.payload_size = 0x4000;
	for (i = 0; i < 4; i++) {
		bio.iovs[i].iov_len = 0x1000;
	}
	rc = nvme_rdma_build_sgl_request(&rqpair, &rdma_req);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(bio.iovpos == 4);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.type == SPDK_NVME_SGL_TYPE_LAST_SEGMENT);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.length == 4 * sizeof(struct spdk_nvme_sgl_descriptor));
	CU_ASSERT(req.cmd.dptr.sgl1.address == (uint64_t)0);
	CU_ASSERT(rdma_req.send_sgl[0].length == 4 * sizeof(struct spdk_nvme_sgl_descriptor) + sizeof(
			  struct spdk_nvme_cmd))
	for (i = 0; i < 4; i++) {
		CU_ASSERT(cmd.sgl[i].keyed.type == SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK);
		CU_ASSERT(cmd.sgl[i].keyed.subtype == SPDK_NVME_SGL_SUBTYPE_ADDRESS);
		CU_ASSERT(cmd.sgl[i].keyed.length == bio.iovs[i].iov_len);
		CU_ASSERT(cmd.sgl[i].keyed.key == g_nvme_rdma_mr.rkey);
		CU_ASSERT(cmd.sgl[i].address == (uint64_t)bio.iovs[i].iov_base);
	}

	/* Test case 3: Multiple SGL, SGL larger than mr size. Expected: FAIL */
	bio.iovpos = 0;
	req.payload_offset = 0;
	g_mr_size = 0x500;
	rc = nvme_rdma_build_sgl_request(&rqpair, &rdma_req);
	SPDK_CU_ASSERT_FATAL(rc != 0);
	CU_ASSERT(bio.iovpos == 1);

	/* Test case 4: Multiple SGL, SGL size smaller than I/O size */
	bio.iovpos = 0;
	req.payload_offset = 0;
	req.payload_size = 0x6000;
	g_mr_size = 0x0;
	rc = nvme_rdma_build_sgl_request(&rqpair, &rdma_req);
	SPDK_CU_ASSERT_FATAL(rc != 0);
	CU_ASSERT(bio.iovpos == NVME_RDMA_MAX_SGL_DESCRIPTORS);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("nvme_rdma", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (CU_add_test(suite, "build_sgl_request", test_nvme_rdma_build_sgl_request) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
