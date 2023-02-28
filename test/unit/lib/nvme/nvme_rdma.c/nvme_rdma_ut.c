/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation. All rights reserved.
 *   Copyright (c) 2021, 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk_cunit.h"
#include "nvme/nvme_rdma.c"
#include "common/lib/nvme/common_stubs.h"
#include "common/lib/test_rdma.c"

SPDK_LOG_REGISTER_COMPONENT(nvme)

DEFINE_STUB(spdk_mem_map_set_translation, int, (struct spdk_mem_map *map, uint64_t vaddr,
		uint64_t size, uint64_t translation), 0);
DEFINE_STUB(spdk_mem_map_clear_translation, int, (struct spdk_mem_map *map, uint64_t vaddr,
		uint64_t size), 0);

DEFINE_STUB(spdk_mem_map_alloc, struct spdk_mem_map *, (uint64_t default_translation,
		const struct spdk_mem_map_ops *ops, void *cb_ctx), NULL);
DEFINE_STUB_V(spdk_mem_map_free, (struct spdk_mem_map **pmap));

DEFINE_STUB(nvme_poll_group_connect_qpair, int, (struct spdk_nvme_qpair *qpair), 0);

DEFINE_STUB_V(nvme_qpair_resubmit_requests, (struct spdk_nvme_qpair *qpair, uint32_t num_requests));
DEFINE_STUB(spdk_nvme_poll_group_process_completions, int64_t, (struct spdk_nvme_poll_group *group,
		uint32_t completions_per_qpair, spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb), 0);

DEFINE_STUB(rdma_ack_cm_event, int, (struct rdma_cm_event *event), 0);
DEFINE_STUB_V(rdma_free_devices, (struct ibv_context **list));
DEFINE_STUB(fcntl, int, (int fd, int cmd, ...), 0);
DEFINE_STUB_V(rdma_destroy_event_channel, (struct rdma_event_channel *channel));

DEFINE_STUB(ibv_dereg_mr, int, (struct ibv_mr *mr), 0);
DEFINE_STUB(ibv_resize_cq, int, (struct ibv_cq *cq, int cqe), 0);

DEFINE_STUB(spdk_memory_domain_get_context, struct spdk_memory_domain_ctx *,
	    (struct spdk_memory_domain *device), NULL);
DEFINE_STUB(spdk_memory_domain_get_dma_device_type, enum spdk_dma_device_type,
	    (struct spdk_memory_domain *device), SPDK_DMA_DEVICE_TYPE_RDMA);
DEFINE_STUB_V(spdk_memory_domain_destroy, (struct spdk_memory_domain *device));
DEFINE_STUB(spdk_memory_domain_pull_data, int, (struct spdk_memory_domain *src_domain,
		void *src_domain_ctx, struct iovec *src_iov, uint32_t src_iov_cnt, struct iovec *dst_iov,
		uint32_t dst_iov_cnt, spdk_memory_domain_data_cpl_cb cpl_cb, void *cpl_cb_arg), 0);

DEFINE_STUB_V(spdk_nvme_qpair_print_command, (struct spdk_nvme_qpair *qpair,
		struct spdk_nvme_cmd *cmd));

DEFINE_STUB_V(spdk_nvme_qpair_print_completion, (struct spdk_nvme_qpair *qpair,
		struct spdk_nvme_cpl *cpl));

DEFINE_RETURN_MOCK(spdk_memory_domain_create, int);
int
spdk_memory_domain_create(struct spdk_memory_domain **domain, enum spdk_dma_device_type type,
			  struct spdk_memory_domain_ctx *ctx, const char *id)
{
	static struct spdk_memory_domain *__dma_dev = (struct spdk_memory_domain *)0xdeaddead;

	HANDLE_RETURN_MOCK(spdk_memory_domain_create);

	*domain = __dma_dev;

	return 0;
}

static struct spdk_memory_domain_translation_result g_memory_translation_translation = {.size = sizeof(struct spdk_memory_domain_translation_result) };

DEFINE_RETURN_MOCK(spdk_memory_domain_translate_data, int);
int
spdk_memory_domain_translate_data(struct spdk_memory_domain *src_domain, void *src_domain_ctx,
				  struct spdk_memory_domain *dst_domain, struct spdk_memory_domain_translation_ctx *dst_domain_ctx,
				  void *addr, size_t len, struct spdk_memory_domain_translation_result *result)
{

	HANDLE_RETURN_MOCK(spdk_memory_domain_translate_data);

	memcpy(result, &g_memory_translation_translation, sizeof(g_memory_translation_translation));

	return 0;
}

/* ibv_reg_mr can be a macro, need to undefine it */
#ifdef ibv_reg_mr
#undef ibv_reg_mr
#endif

DEFINE_RETURN_MOCK(ibv_reg_mr, struct ibv_mr *);
struct ibv_mr *
ibv_reg_mr(struct ibv_pd *pd, void *addr, size_t length, int access)
{
	HANDLE_RETURN_MOCK(ibv_reg_mr);
	if (length > 0) {
		return &g_rdma_mr;
	} else {
		return NULL;
	}
}

struct nvme_rdma_ut_bdev_io {
	struct iovec iovs[NVME_RDMA_MAX_SGL_DESCRIPTORS];
	int iovpos;
	int iovcnt;
};

DEFINE_RETURN_MOCK(rdma_get_devices, struct ibv_context **);
struct ibv_context **
rdma_get_devices(int *num_devices)
{
	static struct ibv_context *_contexts[] = {
		(struct ibv_context *)0xDEADBEEF,
		(struct ibv_context *)0xFEEDBEEF,
		NULL
	};

	HANDLE_RETURN_MOCK(rdma_get_devices);
	return _contexts;
}

DEFINE_RETURN_MOCK(rdma_create_event_channel, struct rdma_event_channel *);
struct rdma_event_channel *
rdma_create_event_channel(void)
{
	HANDLE_RETURN_MOCK(rdma_create_event_channel);
	return NULL;
}

DEFINE_RETURN_MOCK(ibv_query_device, int);
int
ibv_query_device(struct ibv_context *context,
		 struct ibv_device_attr *device_attr)
{
	if (device_attr) {
		device_attr->max_sge = NVME_RDMA_MAX_SGL_DESCRIPTORS;
	}
	HANDLE_RETURN_MOCK(ibv_query_device);

	return 0;
}

/* essentially a simplification of bdev_nvme_next_sge and bdev_nvme_reset_sgl */
static void
nvme_rdma_ut_reset_sgl(void *cb_arg, uint32_t offset)
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

static int
nvme_rdma_ut_next_sge(void *cb_arg, void **address, uint32_t *length)
{
	struct nvme_rdma_ut_bdev_io *bio = cb_arg;
	struct iovec *iov;

	SPDK_CU_ASSERT_FATAL(bio->iovpos < NVME_RDMA_MAX_SGL_DESCRIPTORS);

	if (bio->iovpos == bio->iovcnt) {
		return -1;
	}

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
	struct nvme_rdma_ut_bdev_io bio = { .iovcnt = NVME_RDMA_MAX_SGL_DESCRIPTORS };
	uint64_t i;
	int rc;

	ctrlr.max_sges = NVME_RDMA_MAX_SGL_DESCRIPTORS;
	ctrlr.cdata.nvmf_specific.msdbd = 16;
	ctrlr.ioccsz_bytes = 4096;

	rqpair.mr_map = (struct spdk_rdma_mem_map *)0xdeadbeef;
	rqpair.rdma_qp = (struct spdk_rdma_qp *)0xdeadbeef;
	rqpair.qpair.ctrlr = &ctrlr;
	rqpair.cmds = &cmd;
	cmd.sgl[0].address = 0x1111;
	rdma_req.id = 0;
	rdma_req.req = &req;

	req.payload.reset_sgl_fn = nvme_rdma_ut_reset_sgl;
	req.payload.next_sge_fn = nvme_rdma_ut_next_sge;
	req.payload.contig_or_cb_arg = &bio;
	req.qpair = &rqpair.qpair;

	for (i = 0; i < NVME_RDMA_MAX_SGL_DESCRIPTORS; i++) {
		bio.iovs[i].iov_base = (void *)i + 1;
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
	CU_ASSERT(req.cmd.dptr.sgl1.keyed.key == RDMA_UT_RKEY);
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
		CU_ASSERT(cmd.sgl[i].keyed.key == RDMA_UT_RKEY);
		CU_ASSERT(cmd.sgl[i].address == (uint64_t)bio.iovs[i].iov_base);
	}

	/* Test case 3: Multiple SGL, SGL 2X mr size. Expected: FAIL */
	bio.iovpos = 0;
	req.payload_offset = 0;
	g_mr_size = 0x800;
	rc = nvme_rdma_build_sgl_request(&rqpair, &rdma_req);
	SPDK_CU_ASSERT_FATAL(rc != 0);
	CU_ASSERT(bio.iovpos == 1);

	/* Test case 4: Multiple SGL, SGL size smaller than I/O size. Expected: FAIL */
	bio.iovpos = 0;
	bio.iovcnt = 4;
	req.payload_offset = 0;
	req.payload_size = 0x6000;
	g_mr_size = 0x0;
	rc = nvme_rdma_build_sgl_request(&rqpair, &rdma_req);
	SPDK_CU_ASSERT_FATAL(rc != 0);
	CU_ASSERT(bio.iovpos == bio.iovcnt);
	bio.iovcnt = NVME_RDMA_MAX_SGL_DESCRIPTORS;

	/* Test case 5: SGL length exceeds 3 bytes. Expected: FAIL */
	req.payload_size = 0x1000 + (1 << 24);
	bio.iovs[0].iov_len = 0x1000;
	bio.iovs[1].iov_len = 1 << 24;
	rc = nvme_rdma_build_sgl_request(&rqpair, &rdma_req);
	SPDK_CU_ASSERT_FATAL(rc != 0);

	/* Test case 6: 4 SGL descriptors, size of SGL descriptors exceeds ICD. Expected: FAIL */
	ctrlr.ioccsz_bytes = 60;
	bio.iovpos = 0;
	req.payload_offset = 0;
	req.payload_size = 0x4000;
	for (i = 0; i < 4; i++) {
		bio.iovs[i].iov_len = 0x1000;
	}
	rc = nvme_rdma_build_sgl_request(&rqpair, &rdma_req);
	SPDK_CU_ASSERT_FATAL(rc == -1);
}

static void
test_nvme_rdma_build_sgl_inline_request(void)
{
	struct nvme_rdma_qpair rqpair;
	struct spdk_nvme_ctrlr ctrlr = {0};
	struct spdk_nvmf_cmd cmd = {{0}};
	struct spdk_nvme_rdma_req rdma_req = {0};
	struct nvme_request req = {{0}};
	struct nvme_rdma_ut_bdev_io bio = { .iovcnt = NVME_RDMA_MAX_SGL_DESCRIPTORS };
	int rc;

	ctrlr.max_sges = NVME_RDMA_MAX_SGL_DESCRIPTORS;
	ctrlr.cdata.nvmf_specific.msdbd = 16;

	rqpair.mr_map = (struct spdk_rdma_mem_map *)0xdeadbeef;
	rqpair.rdma_qp = (struct spdk_rdma_qp *)0xdeadbeef;
	rqpair.qpair.ctrlr = &ctrlr;
	rqpair.cmds = &cmd;
	cmd.sgl[0].address = 0x1111;
	rdma_req.id = 0;
	rdma_req.req = &req;

	req.payload.reset_sgl_fn = nvme_rdma_ut_reset_sgl;
	req.payload.next_sge_fn = nvme_rdma_ut_next_sge;
	req.payload.contig_or_cb_arg = &bio;
	req.qpair = &rqpair.qpair;

	/* Test case 1: single inline SGL. Expected: PASS */
	bio.iovpos = 0;
	req.payload_offset = 0;
	req.payload_size = 0x1000;
	bio.iovs[0].iov_base = (void *)0xdeadbeef;
	bio.iovs[0].iov_len = 0x1000;
	rc = nvme_rdma_build_sgl_inline_request(&rqpair, &rdma_req);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(bio.iovpos == 1);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.length == req.payload_size);
	CU_ASSERT(req.cmd.dptr.sgl1.address == 0);
	CU_ASSERT(rdma_req.send_sgl[0].length == sizeof(struct spdk_nvme_cmd));
	CU_ASSERT(rdma_req.send_sgl[1].length == req.payload_size);
	CU_ASSERT(rdma_req.send_sgl[1].addr == (uint64_t)bio.iovs[0].iov_base);
	CU_ASSERT(rdma_req.send_sgl[1].lkey == RDMA_UT_LKEY);

	/* Test case 2: SGL length exceeds 3 bytes. Expected: PASS */
	bio.iovpos = 0;
	req.payload_offset = 0;
	req.payload_size = 1 << 24;
	bio.iovs[0].iov_len = 1 << 24;
	rc = nvme_rdma_build_sgl_inline_request(&rqpair, &rdma_req);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(bio.iovpos == 1);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.length == req.payload_size);
	CU_ASSERT(req.cmd.dptr.sgl1.address == 0);
	CU_ASSERT(rdma_req.send_sgl[0].length == sizeof(struct spdk_nvme_cmd));
	CU_ASSERT(rdma_req.send_sgl[1].length == req.payload_size);
	CU_ASSERT(rdma_req.send_sgl[1].addr == (uint64_t)bio.iovs[0].iov_base);
	CU_ASSERT(rdma_req.send_sgl[1].lkey == RDMA_UT_LKEY);
}

static void
test_nvme_rdma_build_contig_request(void)
{
	struct nvme_rdma_qpair rqpair;
	struct spdk_nvme_ctrlr ctrlr = {0};
	struct spdk_nvmf_cmd cmd = {{0}};
	struct spdk_nvme_rdma_req rdma_req = {0};
	struct nvme_request req = {{0}};
	int rc;

	ctrlr.max_sges = NVME_RDMA_MAX_SGL_DESCRIPTORS;
	ctrlr.cdata.nvmf_specific.msdbd = 16;

	rqpair.mr_map = (struct spdk_rdma_mem_map *)0xdeadbeef;
	rqpair.rdma_qp = (struct spdk_rdma_qp *)0xdeadbeef;
	rqpair.qpair.ctrlr = &ctrlr;
	rqpair.cmds = &cmd;
	cmd.sgl[0].address = 0x1111;
	rdma_req.id = 0;
	rdma_req.req = &req;

	req.payload.contig_or_cb_arg = (void *)0xdeadbeef;
	req.qpair = &rqpair.qpair;

	/* Test case 1: contig request. Expected: PASS */
	req.payload_offset = 0;
	req.payload_size = 0x1000;
	rc = nvme_rdma_build_contig_request(&rqpair, &rdma_req);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(req.cmd.dptr.sgl1.keyed.type == SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK);
	CU_ASSERT(req.cmd.dptr.sgl1.keyed.subtype == SPDK_NVME_SGL_SUBTYPE_ADDRESS);
	CU_ASSERT(req.cmd.dptr.sgl1.keyed.length == req.payload_size);
	CU_ASSERT(req.cmd.dptr.sgl1.keyed.key == RDMA_UT_RKEY);
	CU_ASSERT(req.cmd.dptr.sgl1.address == (uint64_t)req.payload.contig_or_cb_arg);
	CU_ASSERT(rdma_req.send_sgl[0].length == sizeof(struct spdk_nvme_cmd));

	/* Test case 2: SGL length exceeds 3 bytes. Expected: FAIL */
	req.payload_offset = 0;
	req.payload_size = 1 << 24;
	rc = nvme_rdma_build_contig_request(&rqpair, &rdma_req);
	SPDK_CU_ASSERT_FATAL(rc != 0);
}

static void
test_nvme_rdma_build_contig_inline_request(void)
{
	struct nvme_rdma_qpair rqpair;
	struct spdk_nvme_ctrlr ctrlr = {0};
	struct spdk_nvmf_cmd cmd = {{0}};
	struct spdk_nvme_rdma_req rdma_req = {0};
	struct nvme_request req = {{0}};
	int rc;

	ctrlr.max_sges = NVME_RDMA_MAX_SGL_DESCRIPTORS;
	ctrlr.cdata.nvmf_specific.msdbd = 16;

	rqpair.mr_map = (struct spdk_rdma_mem_map *)0xdeadbeef;
	rqpair.rdma_qp = (struct spdk_rdma_qp *)0xdeadbeef;
	rqpair.qpair.ctrlr = &ctrlr;
	rqpair.cmds = &cmd;
	cmd.sgl[0].address = 0x1111;
	rdma_req.id = 0;
	rdma_req.req = &req;

	req.payload.contig_or_cb_arg = (void *)0xdeadbeef;
	req.qpair = &rqpair.qpair;

	/* Test case 1: single inline SGL. Expected: PASS */
	req.payload_offset = 0;
	req.payload_size = 0x1000;
	rc = nvme_rdma_build_contig_inline_request(&rqpair, &rdma_req);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.length == req.payload_size);
	CU_ASSERT(req.cmd.dptr.sgl1.address == 0);
	CU_ASSERT(rdma_req.send_sgl[0].length == sizeof(struct spdk_nvme_cmd));
	CU_ASSERT(rdma_req.send_sgl[1].length == req.payload_size);
	CU_ASSERT(rdma_req.send_sgl[1].addr == (uint64_t)req.payload.contig_or_cb_arg);
	CU_ASSERT(rdma_req.send_sgl[1].lkey == RDMA_UT_LKEY);

	/* Test case 2: SGL length exceeds 3 bytes. Expected: PASS */
	req.payload_offset = 0;
	req.payload_size = 1 << 24;
	rc = nvme_rdma_build_contig_inline_request(&rqpair, &rdma_req);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.length == req.payload_size);
	CU_ASSERT(req.cmd.dptr.sgl1.address == 0);
	CU_ASSERT(rdma_req.send_sgl[0].length == sizeof(struct spdk_nvme_cmd));
	CU_ASSERT(rdma_req.send_sgl[1].length == req.payload_size);
	CU_ASSERT(rdma_req.send_sgl[1].addr == (uint64_t)req.payload.contig_or_cb_arg);
	CU_ASSERT(rdma_req.send_sgl[1].lkey == RDMA_UT_LKEY);
}

static void
test_nvme_rdma_create_reqs(void)
{
	struct nvme_rdma_qpair rqpair = {};
	int rc;

	memset(&g_nvme_hooks, 0, sizeof(g_nvme_hooks));

	/* Test case 1: zero entry. Expect: FAIL */
	rqpair.num_entries = 0;

	rc = nvme_rdma_create_reqs(&rqpair);
	CU_ASSERT(rqpair.rdma_reqs == NULL);
	SPDK_CU_ASSERT_FATAL(rc == -ENOMEM);

	/* Test case 2: single entry. Expect: PASS */
	memset(&rqpair, 0, sizeof(rqpair));
	rqpair.num_entries = 1;

	rc = nvme_rdma_create_reqs(&rqpair);
	CU_ASSERT(rc == 0);
	CU_ASSERT(rqpair.rdma_reqs[0].send_sgl[0].lkey == g_rdma_mr.lkey);
	CU_ASSERT(rqpair.rdma_reqs[0].send_sgl[0].addr
		  == (uint64_t)&rqpair.cmds[0]);
	CU_ASSERT(rqpair.rdma_reqs[0].send_wr.wr_id
		  == (uint64_t)&rqpair.rdma_reqs[0].rdma_wr);
	CU_ASSERT(rqpair.rdma_reqs[0].send_wr.next == NULL);
	CU_ASSERT(rqpair.rdma_reqs[0].send_wr.opcode == IBV_WR_SEND);
	CU_ASSERT(rqpair.rdma_reqs[0].send_wr.send_flags == IBV_SEND_SIGNALED);
	CU_ASSERT(rqpair.rdma_reqs[0].send_wr.sg_list
		  == rqpair.rdma_reqs[0].send_sgl);
	CU_ASSERT(rqpair.rdma_reqs[0].send_wr.imm_data == 0);
	spdk_free(rqpair.rdma_reqs);
	spdk_free(rqpair.cmds);

	/* Test case 3: multiple entries. Expect: PASS */
	memset(&rqpair, 0, sizeof(rqpair));
	rqpair.num_entries = 5;

	rc = nvme_rdma_create_reqs(&rqpair);
	CU_ASSERT(rc == 0);
	for (int i = 0; i < 5; i++) {
		CU_ASSERT(rqpair.rdma_reqs[i].send_sgl[0].lkey == g_rdma_mr.lkey);
		CU_ASSERT(rqpair.rdma_reqs[i].send_sgl[0].addr
			  == (uint64_t)&rqpair.cmds[i]);
		CU_ASSERT(rqpair.rdma_reqs[i].send_wr.wr_id
			  == (uint64_t)&rqpair.rdma_reqs[i].rdma_wr);
		CU_ASSERT(rqpair.rdma_reqs[i].send_wr.next == NULL);
		CU_ASSERT(rqpair.rdma_reqs[i].send_wr.opcode == IBV_WR_SEND);
		CU_ASSERT(rqpair.rdma_reqs[i].send_wr.send_flags
			  == IBV_SEND_SIGNALED);
		CU_ASSERT(rqpair.rdma_reqs[i].send_wr.sg_list
			  == rqpair.rdma_reqs[i].send_sgl);
		CU_ASSERT(rqpair.rdma_reqs[i].send_wr.imm_data == 0);
	}
	spdk_free(rqpair.rdma_reqs);
	spdk_free(rqpair.cmds);
}

static void
test_nvme_rdma_create_rsps(void)
{
	struct nvme_rdma_rsp_opts opts = {};
	struct nvme_rdma_rsps *rsps;
	struct spdk_rdma_qp *rdma_qp = (struct spdk_rdma_qp *)0xfeedf00d;
	struct nvme_rdma_qpair rqpair = { .rdma_qp = rdma_qp, };

	memset(&g_nvme_hooks, 0, sizeof(g_nvme_hooks));

	opts.rqpair = &rqpair;

	/* Test case 1 calloc false */
	opts.num_entries = 0;
	rsps = nvme_rdma_create_rsps(&opts);
	SPDK_CU_ASSERT_FATAL(rsps == NULL);

	/* Test case 2 calloc success */
	opts.num_entries = 1;

	rsps = nvme_rdma_create_rsps(&opts);
	SPDK_CU_ASSERT_FATAL(rsps != NULL);
	CU_ASSERT(rsps->rsp_sgls != NULL);
	CU_ASSERT(rsps->rsp_recv_wrs != NULL);
	CU_ASSERT(rsps->rsps != NULL);
	CU_ASSERT(rsps->rsp_sgls[0].lkey == g_rdma_mr.lkey);
	CU_ASSERT(rsps->rsp_sgls[0].addr == (uint64_t)&rsps->rsps[0]);
	CU_ASSERT(rsps->rsp_recv_wrs[0].wr_id == (uint64_t)&rsps->rsps[0].rdma_wr);

	nvme_rdma_free_rsps(rsps);
}

static void
test_nvme_rdma_ctrlr_create_qpair(void)
{
	struct spdk_nvme_ctrlr ctrlr = {};
	uint16_t qid, qsize;
	struct spdk_nvme_qpair *qpair;
	struct nvme_rdma_qpair *rqpair;

	/* Test case 1: max qsize. Expect: PASS */
	qsize = 0xffff;
	qid = 1;

	qpair = nvme_rdma_ctrlr_create_qpair(&ctrlr, qid, qsize,
					     SPDK_NVME_QPRIO_URGENT, 1,
					     false, false);
	CU_ASSERT(qpair != NULL);
	rqpair = SPDK_CONTAINEROF(qpair, struct nvme_rdma_qpair, qpair);
	CU_ASSERT(qpair == &rqpair->qpair);
	CU_ASSERT(rqpair->num_entries == qsize - 1);
	CU_ASSERT(rqpair->delay_cmd_submit == false);

	spdk_free(rqpair);
	rqpair = NULL;

	/* Test case 2: queue size 2. Expect: PASS */
	qsize = 2;
	qpair = nvme_rdma_ctrlr_create_qpair(&ctrlr, qid, qsize,
					     SPDK_NVME_QPRIO_URGENT, 1,
					     false, false);
	CU_ASSERT(qpair != NULL);
	rqpair = SPDK_CONTAINEROF(qpair, struct nvme_rdma_qpair, qpair);
	CU_ASSERT(rqpair->num_entries == qsize - 1);

	spdk_free(rqpair);
	rqpair = NULL;

	/* Test case 3: queue size zero. Expect: FAIL */
	qsize = 0;

	qpair = nvme_rdma_ctrlr_create_qpair(&ctrlr, qid, qsize,
					     SPDK_NVME_QPRIO_URGENT, 1,
					     false, false);
	SPDK_CU_ASSERT_FATAL(qpair == NULL);

	/* Test case 4: queue size 1. Expect: FAIL */
	qsize = 1;
	qpair = nvme_rdma_ctrlr_create_qpair(&ctrlr, qid, qsize,
					     SPDK_NVME_QPRIO_URGENT, 1,
					     false, false);
	SPDK_CU_ASSERT_FATAL(qpair == NULL);
}

DEFINE_STUB(ibv_create_cq, struct ibv_cq *, (struct ibv_context *context, int cqe, void *cq_context,
		struct ibv_comp_channel *channel, int comp_vector), (struct ibv_cq *)0xFEEDBEEF);
DEFINE_STUB(ibv_destroy_cq, int, (struct ibv_cq *cq), 0);

static void
test_nvme_rdma_poller_create(void)
{
	struct nvme_rdma_poll_group	group = {};
	struct ibv_context context = {
		.device = (struct ibv_device *)0xDEADBEEF
	};
	struct ibv_context context_2 = {
		.device = (struct ibv_device *)0xBAADBEEF
	};
	struct nvme_rdma_poller *poller_1, *poller_2, *poller_3;

	/* Case: calloc and ibv not need to fail test */
	STAILQ_INIT(&group.pollers);

	poller_1 = nvme_rdma_poll_group_get_poller(&group, &context);
	SPDK_CU_ASSERT_FATAL(poller_1 != NULL);
	CU_ASSERT(group.num_pollers == 1);
	CU_ASSERT(STAILQ_FIRST(&group.pollers) == poller_1);
	CU_ASSERT(poller_1->refcnt == 1);
	CU_ASSERT(poller_1->device == &context);
	CU_ASSERT(poller_1->cq == (struct ibv_cq *)0xFEEDBEEF);
	CU_ASSERT(poller_1->current_num_wc == DEFAULT_NVME_RDMA_CQ_SIZE);
	CU_ASSERT(poller_1->required_num_wc == 0);

	poller_2 = nvme_rdma_poll_group_get_poller(&group, &context_2);
	SPDK_CU_ASSERT_FATAL(poller_2 != NULL);
	CU_ASSERT(group.num_pollers == 2);
	CU_ASSERT(STAILQ_FIRST(&group.pollers) == poller_2);
	CU_ASSERT(poller_2->refcnt == 1);
	CU_ASSERT(poller_2->device == &context_2);

	poller_3 = nvme_rdma_poll_group_get_poller(&group, &context);
	SPDK_CU_ASSERT_FATAL(poller_3 != NULL);
	CU_ASSERT(poller_3 == poller_1);
	CU_ASSERT(group.num_pollers == 2);
	CU_ASSERT(poller_3->refcnt == 2);

	nvme_rdma_poll_group_put_poller(&group, poller_2);
	CU_ASSERT(group.num_pollers == 1);

	nvme_rdma_poll_group_put_poller(&group, poller_1);
	CU_ASSERT(group.num_pollers == 1);
	CU_ASSERT(poller_3->refcnt == 1);

	nvme_rdma_poll_group_put_poller(&group, poller_3);
	CU_ASSERT(STAILQ_EMPTY(&group.pollers));
	CU_ASSERT(group.num_pollers == 0);

	nvme_rdma_poll_group_free_pollers(&group);
}

static void
test_nvme_rdma_qpair_process_cm_event(void)
{
	struct nvme_rdma_qpair rqpair = {};
	struct rdma_cm_event	 event = {};
	struct spdk_nvmf_rdma_accept_private_data	accept_data = {};
	int rc = 0;

	/* case1: event == RDMA_CM_EVENT_ADDR_RESOLVED */
	rqpair.evt = &event;
	event.event = RDMA_CM_EVENT_ADDR_RESOLVED;
	rc = nvme_rdma_qpair_process_cm_event(&rqpair);
	CU_ASSERT(rc == 0);

	/* case2: event == RDMA_CM_EVENT_CONNECT_REQUEST */
	rqpair.evt = &event;
	event.event = RDMA_CM_EVENT_CONNECT_REQUEST;
	rc = nvme_rdma_qpair_process_cm_event(&rqpair);
	CU_ASSERT(rc == 0);

	/* case3: event == RDMA_CM_EVENT_CONNECT_ERROR */
	rqpair.evt = &event;
	event.event = RDMA_CM_EVENT_CONNECT_ERROR;
	rc = nvme_rdma_qpair_process_cm_event(&rqpair);
	CU_ASSERT(rc == 0);

	/* case4: event == RDMA_CM_EVENT_UNREACHABLE */
	rqpair.evt = &event;
	event.event = RDMA_CM_EVENT_UNREACHABLE;
	rc = nvme_rdma_qpair_process_cm_event(&rqpair);
	CU_ASSERT(rc == 0);

	/* case5: event == RDMA_CM_EVENT_CONNECT_RESPONSE */
	rqpair.evt = &event;
	event.event = RDMA_CM_EVENT_CONNECT_RESPONSE;
	event.param.conn.private_data = NULL;
	rc = nvme_rdma_qpair_process_cm_event(&rqpair);
	CU_ASSERT(rc == -1);

	rqpair.evt = &event;
	event.event = RDMA_CM_EVENT_CONNECT_RESPONSE;
	event.param.conn.private_data = &accept_data;
	accept_data.crqsize = 512;
	rqpair.num_entries = 1024;
	rc = nvme_rdma_qpair_process_cm_event(&rqpair);
	CU_ASSERT(rc == 0);
	CU_ASSERT(rqpair.num_entries == 1024);

	/* case6: event == RDMA_CM_EVENT_DISCONNECTED */
	rqpair.evt = &event;
	event.event = RDMA_CM_EVENT_DISCONNECTED;
	rc = nvme_rdma_qpair_process_cm_event(&rqpair);
	CU_ASSERT(rc == 0);
	CU_ASSERT(rqpair.qpair.transport_failure_reason == SPDK_NVME_QPAIR_FAILURE_REMOTE);

	/* case7: event == RDMA_CM_EVENT_DEVICE_REMOVAL */
	rqpair.evt = &event;
	event.event = RDMA_CM_EVENT_DEVICE_REMOVAL;
	rc = nvme_rdma_qpair_process_cm_event(&rqpair);
	CU_ASSERT(rc == 0);
	CU_ASSERT(rqpair.qpair.transport_failure_reason == SPDK_NVME_QPAIR_FAILURE_LOCAL);

	/* case8: event == RDMA_CM_EVENT_MULTICAST_JOIN */
	rqpair.evt = &event;
	event.event = RDMA_CM_EVENT_MULTICAST_JOIN;
	rc = nvme_rdma_qpair_process_cm_event(&rqpair);
	CU_ASSERT(rc == 0);

	/* case9: event == RDMA_CM_EVENT_ADDR_CHANGE */
	rqpair.evt = &event;
	event.event = RDMA_CM_EVENT_ADDR_CHANGE;
	rc = nvme_rdma_qpair_process_cm_event(&rqpair);
	CU_ASSERT(rc == 0);
	CU_ASSERT(rqpair.qpair.transport_failure_reason == SPDK_NVME_QPAIR_FAILURE_LOCAL);

	/* case10: event == RDMA_CM_EVENT_TIMEWAIT_EXIT */
	rqpair.evt = &event;
	event.event = RDMA_CM_EVENT_TIMEWAIT_EXIT;
	rc = nvme_rdma_qpair_process_cm_event(&rqpair);
	CU_ASSERT(rc == 0);

	/* case11: default event == 0xFF */
	rqpair.evt = &event;
	event.event = 0xFF;
	rc = nvme_rdma_qpair_process_cm_event(&rqpair);
	CU_ASSERT(rc == 0);
}

static void
test_nvme_rdma_ctrlr_construct(void)
{
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_transport_id trid = {};
	struct spdk_nvme_ctrlr_opts opts = {};
	struct nvme_rdma_qpair *rqpair = NULL;
	struct nvme_rdma_ctrlr *rctrlr = NULL;
	struct rdma_event_channel cm_channel = {};
	void *devhandle = NULL;
	int rc;

	opts.transport_retry_count = NVME_RDMA_CTRLR_MAX_TRANSPORT_RETRY_COUNT + 1;
	opts.transport_ack_timeout = NVME_RDMA_CTRLR_MAX_TRANSPORT_ACK_TIMEOUT + 1;
	opts.admin_queue_size = 0xFFFF;
	trid.trtype = SPDK_NVME_TRANSPORT_RDMA;
	trid.adrfam = SPDK_NVMF_ADRFAM_IPV4;
	MOCK_SET(rdma_create_event_channel, &cm_channel);

	ctrlr = nvme_rdma_ctrlr_construct(&trid, &opts, devhandle);
	SPDK_CU_ASSERT_FATAL(ctrlr != NULL);
	CU_ASSERT(ctrlr->opts.transport_retry_count ==
		  NVME_RDMA_CTRLR_MAX_TRANSPORT_RETRY_COUNT);
	CU_ASSERT(ctrlr->opts.transport_ack_timeout ==
		  NVME_RDMA_CTRLR_MAX_TRANSPORT_ACK_TIMEOUT);
	CU_ASSERT(ctrlr->opts.admin_queue_size == opts.admin_queue_size);
	rctrlr = SPDK_CONTAINEROF(ctrlr, struct nvme_rdma_ctrlr, ctrlr);
	CU_ASSERT(rctrlr->max_sge == NVME_RDMA_MAX_SGL_DESCRIPTORS);
	CU_ASSERT(rctrlr->cm_channel == &cm_channel);
	CU_ASSERT(!strncmp((char *)&rctrlr->ctrlr.trid,
			   (char *)&trid, sizeof(trid)));

	SPDK_CU_ASSERT_FATAL(ctrlr->adminq != NULL);
	rqpair = SPDK_CONTAINEROF(ctrlr->adminq, struct nvme_rdma_qpair, qpair);
	CU_ASSERT(rqpair->num_entries == opts.admin_queue_size - 1);
	CU_ASSERT(rqpair->delay_cmd_submit == false);
	MOCK_CLEAR(rdma_create_event_channel);

	/* Hardcode the trtype, because nvme_qpair_init() is stub function. */
	rqpair->qpair.trtype = SPDK_NVME_TRANSPORT_RDMA;
	rc = nvme_rdma_ctrlr_destruct(ctrlr);
	CU_ASSERT(rc == 0);
}

static void
test_nvme_rdma_req_put_and_get(void)
{
	struct nvme_rdma_qpair rqpair = {};
	struct spdk_nvme_rdma_req rdma_req = {};
	struct spdk_nvme_rdma_req *rdma_req_get;

	/* case 1: nvme_rdma_req_put */
	TAILQ_INIT(&rqpair.free_reqs);
	rdma_req.completion_flags = 1;
	rdma_req.req = (struct nvme_request *)0xDEADBEFF;
	rdma_req.id = 10086;
	nvme_rdma_req_put(&rqpair, &rdma_req);

	CU_ASSERT(rqpair.free_reqs.tqh_first == &rdma_req);
	CU_ASSERT(rqpair.free_reqs.tqh_first->completion_flags == 0);
	CU_ASSERT(rqpair.free_reqs.tqh_first->req == NULL);
	CU_ASSERT(rqpair.free_reqs.tqh_first->id == 10086);
	CU_ASSERT(rdma_req.completion_flags == 0);
	CU_ASSERT(rdma_req.req == NULL);

	/* case 2: nvme_rdma_req_get */
	TAILQ_INIT(&rqpair.outstanding_reqs);
	rdma_req_get = nvme_rdma_req_get(&rqpair);
	CU_ASSERT(rdma_req_get == &rdma_req);
	CU_ASSERT(rdma_req_get->id == 10086);
	CU_ASSERT(rqpair.free_reqs.tqh_first == NULL);
	CU_ASSERT(rqpair.outstanding_reqs.tqh_first == rdma_req_get);
}

static void
test_nvme_rdma_req_init(void)
{
	struct nvme_rdma_qpair rqpair = {};
	struct spdk_nvme_ctrlr ctrlr = {};
	struct spdk_nvmf_cmd cmd = {};
	struct spdk_nvme_rdma_req rdma_req = {};
	struct nvme_request req = {};
	struct nvme_rdma_ut_bdev_io bio = { .iovcnt = NVME_RDMA_MAX_SGL_DESCRIPTORS };
	int rc = 1;

	ctrlr.max_sges = NVME_RDMA_MAX_SGL_DESCRIPTORS;
	ctrlr.cdata.nvmf_specific.msdbd = 16;

	rqpair.mr_map = (struct spdk_rdma_mem_map *)0xdeadbeef;
	rqpair.rdma_qp = (struct spdk_rdma_qp *)0xdeadbeef;
	rqpair.qpair.ctrlr = &ctrlr;
	rqpair.cmds = &cmd;
	cmd.sgl[0].address = 0x1111;
	rdma_req.id = 0;
	req.cmd.opc = SPDK_NVME_DATA_HOST_TO_CONTROLLER;

	req.payload.contig_or_cb_arg = (void *)0xdeadbeef;
	/* case 1: req->payload_size == 0, expect: pass. */
	req.payload_size = 0;
	rqpair.qpair.ctrlr->ioccsz_bytes = 1024;
	rqpair.qpair.ctrlr->icdoff = 0;
	rc = nvme_rdma_req_init(&rqpair, &req, &rdma_req);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req.cmd.psdt == SPDK_NVME_PSDT_SGL_MPTR_CONTIG);
	CU_ASSERT(rdma_req.send_sgl[0].length == sizeof(struct spdk_nvme_cmd));
	CU_ASSERT(rdma_req.send_wr.num_sge == 1);
	CU_ASSERT(req.cmd.dptr.sgl1.keyed.type == SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK);
	CU_ASSERT(req.cmd.dptr.sgl1.keyed.subtype == SPDK_NVME_SGL_SUBTYPE_ADDRESS);
	CU_ASSERT(req.cmd.dptr.sgl1.keyed.length == 0);
	CU_ASSERT(req.cmd.dptr.sgl1.keyed.key == 0);
	CU_ASSERT(req.cmd.dptr.sgl1.address == 0);

	/* case 2: payload_type == NVME_PAYLOAD_TYPE_CONTIG, expect: pass. */
	/* icd_supported is true */
	rdma_req.req = NULL;
	rqpair.qpair.ctrlr->icdoff = 0;
	req.payload_offset = 0;
	req.payload_size = 1024;
	req.payload.reset_sgl_fn = NULL;
	rc = nvme_rdma_req_init(&rqpair, &req, &rdma_req);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.length == req.payload_size);
	CU_ASSERT(req.cmd.dptr.sgl1.address == 0);
	CU_ASSERT(rdma_req.send_sgl[0].length == sizeof(struct spdk_nvme_cmd));
	CU_ASSERT(rdma_req.send_sgl[1].length == req.payload_size);
	CU_ASSERT(rdma_req.send_sgl[1].addr == (uint64_t)req.payload.contig_or_cb_arg);
	CU_ASSERT(rdma_req.send_sgl[1].lkey == RDMA_UT_LKEY);

	/* icd_supported is false */
	rdma_req.req = NULL;
	rqpair.qpair.ctrlr->icdoff = 1;
	req.payload_offset = 0;
	req.payload_size = 1024;
	req.payload.reset_sgl_fn = NULL;
	rc = nvme_rdma_req_init(&rqpair, &req, &rdma_req);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req.cmd.dptr.sgl1.keyed.type == SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK);
	CU_ASSERT(req.cmd.dptr.sgl1.keyed.subtype == SPDK_NVME_SGL_SUBTYPE_ADDRESS);
	CU_ASSERT(req.cmd.dptr.sgl1.keyed.length == req.payload_size);
	CU_ASSERT(req.cmd.dptr.sgl1.keyed.key == RDMA_UT_RKEY);
	CU_ASSERT(req.cmd.dptr.sgl1.address == (uint64_t)req.payload.contig_or_cb_arg);
	CU_ASSERT(rdma_req.send_sgl[0].length == sizeof(struct spdk_nvme_cmd));

	/* case 3: payload_type == NVME_PAYLOAD_TYPE_SGL, expect: pass. */
	/* icd_supported is true */
	rdma_req.req = NULL;
	rqpair.qpair.ctrlr->icdoff = 0;
	req.payload.reset_sgl_fn = nvme_rdma_ut_reset_sgl;
	req.payload.next_sge_fn = nvme_rdma_ut_next_sge;
	req.payload.contig_or_cb_arg = &bio;
	req.qpair = &rqpair.qpair;
	bio.iovpos = 0;
	req.payload_offset = 0;
	req.payload_size = 1024;
	bio.iovs[0].iov_base = (void *)0xdeadbeef;
	bio.iovs[0].iov_len = 1024;
	rc = nvme_rdma_req_init(&rqpair, &req, &rdma_req);
	CU_ASSERT(rc == 0);
	CU_ASSERT(bio.iovpos == 1);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET);
	CU_ASSERT(req.cmd.dptr.sgl1.unkeyed.length == req.payload_size);
	CU_ASSERT(req.cmd.dptr.sgl1.address == 0);
	CU_ASSERT(rdma_req.send_sgl[0].length == sizeof(struct spdk_nvme_cmd));
	CU_ASSERT(rdma_req.send_sgl[1].length == req.payload_size);
	CU_ASSERT(rdma_req.send_sgl[1].addr == (uint64_t)bio.iovs[0].iov_base);
	CU_ASSERT(rdma_req.send_sgl[1].lkey == RDMA_UT_LKEY);

	/* icd_supported is false */
	rdma_req.req = NULL;
	rqpair.qpair.ctrlr->icdoff = 1;
	req.payload.reset_sgl_fn = nvme_rdma_ut_reset_sgl;
	req.payload.next_sge_fn = nvme_rdma_ut_next_sge;
	req.payload.contig_or_cb_arg = &bio;
	req.qpair = &rqpair.qpair;
	bio.iovpos = 0;
	req.payload_offset = 0;
	req.payload_size = 1024;
	bio.iovs[0].iov_base = (void *)0xdeadbeef;
	bio.iovs[0].iov_len = 1024;
	rc = nvme_rdma_req_init(&rqpair, &req, &rdma_req);
	CU_ASSERT(rc == 0);
	CU_ASSERT(bio.iovpos == 1);
	CU_ASSERT(req.cmd.dptr.sgl1.keyed.type == SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK);
	CU_ASSERT(req.cmd.dptr.sgl1.keyed.subtype == SPDK_NVME_SGL_SUBTYPE_ADDRESS);
	CU_ASSERT(req.cmd.dptr.sgl1.keyed.length == req.payload_size);
	CU_ASSERT(req.cmd.dptr.sgl1.keyed.key == RDMA_UT_RKEY);
	CU_ASSERT(req.cmd.dptr.sgl1.address == (uint64_t)bio.iovs[0].iov_base);
	CU_ASSERT(rdma_req.send_sgl[0].length == sizeof(struct spdk_nvme_cmd));
}

static void
test_nvme_rdma_validate_cm_event(void)
{
	enum rdma_cm_event_type expected_evt_type;
	struct rdma_cm_event reaped_evt = {};
	int rc;

	/* case 1: expected_evt_type == reaped_evt->event, expect: pass */
	expected_evt_type = RDMA_CM_EVENT_ADDR_RESOLVED;
	reaped_evt.event = RDMA_CM_EVENT_ADDR_RESOLVED;

	rc = nvme_rdma_validate_cm_event(expected_evt_type, &reaped_evt);
	CU_ASSERT(rc == 0);

	/* case 2: expected_evt_type != RDMA_CM_EVENT_ESTABLISHED and is not equal to reaped_evt->event, expect: fail */
	reaped_evt.event = RDMA_CM_EVENT_CONNECT_RESPONSE;

	rc = nvme_rdma_validate_cm_event(expected_evt_type, &reaped_evt);
	CU_ASSERT(rc == -EBADMSG);

	/* case 3: expected_evt_type == RDMA_CM_EVENT_ESTABLISHED */
	expected_evt_type = RDMA_CM_EVENT_ESTABLISHED;
	/* reaped_evt->event == RDMA_CM_EVENT_REJECTED and reaped_evt->status == 10, expect: fail */
	reaped_evt.event = RDMA_CM_EVENT_REJECTED;
	reaped_evt.status = 10;

	rc = nvme_rdma_validate_cm_event(expected_evt_type, &reaped_evt);
	CU_ASSERT(rc == -ESTALE);

	/* reaped_evt->event == RDMA_CM_EVENT_CONNECT_RESPONSE, expect: pass */
	reaped_evt.event = RDMA_CM_EVENT_CONNECT_RESPONSE;

	rc = nvme_rdma_validate_cm_event(expected_evt_type, &reaped_evt);
	CU_ASSERT(rc == 0);
}

static void
test_nvme_rdma_parse_addr(void)
{
	struct sockaddr_storage dst_addr;
	int rc = 0;

	memset(&dst_addr, 0, sizeof(dst_addr));
	/* case1: getaddrinfo failed */
	rc = nvme_rdma_parse_addr(&dst_addr, AF_INET, NULL, NULL);
	CU_ASSERT(rc != 0);

	/* case2: res->ai_addrlen < sizeof(*sa). Expect: Pass. */
	rc = nvme_rdma_parse_addr(&dst_addr, AF_INET, "12.34.56.78", "23");
	CU_ASSERT(rc == 0);
	CU_ASSERT(dst_addr.ss_family == AF_INET);
}

static void
test_nvme_rdma_qpair_init(void)
{
	struct nvme_rdma_qpair		rqpair = {};
	struct rdma_cm_id		 cm_id = {};
	struct ibv_pd				*pd = (struct ibv_pd *)0xfeedbeef;
	struct ibv_qp				qp = { .pd = pd };
	struct nvme_rdma_ctrlr	rctrlr = {};
	int rc = 0;

	rctrlr.ctrlr.trid.trtype = SPDK_NVME_TRANSPORT_RDMA;
	rqpair.cm_id = &cm_id;
	g_nvme_hooks.get_ibv_pd = NULL;
	rqpair.qpair.poll_group = NULL;
	rqpair.qpair.ctrlr = &rctrlr.ctrlr;
	g_spdk_rdma_qp.qp = &qp;
	MOCK_SET(spdk_rdma_get_pd, pd);

	rc = nvme_rdma_qpair_init(&rqpair);
	CU_ASSERT(rc == 0);

	CU_ASSERT(rqpair.cm_id->context == &rqpair.qpair);
	CU_ASSERT(rqpair.max_send_sge == NVME_RDMA_DEFAULT_TX_SGE);
	CU_ASSERT(rqpair.max_recv_sge == NVME_RDMA_DEFAULT_RX_SGE);
	CU_ASSERT(rqpair.current_num_sends == 0);
	CU_ASSERT(rqpair.cq == (struct ibv_cq *)0xFEEDBEEF);
	CU_ASSERT(rqpair.memory_domain != NULL);

	MOCK_CLEAR(spdk_rdma_get_pd);
}

static void
test_nvme_rdma_qpair_submit_request(void)
{
	int				rc;
	struct nvme_rdma_qpair		rqpair = {};
	struct spdk_nvme_ctrlr		ctrlr = {};
	struct nvme_request		req = {};
	struct nvme_rdma_poller		poller = {};
	struct spdk_nvme_rdma_req	*rdma_req = NULL;

	req.cmd.opc = SPDK_NVME_DATA_HOST_TO_CONTROLLER;
	req.payload.contig_or_cb_arg = (void *)0xdeadbeef;
	req.payload_size = 0;
	rqpair.mr_map = (struct spdk_rdma_mem_map *)0xdeadbeef;
	rqpair.rdma_qp = (struct spdk_rdma_qp *)0xdeadbeef;
	rqpair.qpair.ctrlr = &ctrlr;
	rqpair.num_entries = 1;
	rqpair.qpair.trtype = SPDK_NVME_TRANSPORT_RDMA;
	rqpair.poller = &poller;

	rc = nvme_rdma_create_reqs(&rqpair);
	CU_ASSERT(rc == 0);
	/* Give send_wr.next a non null value */
	rdma_req = TAILQ_FIRST(&rqpair.free_reqs);
	SPDK_CU_ASSERT_FATAL(rdma_req != NULL);
	rdma_req->send_wr.next = (void *)0xdeadbeef;

	rc = nvme_rdma_qpair_submit_request(&rqpair.qpair, &req);
	CU_ASSERT(rc == 0);
	CU_ASSERT(rqpair.current_num_sends == 1);
	CU_ASSERT(rdma_req->send_wr.next == NULL);
	TAILQ_REMOVE(&rqpair.outstanding_reqs, rdma_req, link);
	CU_ASSERT(TAILQ_EMPTY(&rqpair.outstanding_reqs));

	/* No request available */
	rc = nvme_rdma_qpair_submit_request(&rqpair.qpair, &req);
	CU_ASSERT(rc == -EAGAIN);
	CU_ASSERT(rqpair.poller->stats.queued_requests == 1);

	nvme_rdma_free_reqs(&rqpair);
}

static void
test_nvme_rdma_memory_domain(void)
{
	struct nvme_rdma_memory_domain *domain_1 = NULL, *domain_2 = NULL, *domain_tmp;
	struct ibv_pd *pd_1 = (struct ibv_pd *)0x1, *pd_2 = (struct ibv_pd *)0x2;
	/* Counters below are used to check the number of created/destroyed rdma_dma_device objects.
	 * Since other unit tests may create dma_devices, we can't just check that the queue is empty or not */
	uint32_t dma_dev_count_start = 0, dma_dev_count = 0, dma_dev_count_end = 0;

	TAILQ_FOREACH(domain_tmp, &g_memory_domains, link) {
		dma_dev_count_start++;
	}

	/* spdk_memory_domain_create failed, expect fail */
	MOCK_SET(spdk_memory_domain_create, -1);
	domain_1 = nvme_rdma_get_memory_domain(pd_1);
	CU_ASSERT(domain_1 == NULL);
	MOCK_CLEAR(spdk_memory_domain_create);

	/* Normal scenario */
	domain_1 = nvme_rdma_get_memory_domain(pd_1);
	SPDK_CU_ASSERT_FATAL(domain_1 != NULL);
	CU_ASSERT(domain_1->domain != NULL);
	CU_ASSERT(domain_1->pd == pd_1);
	CU_ASSERT(domain_1->ref == 1);

	/* Request the same pd, ref counter increased */
	CU_ASSERT(nvme_rdma_get_memory_domain(pd_1) == domain_1);
	CU_ASSERT(domain_1->ref == 2);

	/* Request another pd */
	domain_2 = nvme_rdma_get_memory_domain(pd_2);
	SPDK_CU_ASSERT_FATAL(domain_2 != NULL);
	CU_ASSERT(domain_2->domain != NULL);
	CU_ASSERT(domain_2->pd == pd_2);
	CU_ASSERT(domain_2->ref == 1);

	TAILQ_FOREACH(domain_tmp, &g_memory_domains, link) {
		dma_dev_count++;
	}
	CU_ASSERT(dma_dev_count == dma_dev_count_start + 2);

	/* put domain_1, decrement refcount */
	nvme_rdma_put_memory_domain(domain_1);

	/* Release both devices */
	CU_ASSERT(domain_2->ref == 1);
	nvme_rdma_put_memory_domain(domain_1);
	nvme_rdma_put_memory_domain(domain_2);

	TAILQ_FOREACH(domain_tmp, &g_memory_domains, link) {
		dma_dev_count_end++;
	}
	CU_ASSERT(dma_dev_count_start == dma_dev_count_end);
}

static void
test_rdma_ctrlr_get_memory_domains(void)
{
	struct nvme_rdma_ctrlr rctrlr = {};
	struct nvme_rdma_qpair rqpair = {};
	struct spdk_memory_domain *domain = (struct spdk_memory_domain *)0xbaadbeef;
	struct nvme_rdma_memory_domain rdma_domain = { .domain = domain };
	struct spdk_memory_domain *domains[1] = {NULL};

	rqpair.memory_domain = &rdma_domain;
	rqpair.qpair.trtype = SPDK_NVME_TRANSPORT_RDMA;
	rctrlr.ctrlr.adminq = &rqpair.qpair;

	/* Test 1, input domains pointer is NULL */
	CU_ASSERT(nvme_rdma_ctrlr_get_memory_domains(&rctrlr.ctrlr, NULL, 1) == 1);

	/* Test 2, input array_size is 0 */
	CU_ASSERT(nvme_rdma_ctrlr_get_memory_domains(&rctrlr.ctrlr, domains, 0) == 1);
	CU_ASSERT(domains[0] == NULL);

	/* Test 3, both input domains pointer and array_size are NULL/0 */
	CU_ASSERT(nvme_rdma_ctrlr_get_memory_domains(&rctrlr.ctrlr, NULL, 0) == 1);

	/* Test 2, input parameters are valid */
	CU_ASSERT(nvme_rdma_ctrlr_get_memory_domains(&rctrlr.ctrlr, domains, 1) == 1);
	CU_ASSERT(domains[0] == domain);
}

static void
test_rdma_get_memory_translation(void)
{
	struct ibv_qp qp = {.pd = (struct ibv_pd *) 0xfeedbeef};
	struct spdk_rdma_qp rdma_qp = {.qp = &qp};
	struct nvme_rdma_qpair rqpair = {.rdma_qp = &rdma_qp};
	struct spdk_nvme_ns_cmd_ext_io_opts io_opts = {
		.memory_domain = (struct spdk_memory_domain *) 0xdeaddead
	};
	struct nvme_request req = {.payload = {.opts = &io_opts}};
	struct nvme_rdma_memory_translation_ctx ctx = {
		.addr = (void *) 0xBAADF00D,
		.length = 0x100
	};
	int rc;

	rqpair.memory_domain = nvme_rdma_get_memory_domain(rqpair.rdma_qp->qp->pd);
	SPDK_CU_ASSERT_FATAL(rqpair.memory_domain != NULL);

	/* case 1, using extended IO opts with DMA device.
	 * Test 1 - spdk_dma_translate_data error, expect fail */
	MOCK_SET(spdk_memory_domain_translate_data, -1);
	rc = nvme_rdma_get_memory_translation(&req, &rqpair, &ctx);
	CU_ASSERT(rc != 0);
	MOCK_CLEAR(spdk_memory_domain_translate_data);

	/* Test 2 - expect pass */
	g_memory_translation_translation.iov_count = 1;
	g_memory_translation_translation.iov.iov_base = ctx.addr + 1;
	g_memory_translation_translation.iov.iov_len = ctx.length;
	g_memory_translation_translation.rdma.lkey = 123;
	g_memory_translation_translation.rdma.rkey = 321;

	rc = nvme_rdma_get_memory_translation(&req, &rqpair, &ctx);
	CU_ASSERT(rc == 0);
	CU_ASSERT(ctx.lkey == g_memory_translation_translation.rdma.lkey);
	CU_ASSERT(ctx.rkey == g_memory_translation_translation.rdma.rkey);
	CU_ASSERT(ctx.addr == g_memory_translation_translation.iov.iov_base);
	CU_ASSERT(ctx.length == g_memory_translation_translation.iov.iov_len);

	/* case 2, using rdma translation
	 * Test 1 - spdk_rdma_get_translation error, expect fail */
	req.payload.opts = NULL;
	MOCK_SET(spdk_rdma_get_translation, -1);
	rc = nvme_rdma_get_memory_translation(&req, &rqpair, &ctx);
	CU_ASSERT(rc != 0);
	MOCK_CLEAR(spdk_rdma_get_translation);

	/* Test 2 - expect pass */
	rc = nvme_rdma_get_memory_translation(&req, &rqpair, &ctx);
	CU_ASSERT(rc == 0);
	CU_ASSERT(ctx.lkey == RDMA_UT_LKEY);
	CU_ASSERT(ctx.rkey == RDMA_UT_RKEY);

	/* Cleanup */
	nvme_rdma_put_memory_domain(rqpair.memory_domain);
}

static void
test_get_rdma_qpair_from_wc(void)
{
	const uint32_t test_qp_num = 123;
	struct nvme_rdma_poll_group	group = {};
	struct nvme_rdma_qpair rqpair = {};
	struct spdk_rdma_qp rdma_qp = {};
	struct ibv_qp qp = { .qp_num = test_qp_num };
	struct ibv_wc wc = { .qp_num = test_qp_num };

	STAILQ_INIT(&group.group.disconnected_qpairs);
	STAILQ_INIT(&group.group.connected_qpairs);
	rqpair.qpair.trtype = SPDK_NVME_TRANSPORT_RDMA;

	/* Test 1 - Simulate case when nvme_rdma_qpair is disconnected but still in one of lists.
	 * get_rdma_qpair_from_wc must return NULL */
	STAILQ_INSERT_HEAD(&group.group.disconnected_qpairs, &rqpair.qpair, poll_group_stailq);
	CU_ASSERT(get_rdma_qpair_from_wc(&group, &wc) == NULL);
	STAILQ_REMOVE_HEAD(&group.group.disconnected_qpairs, poll_group_stailq);

	STAILQ_INSERT_HEAD(&group.group.connected_qpairs, &rqpair.qpair, poll_group_stailq);
	CU_ASSERT(get_rdma_qpair_from_wc(&group, &wc) == NULL);
	STAILQ_REMOVE_HEAD(&group.group.connected_qpairs, poll_group_stailq);

	/* Test 2 - nvme_rdma_qpair with valid rdma_qp/ibv_qp and qp_num */
	rdma_qp.qp = &qp;
	rqpair.rdma_qp = &rdma_qp;

	STAILQ_INSERT_HEAD(&group.group.disconnected_qpairs, &rqpair.qpair, poll_group_stailq);
	CU_ASSERT(get_rdma_qpair_from_wc(&group, &wc) == &rqpair);
	STAILQ_REMOVE_HEAD(&group.group.disconnected_qpairs, poll_group_stailq);

	STAILQ_INSERT_HEAD(&group.group.connected_qpairs, &rqpair.qpair, poll_group_stailq);
	CU_ASSERT(get_rdma_qpair_from_wc(&group, &wc) == &rqpair);
	STAILQ_REMOVE_HEAD(&group.group.connected_qpairs, poll_group_stailq);
}

static void
test_nvme_rdma_ctrlr_get_max_sges(void)
{
	struct nvme_rdma_ctrlr	rctrlr = {};

	rctrlr.ctrlr.trid.trtype = SPDK_NVME_TRANSPORT_RDMA;
	rctrlr.max_sge = NVME_RDMA_MAX_SGL_DESCRIPTORS;
	rctrlr.ctrlr.cdata.nvmf_specific.msdbd = 16;
	rctrlr.ctrlr.cdata.nvmf_specific.ioccsz = 4096;
	CU_ASSERT(nvme_rdma_ctrlr_get_max_sges(&rctrlr.ctrlr) == 16);

	rctrlr.ctrlr.cdata.nvmf_specific.msdbd = 32;
	rctrlr.ctrlr.cdata.nvmf_specific.ioccsz = 4096;
	CU_ASSERT(nvme_rdma_ctrlr_get_max_sges(&rctrlr.ctrlr) == 16);

	rctrlr.ctrlr.cdata.nvmf_specific.msdbd = 8;
	rctrlr.ctrlr.cdata.nvmf_specific.ioccsz = 4096;
	CU_ASSERT(nvme_rdma_ctrlr_get_max_sges(&rctrlr.ctrlr) == 8);

	rctrlr.ctrlr.cdata.nvmf_specific.msdbd = 16;
	rctrlr.ctrlr.cdata.nvmf_specific.ioccsz = 4;
	CU_ASSERT(nvme_rdma_ctrlr_get_max_sges(&rctrlr.ctrlr) == 1);

	rctrlr.ctrlr.cdata.nvmf_specific.msdbd = 16;
	rctrlr.ctrlr.cdata.nvmf_specific.ioccsz = 6;
	CU_ASSERT(nvme_rdma_ctrlr_get_max_sges(&rctrlr.ctrlr) == 2);
}

static void
test_nvme_rdma_poll_group_get_stats(void)
{
	int rc = -1;
	struct spdk_nvme_transport_poll_group_stat *tpointer = NULL;
	struct nvme_rdma_poll_group tgroup = {};
	struct ibv_device dev1, dev2 = {};
	struct ibv_context contexts1, contexts2 = {};
	struct nvme_rdma_poller *tpoller1 = NULL;
	struct nvme_rdma_poller *tpoller2 = NULL;

	memcpy(dev1.name, "/dev/test1", sizeof("/dev/test1"));
	memcpy(dev2.name, "/dev/test2", sizeof("/dev/test2"));
	contexts1.device = &dev1;
	contexts2.device = &dev2;

	/* Initialization */
	STAILQ_INIT(&tgroup.pollers);
	tpoller2 = nvme_rdma_poller_create(&tgroup, &contexts1);
	SPDK_CU_ASSERT_FATAL(tpoller2 != NULL);
	CU_ASSERT(tgroup.num_pollers == 1);

	tpoller1 = nvme_rdma_poller_create(&tgroup, &contexts2);
	SPDK_CU_ASSERT_FATAL(tpoller1 != NULL);
	CU_ASSERT(tgroup.num_pollers == 2);
	CU_ASSERT(&tgroup.pollers != NULL);

	CU_ASSERT(tpoller1->device == &contexts2);
	CU_ASSERT(tpoller2->device == &contexts1);
	CU_ASSERT(strcmp(tpoller1->device->device->name, "/dev/test2") == 0);
	CU_ASSERT(strcmp(tpoller2->device->device->name, "/dev/test1") == 0);
	CU_ASSERT(tpoller1->current_num_wc == DEFAULT_NVME_RDMA_CQ_SIZE);
	CU_ASSERT(tpoller2->current_num_wc == DEFAULT_NVME_RDMA_CQ_SIZE);
	CU_ASSERT(tpoller1->required_num_wc == 0);
	CU_ASSERT(tpoller2->required_num_wc == 0);

	/* Test1: Invalid stats */
	rc = nvme_rdma_poll_group_get_stats(NULL, &tpointer);
	CU_ASSERT(rc == -EINVAL);

	/* Test2: Invalid group pointer */
	rc = nvme_rdma_poll_group_get_stats(&tgroup.group, NULL);
	CU_ASSERT(rc == -EINVAL);

	/* Test3: Success member variables should be correct */
	tpoller1->stats.polls = 111;
	tpoller1->stats.idle_polls = 112;
	tpoller1->stats.completions = 113;
	tpoller1->stats.queued_requests = 114;
	tpoller1->stats.rdma_stats.send.num_submitted_wrs = 121;
	tpoller1->stats.rdma_stats.send.doorbell_updates = 122;
	tpoller1->stats.rdma_stats.recv.num_submitted_wrs = 131;
	tpoller1->stats.rdma_stats.recv.doorbell_updates = 132;
	tpoller2->stats.polls = 211;
	tpoller2->stats.idle_polls = 212;
	tpoller2->stats.completions = 213;
	tpoller2->stats.queued_requests = 214;
	tpoller2->stats.rdma_stats.send.num_submitted_wrs = 221;
	tpoller2->stats.rdma_stats.send.doorbell_updates = 222;
	tpoller2->stats.rdma_stats.recv.num_submitted_wrs = 231;
	tpoller2->stats.rdma_stats.recv.doorbell_updates = 232;

	rc = nvme_rdma_poll_group_get_stats(&tgroup.group, &tpointer);
	CU_ASSERT(rc == 0);
	CU_ASSERT(tpointer != NULL);
	CU_ASSERT(tpointer->trtype == SPDK_NVME_TRANSPORT_RDMA);
	CU_ASSERT(tpointer->rdma.num_devices == tgroup.num_pollers);
	CU_ASSERT(tpointer->rdma.device_stats != NULL);

	CU_ASSERT(strcmp(tpointer->rdma.device_stats[0].name, "/dev/test2") == 0);
	CU_ASSERT(tpointer->rdma.device_stats[0].polls == 111);
	CU_ASSERT(tpointer->rdma.device_stats[0].idle_polls == 112);
	CU_ASSERT(tpointer->rdma.device_stats[0].completions == 113);
	CU_ASSERT(tpointer->rdma.device_stats[0].queued_requests == 114);
	CU_ASSERT(tpointer->rdma.device_stats[0].total_send_wrs == 121);
	CU_ASSERT(tpointer->rdma.device_stats[0].send_doorbell_updates == 122);
	CU_ASSERT(tpointer->rdma.device_stats[0].total_recv_wrs == 131);
	CU_ASSERT(tpointer->rdma.device_stats[0].recv_doorbell_updates == 132);

	CU_ASSERT(strcmp(tpointer->rdma.device_stats[1].name, "/dev/test1") == 0);
	CU_ASSERT(tpointer->rdma.device_stats[1].polls == 211);
	CU_ASSERT(tpointer->rdma.device_stats[1].idle_polls == 212);
	CU_ASSERT(tpointer->rdma.device_stats[1].completions == 213);
	CU_ASSERT(tpointer->rdma.device_stats[1].queued_requests == 214);
	CU_ASSERT(tpointer->rdma.device_stats[1].total_send_wrs == 221);
	CU_ASSERT(tpointer->rdma.device_stats[1].send_doorbell_updates == 222);
	CU_ASSERT(tpointer->rdma.device_stats[1].total_recv_wrs == 231);
	CU_ASSERT(tpointer->rdma.device_stats[1].recv_doorbell_updates == 232);

	nvme_rdma_poll_group_free_stats(&tgroup.group, tpointer);
	nvme_rdma_poll_group_free_pollers(&tgroup);
}

static void
test_nvme_rdma_qpair_set_poller(void)
{
	int rc = -1;
	struct nvme_rdma_poll_group *group;
	struct spdk_nvme_transport_poll_group *tgroup;
	struct nvme_rdma_poller *poller;
	struct nvme_rdma_qpair rqpair = {};
	struct rdma_cm_id cm_id = {};

	/* Case1: Test function nvme_rdma_poll_group_create */
	/* Test1: Function nvme_rdma_poll_group_create success */
	tgroup = nvme_rdma_poll_group_create();
	SPDK_CU_ASSERT_FATAL(tgroup != NULL);

	group = nvme_rdma_poll_group(tgroup);
	CU_ASSERT(group != NULL);
	CU_ASSERT(STAILQ_EMPTY(&group->pollers));

	/* Case2: Test function nvme_rdma_qpair_set_poller */
	rqpair.qpair.poll_group = tgroup;
	rqpair.qpair.trtype = SPDK_NVME_TRANSPORT_RDMA;
	rqpair.cm_id = &cm_id;

	/* Test1: Function ibv_create_cq failed */
	cm_id.verbs = (void *)0xFEEDBEEF;
	MOCK_SET(ibv_create_cq, NULL);

	rc = nvme_rdma_qpair_set_poller(&rqpair.qpair);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(rqpair.cq == NULL);
	CU_ASSERT(STAILQ_EMPTY(&group->pollers));

	MOCK_CLEAR(ibv_create_cq);

	/* Test2: Unable to find a cq for qpair on poll group */
	cm_id.verbs = NULL;

	rc = nvme_rdma_qpair_set_poller(&rqpair.qpair);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(rqpair.cq == NULL);
	CU_ASSERT(STAILQ_EMPTY(&group->pollers));

	/* Test3: Match cq success, current_num_wc is enough */
	MOCK_SET(ibv_create_cq, (struct ibv_cq *)0xFEEDBEEF);

	cm_id.verbs = (void *)0xFEEDBEEF;
	rqpair.num_entries = 0;

	rc = nvme_rdma_qpair_set_poller(&rqpair.qpair);
	CU_ASSERT(rc == 0);
	CU_ASSERT(rqpair.cq == (void *)0xFEEDBEEF);

	poller = STAILQ_FIRST(&group->pollers);
	SPDK_CU_ASSERT_FATAL(poller != NULL);
	CU_ASSERT(STAILQ_NEXT(poller, link) == NULL);
	CU_ASSERT(poller->device == (struct ibv_context *)0xFEEDBEEF);
	CU_ASSERT(poller->current_num_wc == DEFAULT_NVME_RDMA_CQ_SIZE);
	CU_ASSERT(poller->required_num_wc == 0);
	CU_ASSERT(rqpair.poller == poller);

	rqpair.qpair.poll_group_tailq_head = &tgroup->disconnected_qpairs;

	rc = nvme_rdma_poll_group_remove(tgroup, &rqpair.qpair);
	CU_ASSERT(rc == 0);
	CU_ASSERT(rqpair.cq == NULL);
	CU_ASSERT(rqpair.poller == NULL);
	CU_ASSERT(STAILQ_EMPTY(&group->pollers));

	rqpair.qpair.poll_group_tailq_head = &tgroup->connected_qpairs;

	/* Test4: Match cq success, function ibv_resize_cq failed */
	rqpair.cq = NULL;
	rqpair.num_entries = DEFAULT_NVME_RDMA_CQ_SIZE - 1;
	MOCK_SET(ibv_resize_cq, -1);

	rc = nvme_rdma_qpair_set_poller(&rqpair.qpair);
	CU_ASSERT(rc == -EPROTO);
	CU_ASSERT(STAILQ_EMPTY(&group->pollers));

	/* Test5: Current_num_wc is not enough, resize success */
	MOCK_SET(ibv_resize_cq, 0);

	rc = nvme_rdma_qpair_set_poller(&rqpair.qpair);
	CU_ASSERT(rc == 0);

	poller = STAILQ_FIRST(&group->pollers);
	SPDK_CU_ASSERT_FATAL(poller != NULL);
	CU_ASSERT(poller->current_num_wc == DEFAULT_NVME_RDMA_CQ_SIZE * 2);
	CU_ASSERT(poller->required_num_wc == (DEFAULT_NVME_RDMA_CQ_SIZE - 1) * 2);
	CU_ASSERT(rqpair.cq == poller->cq);
	CU_ASSERT(rqpair.poller == poller);

	rqpair.qpair.poll_group_tailq_head = &tgroup->disconnected_qpairs;

	rc = nvme_rdma_poll_group_remove(tgroup, &rqpair.qpair);
	CU_ASSERT(rc == 0);

	rc = nvme_rdma_poll_group_destroy(tgroup);
	CU_ASSERT(rc == 0);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("nvme_rdma", NULL, NULL);
	CU_ADD_TEST(suite, test_nvme_rdma_build_sgl_request);
	CU_ADD_TEST(suite, test_nvme_rdma_build_sgl_inline_request);
	CU_ADD_TEST(suite, test_nvme_rdma_build_contig_request);
	CU_ADD_TEST(suite, test_nvme_rdma_build_contig_inline_request);
	CU_ADD_TEST(suite, test_nvme_rdma_create_reqs);
	CU_ADD_TEST(suite, test_nvme_rdma_create_rsps);
	CU_ADD_TEST(suite, test_nvme_rdma_ctrlr_create_qpair);
	CU_ADD_TEST(suite, test_nvme_rdma_poller_create);
	CU_ADD_TEST(suite, test_nvme_rdma_qpair_process_cm_event);
	CU_ADD_TEST(suite, test_nvme_rdma_ctrlr_construct);
	CU_ADD_TEST(suite, test_nvme_rdma_req_put_and_get);
	CU_ADD_TEST(suite, test_nvme_rdma_req_init);
	CU_ADD_TEST(suite, test_nvme_rdma_validate_cm_event);
	CU_ADD_TEST(suite, test_nvme_rdma_parse_addr);
	CU_ADD_TEST(suite, test_nvme_rdma_qpair_init);
	CU_ADD_TEST(suite, test_nvme_rdma_qpair_submit_request);
	CU_ADD_TEST(suite, test_nvme_rdma_memory_domain);
	CU_ADD_TEST(suite, test_rdma_ctrlr_get_memory_domains);
	CU_ADD_TEST(suite, test_rdma_get_memory_translation);
	CU_ADD_TEST(suite, test_get_rdma_qpair_from_wc);
	CU_ADD_TEST(suite, test_nvme_rdma_ctrlr_get_max_sges);
	CU_ADD_TEST(suite, test_nvme_rdma_poll_group_get_stats);
	CU_ADD_TEST(suite, test_nvme_rdma_qpair_set_poller);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
