/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019, 2021 Mellanox Technologies LTD. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk_cunit.h"
#include "common/lib/test_env.c"
#include "common/lib/test_rdma.c"
#include "nvmf/rdma.c"
#include "nvmf/transport.c"

#define RDMA_UT_UNITS_IN_MAX_IO 16

struct spdk_nvmf_transport_opts g_rdma_ut_transport_opts = {
	.max_queue_depth = SPDK_NVMF_RDMA_DEFAULT_MAX_QUEUE_DEPTH,
	.max_qpairs_per_ctrlr = SPDK_NVMF_RDMA_DEFAULT_MAX_QPAIRS_PER_CTRLR,
	.in_capsule_data_size = SPDK_NVMF_RDMA_DEFAULT_IN_CAPSULE_DATA_SIZE,
	.max_io_size = (SPDK_NVMF_RDMA_MIN_IO_BUFFER_SIZE * RDMA_UT_UNITS_IN_MAX_IO),
	.io_unit_size = SPDK_NVMF_RDMA_MIN_IO_BUFFER_SIZE,
	.max_aq_depth = SPDK_NVMF_RDMA_DEFAULT_AQ_DEPTH,
	.num_shared_buffers = SPDK_NVMF_RDMA_DEFAULT_NUM_SHARED_BUFFERS,
};

SPDK_LOG_REGISTER_COMPONENT(nvmf)
DEFINE_STUB(spdk_mem_map_set_translation, int, (struct spdk_mem_map *map, uint64_t vaddr,
		uint64_t size, uint64_t translation), 0);
DEFINE_STUB(spdk_mem_map_clear_translation, int, (struct spdk_mem_map *map, uint64_t vaddr,
		uint64_t size), 0);
DEFINE_STUB(spdk_mem_map_alloc, struct spdk_mem_map *, (uint64_t default_translation,
		const struct spdk_mem_map_ops *ops, void *cb_ctx), NULL);
DEFINE_STUB(spdk_nvmf_qpair_disconnect, int, (struct spdk_nvmf_qpair *qpair,
		nvmf_qpair_disconnect_cb cb_fn, void *ctx), 0);
DEFINE_STUB(spdk_nvmf_qpair_get_listen_trid, int,
	    (struct spdk_nvmf_qpair *qpair, struct spdk_nvme_transport_id *trid), 0);
DEFINE_STUB_V(spdk_mem_map_free, (struct spdk_mem_map **pmap));

DEFINE_STUB_V(spdk_nvmf_ctrlr_data_init, (struct spdk_nvmf_transport_opts *opts,
		struct spdk_nvmf_ctrlr_data *cdata));
DEFINE_STUB_V(spdk_nvmf_request_exec, (struct spdk_nvmf_request *req));
DEFINE_STUB(spdk_nvmf_request_complete, int, (struct spdk_nvmf_request *req), 0);
DEFINE_STUB(spdk_nvme_transport_id_compare, int, (const struct spdk_nvme_transport_id *trid1,
		const struct spdk_nvme_transport_id *trid2), 0);
DEFINE_STUB_V(nvmf_ctrlr_abort_aer, (struct spdk_nvmf_ctrlr *ctrlr));
DEFINE_STUB(spdk_nvmf_request_get_dif_ctx, bool, (struct spdk_nvmf_request *req,
		struct spdk_dif_ctx *dif_ctx), false);
DEFINE_STUB_V(spdk_nvme_trid_populate_transport, (struct spdk_nvme_transport_id *trid,
		enum spdk_nvme_transport_type trtype));
DEFINE_STUB_V(spdk_nvmf_tgt_new_qpair, (struct spdk_nvmf_tgt *tgt, struct spdk_nvmf_qpair *qpair));
DEFINE_STUB(nvmf_ctrlr_abort_request, int, (struct spdk_nvmf_request *req), 0);
DEFINE_STUB(spdk_nvme_transport_id_adrfam_str, const char *, (enum spdk_nvmf_adrfam adrfam), NULL);
DEFINE_STUB(ibv_dereg_mr, int, (struct ibv_mr *mr), 0);
DEFINE_STUB(ibv_resize_cq, int, (struct ibv_cq *cq, int cqe), 0);
DEFINE_STUB(spdk_mempool_lookup, struct spdk_mempool *, (const char *name), NULL);

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

int
ibv_query_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr,
	     int attr_mask, struct ibv_qp_init_attr *init_attr)
{
	if (qp == NULL) {
		return -1;
	} else {
		attr->port_num = 80;

		if (qp->state == IBV_QPS_ERR) {
			attr->qp_state = 10;
		} else {
			attr->qp_state = IBV_QPS_INIT;
		}

		return 0;
	}
}

const char *
spdk_nvme_transport_id_trtype_str(enum spdk_nvme_transport_type trtype)
{
	switch (trtype) {
	case SPDK_NVME_TRANSPORT_PCIE:
		return "PCIe";
	case SPDK_NVME_TRANSPORT_RDMA:
		return "RDMA";
	case SPDK_NVME_TRANSPORT_FC:
		return "FC";
	default:
		return NULL;
	}
}

int
spdk_nvme_transport_id_populate_trstring(struct spdk_nvme_transport_id *trid, const char *trstring)
{
	int len, i;

	if (trstring == NULL) {
		return -EINVAL;
	}

	len = strnlen(trstring, SPDK_NVMF_TRSTRING_MAX_LEN);
	if (len == SPDK_NVMF_TRSTRING_MAX_LEN) {
		return -EINVAL;
	}

	/* cast official trstring to uppercase version of input. */
	for (i = 0; i < len; i++) {
		trid->trstring[i] = toupper(trstring[i]);
	}
	return 0;
}

static void
reset_nvmf_rdma_request(struct spdk_nvmf_rdma_request *rdma_req)
{
	int i;

	rdma_req->req.length = 0;
	rdma_req->req.data_from_pool = false;
	rdma_req->req.data = NULL;
	rdma_req->data.wr.num_sge = 0;
	rdma_req->data.wr.wr.rdma.remote_addr = 0;
	rdma_req->data.wr.wr.rdma.rkey = 0;
	rdma_req->offset = 0;
	memset(&rdma_req->req.dif, 0, sizeof(rdma_req->req.dif));

	for (i = 0; i < SPDK_NVMF_MAX_SGL_ENTRIES; i++) {
		rdma_req->req.iov[i].iov_base = 0;
		rdma_req->req.iov[i].iov_len = 0;
		rdma_req->req.buffers[i] = 0;
		rdma_req->data.wr.sg_list[i].addr = 0;
		rdma_req->data.wr.sg_list[i].length = 0;
		rdma_req->data.wr.sg_list[i].lkey = 0;
	}
	rdma_req->req.iovcnt = 0;
	if (rdma_req->req.stripped_data) {
		free(rdma_req->req.stripped_data);
		rdma_req->req.stripped_data = NULL;
	}
}

static void
test_spdk_nvmf_rdma_request_parse_sgl(void)
{
	struct spdk_nvmf_rdma_transport rtransport;
	struct spdk_nvmf_rdma_device device;
	struct spdk_nvmf_rdma_request rdma_req = {};
	struct spdk_nvmf_rdma_recv recv;
	struct spdk_nvmf_rdma_poll_group group;
	struct spdk_nvmf_rdma_qpair rqpair;
	struct spdk_nvmf_rdma_poller poller;
	union nvmf_c2h_msg cpl;
	union nvmf_h2c_msg cmd;
	struct spdk_nvme_sgl_descriptor *sgl;
	struct spdk_nvmf_transport_pg_cache_buf bufs[4];
	struct spdk_nvme_sgl_descriptor sgl_desc[SPDK_NVMF_MAX_SGL_ENTRIES] = {{0}};
	struct spdk_nvmf_rdma_request_data data;
	int rc, i;
	uint32_t sgl_length;
	uintptr_t aligned_buffer_address;

	data.wr.sg_list = data.sgl;
	STAILQ_INIT(&group.group.buf_cache);
	group.group.buf_cache_size = 0;
	group.group.buf_cache_count = 0;
	group.group.transport = &rtransport.transport;
	poller.group = &group;
	rqpair.poller = &poller;
	rqpair.max_send_sge = SPDK_NVMF_MAX_SGL_ENTRIES;

	sgl = &cmd.nvme_cmd.dptr.sgl1;
	rdma_req.recv = &recv;
	rdma_req.req.cmd = &cmd;
	rdma_req.req.rsp = &cpl;
	rdma_req.data.wr.sg_list = rdma_req.data.sgl;
	rdma_req.req.qpair = &rqpair.qpair;
	rdma_req.req.xfer = SPDK_NVME_DATA_CONTROLLER_TO_HOST;

	rtransport.transport.opts = g_rdma_ut_transport_opts;
	rtransport.data_wr_pool = NULL;
	rtransport.transport.data_buf_pool = NULL;

	device.attr.device_cap_flags = 0;
	sgl->keyed.key = 0xEEEE;
	sgl->address = 0xFFFF;
	rdma_req.recv->buf = (void *)0xDDDD;

	/* Test 1: sgl type: keyed data block subtype: address */
	sgl->generic.type = SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK;
	sgl->keyed.subtype = SPDK_NVME_SGL_SUBTYPE_ADDRESS;

	/* Part 1: simple I/O, one SGL smaller than the transport io unit size */
	MOCK_SET(spdk_mempool_get, (void *)0x2000);
	reset_nvmf_rdma_request(&rdma_req);
	sgl->keyed.length = rtransport.transport.opts.io_unit_size / 2;

	device.map = (void *)0x0;
	rc = nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);
	CU_ASSERT(rc == 0);
	CU_ASSERT(rdma_req.req.data_from_pool == true);
	CU_ASSERT(rdma_req.req.length == rtransport.transport.opts.io_unit_size / 2);
	CU_ASSERT((uint64_t)rdma_req.req.data == 0x2000);
	CU_ASSERT(rdma_req.data.wr.num_sge == 1);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.rkey == 0xEEEE);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.remote_addr == 0xFFFF);
	CU_ASSERT((uint64_t)rdma_req.req.buffers[0] == 0x2000);
	CU_ASSERT(rdma_req.data.wr.sg_list[0].addr == 0x2000);
	CU_ASSERT(rdma_req.data.wr.sg_list[0].length == rtransport.transport.opts.io_unit_size / 2);
	CU_ASSERT(rdma_req.data.wr.sg_list[0].lkey == RDMA_UT_LKEY);

	/* Part 2: simple I/O, one SGL larger than the transport io unit size (equal to the max io size) */
	reset_nvmf_rdma_request(&rdma_req);
	sgl->keyed.length = rtransport.transport.opts.io_unit_size * RDMA_UT_UNITS_IN_MAX_IO;
	rc = nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == 0);
	CU_ASSERT(rdma_req.req.data_from_pool == true);
	CU_ASSERT(rdma_req.req.length == rtransport.transport.opts.io_unit_size * RDMA_UT_UNITS_IN_MAX_IO);
	CU_ASSERT(rdma_req.data.wr.num_sge == RDMA_UT_UNITS_IN_MAX_IO);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.rkey == 0xEEEE);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.remote_addr == 0xFFFF);
	for (i = 0; i < RDMA_UT_UNITS_IN_MAX_IO; i++) {
		CU_ASSERT((uint64_t)rdma_req.req.buffers[i] == 0x2000);
		CU_ASSERT(rdma_req.data.wr.sg_list[i].addr == 0x2000);
		CU_ASSERT(rdma_req.data.wr.sg_list[i].length == rtransport.transport.opts.io_unit_size);
		CU_ASSERT(rdma_req.data.wr.sg_list[i].lkey == RDMA_UT_LKEY);
	}

	/* Part 3: simple I/O one SGL larger than the transport max io size */
	reset_nvmf_rdma_request(&rdma_req);
	sgl->keyed.length = rtransport.transport.opts.max_io_size * 2;
	rc = nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == -1);

	/* Part 4: Pretend there are no buffer pools */
	MOCK_SET(spdk_mempool_get, NULL);
	reset_nvmf_rdma_request(&rdma_req);
	sgl->keyed.length = rtransport.transport.opts.io_unit_size * RDMA_UT_UNITS_IN_MAX_IO;
	rc = nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == 0);
	CU_ASSERT(rdma_req.req.data_from_pool == false);
	CU_ASSERT(rdma_req.req.data == NULL);
	CU_ASSERT(rdma_req.data.wr.num_sge == 0);
	CU_ASSERT(rdma_req.req.buffers[0] == NULL);
	CU_ASSERT(rdma_req.data.wr.sg_list[0].addr == 0);
	CU_ASSERT(rdma_req.data.wr.sg_list[0].length == 0);
	CU_ASSERT(rdma_req.data.wr.sg_list[0].lkey == 0);

	rdma_req.recv->buf = (void *)0xDDDD;
	/* Test 2: sgl type: keyed data block subtype: offset (in capsule data) */
	sgl->generic.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
	sgl->unkeyed.subtype = SPDK_NVME_SGL_SUBTYPE_OFFSET;

	/* Part 1: Normal I/O smaller than in capsule data size no offset */
	reset_nvmf_rdma_request(&rdma_req);
	sgl->address = 0;
	sgl->unkeyed.length = rtransport.transport.opts.in_capsule_data_size;
	rc = nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == 0);
	CU_ASSERT(rdma_req.req.data == (void *)0xDDDD);
	CU_ASSERT(rdma_req.req.length == rtransport.transport.opts.in_capsule_data_size);
	CU_ASSERT(rdma_req.req.data_from_pool == false);

	/* Part 2: I/O offset + length too large */
	reset_nvmf_rdma_request(&rdma_req);
	sgl->address = rtransport.transport.opts.in_capsule_data_size;
	sgl->unkeyed.length = rtransport.transport.opts.in_capsule_data_size;
	rc = nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == -1);

	/* Part 3: I/O too large */
	reset_nvmf_rdma_request(&rdma_req);
	sgl->address = 0;
	sgl->unkeyed.length = rtransport.transport.opts.in_capsule_data_size * 2;
	rc = nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == -1);

	/* Test 3: Multi SGL */
	sgl->generic.type = SPDK_NVME_SGL_TYPE_LAST_SEGMENT;
	sgl->unkeyed.subtype = SPDK_NVME_SGL_SUBTYPE_OFFSET;
	sgl->address = 0;
	rdma_req.recv->buf = (void *)&sgl_desc;
	MOCK_SET(spdk_mempool_get, &data);

	/* part 1: 2 segments each with 1 wr. */
	reset_nvmf_rdma_request(&rdma_req);
	sgl->unkeyed.length = 2 * sizeof(struct spdk_nvme_sgl_descriptor);
	for (i = 0; i < 2; i++) {
		sgl_desc[i].keyed.type = SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK;
		sgl_desc[i].keyed.subtype = SPDK_NVME_SGL_SUBTYPE_ADDRESS;
		sgl_desc[i].keyed.length = rtransport.transport.opts.io_unit_size;
		sgl_desc[i].address = 0x4000 + i * rtransport.transport.opts.io_unit_size;
		sgl_desc[i].keyed.key = 0x44;
	}

	rc = nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == 0);
	CU_ASSERT(rdma_req.req.data_from_pool == true);
	CU_ASSERT(rdma_req.req.length == rtransport.transport.opts.io_unit_size * 2);
	CU_ASSERT(rdma_req.data.wr.num_sge == 1);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.rkey == 0x44);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.remote_addr == 0x4000);
	CU_ASSERT(rdma_req.data.wr.next == &data.wr);
	CU_ASSERT(data.wr.wr.rdma.rkey == 0x44);
	CU_ASSERT(data.wr.wr.rdma.remote_addr == 0x4000 + rtransport.transport.opts.io_unit_size);
	CU_ASSERT(data.wr.num_sge == 1);
	CU_ASSERT(data.wr.next == &rdma_req.rsp.wr);

	/* part 2: 2 segments, each with 1 wr containing 8 sge_elements */
	reset_nvmf_rdma_request(&rdma_req);
	sgl->unkeyed.length = 2 * sizeof(struct spdk_nvme_sgl_descriptor);
	for (i = 0; i < 2; i++) {
		sgl_desc[i].keyed.type = SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK;
		sgl_desc[i].keyed.subtype = SPDK_NVME_SGL_SUBTYPE_ADDRESS;
		sgl_desc[i].keyed.length = rtransport.transport.opts.io_unit_size * 8;
		sgl_desc[i].address = 0x4000 + i * 8 * rtransport.transport.opts.io_unit_size;
		sgl_desc[i].keyed.key = 0x44;
	}

	rc = nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == 0);
	CU_ASSERT(rdma_req.req.data_from_pool == true);
	CU_ASSERT(rdma_req.req.length == rtransport.transport.opts.io_unit_size * 16);
	CU_ASSERT(rdma_req.req.iovcnt == 16);
	CU_ASSERT(rdma_req.data.wr.num_sge == 8);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.rkey == 0x44);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.remote_addr == 0x4000);
	CU_ASSERT(rdma_req.data.wr.next == &data.wr);
	CU_ASSERT(data.wr.wr.rdma.rkey == 0x44);
	CU_ASSERT(data.wr.wr.rdma.remote_addr == 0x4000 + rtransport.transport.opts.io_unit_size * 8);
	CU_ASSERT(data.wr.num_sge == 8);
	CU_ASSERT(data.wr.next == &rdma_req.rsp.wr);

	/* part 3: 2 segments, one very large, one very small */
	reset_nvmf_rdma_request(&rdma_req);
	for (i = 0; i < 2; i++) {
		sgl_desc[i].keyed.type = SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK;
		sgl_desc[i].keyed.subtype = SPDK_NVME_SGL_SUBTYPE_ADDRESS;
		sgl_desc[i].keyed.key = 0x44;
	}

	sgl_desc[0].keyed.length = rtransport.transport.opts.io_unit_size * 15 +
				   rtransport.transport.opts.io_unit_size / 2;
	sgl_desc[0].address = 0x4000;
	sgl_desc[1].keyed.length = rtransport.transport.opts.io_unit_size / 2;
	sgl_desc[1].address = 0x4000 + rtransport.transport.opts.io_unit_size * 15 +
			      rtransport.transport.opts.io_unit_size / 2;

	rc = nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == 0);
	CU_ASSERT(rdma_req.req.data_from_pool == true);
	CU_ASSERT(rdma_req.req.length == rtransport.transport.opts.io_unit_size * 16);
	CU_ASSERT(rdma_req.req.iovcnt == 16);
	CU_ASSERT(rdma_req.data.wr.num_sge == 16);
	for (i = 0; i < 15; i++) {
		CU_ASSERT(rdma_req.data.sgl[i].length == rtransport.transport.opts.io_unit_size);
	}
	CU_ASSERT(rdma_req.data.sgl[15].length == rtransport.transport.opts.io_unit_size / 2);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.rkey == 0x44);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.remote_addr == 0x4000);
	CU_ASSERT(rdma_req.data.wr.next == &data.wr);
	CU_ASSERT(data.wr.wr.rdma.rkey == 0x44);
	CU_ASSERT(data.wr.wr.rdma.remote_addr == 0x4000 + rtransport.transport.opts.io_unit_size * 15 +
		  rtransport.transport.opts.io_unit_size / 2);
	CU_ASSERT(data.sgl[0].length == rtransport.transport.opts.io_unit_size / 2);
	CU_ASSERT(data.wr.num_sge == 1);
	CU_ASSERT(data.wr.next == &rdma_req.rsp.wr);

	/* part 4: 2 SGL descriptors, each length is transport buffer / 2
	 * 1 transport buffers should be allocated */
	reset_nvmf_rdma_request(&rdma_req);
	aligned_buffer_address = ((uintptr_t)(&data) + NVMF_DATA_BUFFER_MASK) & ~NVMF_DATA_BUFFER_MASK;
	sgl->unkeyed.length = 2 * sizeof(struct spdk_nvme_sgl_descriptor);
	sgl_length = rtransport.transport.opts.io_unit_size / 2;
	for (i = 0; i < 2; i++) {
		sgl_desc[i].keyed.length = sgl_length;
		sgl_desc[i].address = 0x4000 + i * sgl_length;
	}

	rc = nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == 0);
	CU_ASSERT(rdma_req.req.data_from_pool == true);
	CU_ASSERT(rdma_req.req.length == rtransport.transport.opts.io_unit_size);
	CU_ASSERT(rdma_req.req.iovcnt == 1);

	CU_ASSERT(rdma_req.data.sgl[0].length == sgl_length);
	/* We mocked mempool_get to return address of data variable. Mempool is used
	 * to get both additional WRs and data buffers, so data points to &data */
	CU_ASSERT(rdma_req.data.sgl[0].addr == aligned_buffer_address);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.rkey == 0x44);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.remote_addr == 0x4000);
	CU_ASSERT(rdma_req.data.wr.num_sge == 1);
	CU_ASSERT(rdma_req.data.wr.next == &data.wr);

	CU_ASSERT(data.wr.wr.rdma.rkey == 0x44);
	CU_ASSERT(data.wr.wr.rdma.remote_addr == 0x4000 + sgl_length);
	CU_ASSERT(data.sgl[0].length == sgl_length);
	CU_ASSERT(data.sgl[0].addr == aligned_buffer_address + sgl_length);
	CU_ASSERT(data.wr.num_sge == 1);

	/* Test 4: use PG buffer cache */
	sgl->generic.type = SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK;
	sgl->keyed.subtype = SPDK_NVME_SGL_SUBTYPE_ADDRESS;
	sgl->address = 0xFFFF;
	rdma_req.recv->buf = (void *)0xDDDD;
	sgl->keyed.key = 0xEEEE;

	for (i = 0; i < 4; i++) {
		STAILQ_INSERT_TAIL(&group.group.buf_cache, &bufs[i], link);
	}

	/* part 1: use the four buffers from the pg cache */
	group.group.buf_cache_size = 4;
	group.group.buf_cache_count = 4;
	MOCK_SET(spdk_mempool_get, (void *)0x2000);
	reset_nvmf_rdma_request(&rdma_req);
	sgl->keyed.length = rtransport.transport.opts.io_unit_size * 4;
	rc = nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(rdma_req.req.data_from_pool == true);
	CU_ASSERT(rdma_req.req.length == rtransport.transport.opts.io_unit_size * 4);
	CU_ASSERT((uint64_t)rdma_req.req.data == (((uint64_t)&bufs[0] + NVMF_DATA_BUFFER_MASK) &
			~NVMF_DATA_BUFFER_MASK));
	CU_ASSERT(rdma_req.data.wr.num_sge == 4);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.rkey == 0xEEEE);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.remote_addr == 0xFFFF);
	CU_ASSERT(group.group.buf_cache_count == 0);
	CU_ASSERT(STAILQ_EMPTY(&group.group.buf_cache));
	for (i = 0; i < 4; i++) {
		CU_ASSERT((uint64_t)rdma_req.req.buffers[i] == (uint64_t)&bufs[i]);
		CU_ASSERT(rdma_req.data.wr.sg_list[i].addr == (((uint64_t)&bufs[i] + NVMF_DATA_BUFFER_MASK) &
				~NVMF_DATA_BUFFER_MASK));
		CU_ASSERT(rdma_req.data.wr.sg_list[i].length == rtransport.transport.opts.io_unit_size);
	}

	/* part 2: now that we have used the buffers from the cache, try again. We should get mempool buffers. */
	reset_nvmf_rdma_request(&rdma_req);
	rc = nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(rdma_req.req.data_from_pool == true);
	CU_ASSERT(rdma_req.req.length == rtransport.transport.opts.io_unit_size * 4);
	CU_ASSERT((uint64_t)rdma_req.req.data == 0x2000);
	CU_ASSERT(rdma_req.data.wr.num_sge == 4);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.rkey == 0xEEEE);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.remote_addr == 0xFFFF);
	CU_ASSERT(group.group.buf_cache_count == 0);
	CU_ASSERT(STAILQ_EMPTY(&group.group.buf_cache));
	for (i = 0; i < 4; i++) {
		CU_ASSERT((uint64_t)rdma_req.req.buffers[i] == 0x2000);
		CU_ASSERT(rdma_req.data.wr.sg_list[i].addr == 0x2000);
		CU_ASSERT(rdma_req.data.wr.sg_list[i].length == rtransport.transport.opts.io_unit_size);
		CU_ASSERT(group.group.buf_cache_count == 0);
	}

	/* part 3: half and half */
	group.group.buf_cache_count = 2;

	for (i = 0; i < 2; i++) {
		STAILQ_INSERT_TAIL(&group.group.buf_cache, &bufs[i], link);
	}
	reset_nvmf_rdma_request(&rdma_req);
	rc = nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(rdma_req.req.data_from_pool == true);
	CU_ASSERT(rdma_req.req.length == rtransport.transport.opts.io_unit_size * 4);
	CU_ASSERT((uint64_t)rdma_req.req.data == (((uint64_t)&bufs[0] + NVMF_DATA_BUFFER_MASK) &
			~NVMF_DATA_BUFFER_MASK));
	CU_ASSERT(rdma_req.data.wr.num_sge == 4);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.rkey == 0xEEEE);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.remote_addr == 0xFFFF);
	CU_ASSERT(group.group.buf_cache_count == 0);
	for (i = 0; i < 2; i++) {
		CU_ASSERT((uint64_t)rdma_req.req.buffers[i] == (uint64_t)&bufs[i]);
		CU_ASSERT(rdma_req.data.wr.sg_list[i].addr == (((uint64_t)&bufs[i] + NVMF_DATA_BUFFER_MASK) &
				~NVMF_DATA_BUFFER_MASK));
		CU_ASSERT(rdma_req.data.wr.sg_list[i].length == rtransport.transport.opts.io_unit_size);
	}
	for (i = 2; i < 4; i++) {
		CU_ASSERT((uint64_t)rdma_req.req.buffers[i] == 0x2000);
		CU_ASSERT(rdma_req.data.wr.sg_list[i].addr == 0x2000);
		CU_ASSERT(rdma_req.data.wr.sg_list[i].length == rtransport.transport.opts.io_unit_size);
	}

	reset_nvmf_rdma_request(&rdma_req);
}

static struct spdk_nvmf_rdma_recv *
create_recv(struct spdk_nvmf_rdma_qpair *rqpair, enum spdk_nvme_nvm_opcode opc)
{
	struct spdk_nvmf_rdma_recv *rdma_recv;
	union nvmf_h2c_msg *cmd;
	struct spdk_nvme_sgl_descriptor *sgl;

	rdma_recv = calloc(1, sizeof(*rdma_recv));
	rdma_recv->qpair = rqpair;
	cmd = calloc(1, sizeof(*cmd));
	rdma_recv->sgl[0].addr = (uintptr_t)cmd;
	cmd->nvme_cmd.opc = opc;
	sgl = &cmd->nvme_cmd.dptr.sgl1;
	sgl->keyed.key = 0xEEEE;
	sgl->address = 0xFFFF;
	sgl->keyed.type = SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK;
	sgl->keyed.subtype = SPDK_NVME_SGL_SUBTYPE_ADDRESS;
	sgl->keyed.length = 1;

	return rdma_recv;
}

static void
free_recv(struct spdk_nvmf_rdma_recv *rdma_recv)
{
	free((void *)rdma_recv->sgl[0].addr);
	free(rdma_recv);
}

static struct spdk_nvmf_rdma_request *
create_req(struct spdk_nvmf_rdma_qpair *rqpair,
	   struct spdk_nvmf_rdma_recv *rdma_recv)
{
	struct spdk_nvmf_rdma_request *rdma_req;
	union nvmf_c2h_msg *cpl;

	rdma_req = calloc(1, sizeof(*rdma_req));
	rdma_req->recv = rdma_recv;
	rdma_req->req.qpair = &rqpair->qpair;
	rdma_req->state = RDMA_REQUEST_STATE_NEW;
	rdma_req->data.wr.wr_id = (uintptr_t)&rdma_req->data.rdma_wr;
	rdma_req->data.wr.sg_list = rdma_req->data.sgl;
	cpl = calloc(1, sizeof(*cpl));
	rdma_req->rsp.sgl[0].addr = (uintptr_t)cpl;
	rdma_req->req.rsp = cpl;

	return rdma_req;
}

static void
free_req(struct spdk_nvmf_rdma_request *rdma_req)
{
	free((void *)rdma_req->rsp.sgl[0].addr);
	free(rdma_req);
}

static void
qpair_reset(struct spdk_nvmf_rdma_qpair *rqpair,
	    struct spdk_nvmf_rdma_poller *poller,
	    struct spdk_nvmf_rdma_device *device,
	    struct spdk_nvmf_rdma_resources *resources,
	    struct spdk_nvmf_transport *transport)
{
	memset(rqpair, 0, sizeof(*rqpair));
	STAILQ_INIT(&rqpair->pending_rdma_write_queue);
	STAILQ_INIT(&rqpair->pending_rdma_read_queue);
	rqpair->poller = poller;
	rqpair->device = device;
	rqpair->resources = resources;
	rqpair->qpair.qid = 1;
	rqpair->ibv_state = IBV_QPS_RTS;
	rqpair->qpair.state = SPDK_NVMF_QPAIR_ACTIVE;
	rqpair->max_send_sge = SPDK_NVMF_MAX_SGL_ENTRIES;
	rqpair->max_send_depth = 16;
	rqpair->max_read_depth = 16;
	rqpair->qpair.transport = transport;
}

static void
poller_reset(struct spdk_nvmf_rdma_poller *poller,
	     struct spdk_nvmf_rdma_poll_group *group)
{
	memset(poller, 0, sizeof(*poller));
	STAILQ_INIT(&poller->qpairs_pending_recv);
	STAILQ_INIT(&poller->qpairs_pending_send);
	poller->group = group;
}

static void
test_spdk_nvmf_rdma_request_process(void)
{
	struct spdk_nvmf_rdma_transport rtransport = {};
	struct spdk_nvmf_rdma_poll_group group = {};
	struct spdk_nvmf_rdma_poller poller = {};
	struct spdk_nvmf_rdma_device device = {};
	struct spdk_nvmf_rdma_resources resources = {};
	struct spdk_nvmf_rdma_qpair rqpair = {};
	struct spdk_nvmf_rdma_recv *rdma_recv;
	struct spdk_nvmf_rdma_request *rdma_req;
	bool progress;

	STAILQ_INIT(&group.group.buf_cache);
	STAILQ_INIT(&group.group.pending_buf_queue);
	group.group.buf_cache_size = 0;
	group.group.buf_cache_count = 0;
	poller_reset(&poller, &group);
	qpair_reset(&rqpair, &poller, &device, &resources, &rtransport.transport);

	rtransport.transport.opts = g_rdma_ut_transport_opts;
	rtransport.transport.data_buf_pool = spdk_mempool_create("test_data_pool", 16, 128, 0, 0);
	rtransport.data_wr_pool = spdk_mempool_create("test_wr_pool", 128,
				  sizeof(struct spdk_nvmf_rdma_request_data),
				  0, 0);
	MOCK_CLEAR(spdk_mempool_get);

	device.attr.device_cap_flags = 0;
	device.map = (void *)0x0;

	/* Test 1: single SGL READ request */
	rdma_recv = create_recv(&rqpair, SPDK_NVME_OPC_READ);
	rdma_req = create_req(&rqpair, rdma_recv);
	rqpair.current_recv_depth = 1;
	/* NEW -> EXECUTING */
	progress = nvmf_rdma_request_process(&rtransport, rdma_req);
	CU_ASSERT(progress == true);
	CU_ASSERT(rdma_req->state == RDMA_REQUEST_STATE_EXECUTING);
	CU_ASSERT(rdma_req->req.xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST);
	/* EXECUTED -> TRANSFERRING_C2H */
	rdma_req->state = RDMA_REQUEST_STATE_EXECUTED;
	progress = nvmf_rdma_request_process(&rtransport, rdma_req);
	CU_ASSERT(progress == true);
	CU_ASSERT(rdma_req->state == RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST);
	CU_ASSERT(rdma_req->recv == NULL);
	/* COMPLETED -> FREE */
	rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
	progress = nvmf_rdma_request_process(&rtransport, rdma_req);
	CU_ASSERT(progress == true);
	CU_ASSERT(rdma_req->state == RDMA_REQUEST_STATE_FREE);

	free_recv(rdma_recv);
	free_req(rdma_req);
	poller_reset(&poller, &group);
	qpair_reset(&rqpair, &poller, &device, &resources, &rtransport.transport);

	/* Test 2: single SGL WRITE request */
	rdma_recv = create_recv(&rqpair, SPDK_NVME_OPC_WRITE);
	rdma_req = create_req(&rqpair, rdma_recv);
	rqpair.current_recv_depth = 1;
	/* NEW -> TRANSFERRING_H2C */
	progress = nvmf_rdma_request_process(&rtransport, rdma_req);
	CU_ASSERT(progress == true);
	CU_ASSERT(rdma_req->state == RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER);
	CU_ASSERT(rdma_req->req.xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER);
	STAILQ_INIT(&poller.qpairs_pending_send);
	/* READY_TO_EXECUTE -> EXECUTING */
	rdma_req->state = RDMA_REQUEST_STATE_READY_TO_EXECUTE;
	progress = nvmf_rdma_request_process(&rtransport, rdma_req);
	CU_ASSERT(progress == true);
	CU_ASSERT(rdma_req->state == RDMA_REQUEST_STATE_EXECUTING);
	/* EXECUTED -> COMPLETING */
	rdma_req->state = RDMA_REQUEST_STATE_EXECUTED;
	progress = nvmf_rdma_request_process(&rtransport, rdma_req);
	CU_ASSERT(progress == true);
	CU_ASSERT(rdma_req->state == RDMA_REQUEST_STATE_COMPLETING);
	CU_ASSERT(rdma_req->recv == NULL);
	/* COMPLETED -> FREE */
	rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
	progress = nvmf_rdma_request_process(&rtransport, rdma_req);
	CU_ASSERT(progress == true);
	CU_ASSERT(rdma_req->state == RDMA_REQUEST_STATE_FREE);

	free_recv(rdma_recv);
	free_req(rdma_req);
	poller_reset(&poller, &group);
	qpair_reset(&rqpair, &poller, &device, &resources, &rtransport.transport);

	/* Test 3: WRITE+WRITE ibv_send batching */
	{
		struct spdk_nvmf_rdma_recv *recv1, *recv2;
		struct spdk_nvmf_rdma_request *req1, *req2;
		recv1 = create_recv(&rqpair, SPDK_NVME_OPC_WRITE);
		req1 = create_req(&rqpair, recv1);
		recv2 = create_recv(&rqpair, SPDK_NVME_OPC_WRITE);
		req2 = create_req(&rqpair, recv2);

		/* WRITE 1: NEW -> TRANSFERRING_H2C */
		rqpair.current_recv_depth = 1;
		nvmf_rdma_request_process(&rtransport, req1);
		CU_ASSERT(req1->state == RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER);

		/* WRITE 2: NEW -> TRANSFERRING_H2C */
		rqpair.current_recv_depth = 2;
		nvmf_rdma_request_process(&rtransport, req2);
		CU_ASSERT(req2->state == RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER);

		STAILQ_INIT(&poller.qpairs_pending_send);

		/* WRITE 1 completes before WRITE 2 has finished RDMA reading */
		/* WRITE 1: READY_TO_EXECUTE -> EXECUTING */
		req1->state = RDMA_REQUEST_STATE_READY_TO_EXECUTE;
		nvmf_rdma_request_process(&rtransport, req1);
		CU_ASSERT(req1->state == RDMA_REQUEST_STATE_EXECUTING);
		/* WRITE 1: EXECUTED -> COMPLETING */
		req1->state = RDMA_REQUEST_STATE_EXECUTED;
		nvmf_rdma_request_process(&rtransport, req1);
		CU_ASSERT(req1->state == RDMA_REQUEST_STATE_COMPLETING);
		STAILQ_INIT(&poller.qpairs_pending_send);
		/* WRITE 1: COMPLETED -> FREE */
		req1->state = RDMA_REQUEST_STATE_COMPLETED;
		nvmf_rdma_request_process(&rtransport, req1);
		CU_ASSERT(req1->state == RDMA_REQUEST_STATE_FREE);

		/* Now WRITE 2 has finished reading and completes */
		/* WRITE 2: COMPLETED -> FREE */
		/* WRITE 2: READY_TO_EXECUTE -> EXECUTING */
		req2->state = RDMA_REQUEST_STATE_READY_TO_EXECUTE;
		nvmf_rdma_request_process(&rtransport, req2);
		CU_ASSERT(req2->state == RDMA_REQUEST_STATE_EXECUTING);
		/* WRITE 1: EXECUTED -> COMPLETING */
		req2->state = RDMA_REQUEST_STATE_EXECUTED;
		nvmf_rdma_request_process(&rtransport, req2);
		CU_ASSERT(req2->state == RDMA_REQUEST_STATE_COMPLETING);
		STAILQ_INIT(&poller.qpairs_pending_send);
		/* WRITE 1: COMPLETED -> FREE */
		req2->state = RDMA_REQUEST_STATE_COMPLETED;
		nvmf_rdma_request_process(&rtransport, req2);
		CU_ASSERT(req2->state == RDMA_REQUEST_STATE_FREE);

		free_recv(recv1);
		free_req(req1);
		free_recv(recv2);
		free_req(req2);
		poller_reset(&poller, &group);
		qpair_reset(&rqpair, &poller, &device, &resources, &rtransport.transport);
	}

	/* Test 4, invalid command, check xfer type */
	{
		struct spdk_nvmf_rdma_recv *rdma_recv_inv;
		struct spdk_nvmf_rdma_request *rdma_req_inv;
		/* construct an opcode that specifies BIDIRECTIONAL transfer */
		uint8_t opc = 0x10 | SPDK_NVME_DATA_BIDIRECTIONAL;

		rdma_recv_inv = create_recv(&rqpair, opc);
		rdma_req_inv = create_req(&rqpair, rdma_recv_inv);

		/* NEW -> RDMA_REQUEST_STATE_COMPLETING */
		rqpair.current_recv_depth = 1;
		progress = nvmf_rdma_request_process(&rtransport, rdma_req_inv);
		CU_ASSERT(progress == true);
		CU_ASSERT(rdma_req_inv->state == RDMA_REQUEST_STATE_COMPLETING);
		CU_ASSERT(rdma_req_inv->req.rsp->nvme_cpl.status.sct == SPDK_NVME_SCT_GENERIC);
		CU_ASSERT(rdma_req_inv->req.rsp->nvme_cpl.status.sc == SPDK_NVME_SC_INVALID_OPCODE);

		/* RDMA_REQUEST_STATE_COMPLETED -> FREE */
		rdma_req_inv->state = RDMA_REQUEST_STATE_COMPLETED;
		nvmf_rdma_request_process(&rtransport, rdma_req_inv);
		CU_ASSERT(rdma_req_inv->state == RDMA_REQUEST_STATE_FREE);

		free_recv(rdma_recv_inv);
		free_req(rdma_req_inv);
		poller_reset(&poller, &group);
		qpair_reset(&rqpair, &poller, &device, &resources, &rtransport.transport);
	}

	spdk_mempool_free(rtransport.transport.data_buf_pool);
	spdk_mempool_free(rtransport.data_wr_pool);
}

#define TEST_GROUPS_COUNT 5
static void
test_nvmf_rdma_get_optimal_poll_group(void)
{
	struct spdk_nvmf_rdma_transport rtransport = {};
	struct spdk_nvmf_transport *transport = &rtransport.transport;
	struct spdk_nvmf_rdma_qpair rqpair = {};
	struct spdk_nvmf_transport_poll_group *groups[TEST_GROUPS_COUNT];
	struct spdk_nvmf_rdma_poll_group *rgroups[TEST_GROUPS_COUNT];
	struct spdk_nvmf_transport_poll_group *result;
	struct spdk_nvmf_poll_group group = {};
	uint32_t i;

	rqpair.qpair.transport = transport;
	TAILQ_INIT(&rtransport.poll_groups);

	for (i = 0; i < TEST_GROUPS_COUNT; i++) {
		groups[i] = nvmf_rdma_poll_group_create(transport, NULL);
		CU_ASSERT(groups[i] != NULL);
		groups[i]->group = &group;
		rgroups[i] = SPDK_CONTAINEROF(groups[i], struct spdk_nvmf_rdma_poll_group, group);
		groups[i]->transport = transport;
	}
	CU_ASSERT(rtransport.conn_sched.next_admin_pg == rgroups[0]);
	CU_ASSERT(rtransport.conn_sched.next_io_pg == rgroups[0]);

	/* Emulate connection of %TEST_GROUPS_COUNT% initiators - each creates 1 admin and 1 io qp */
	for (i = 0; i < TEST_GROUPS_COUNT; i++) {
		rqpair.qpair.qid = 0;
		result = nvmf_rdma_get_optimal_poll_group(&rqpair.qpair);
		CU_ASSERT(result == groups[i]);
		CU_ASSERT(rtransport.conn_sched.next_admin_pg == rgroups[(i + 1) % TEST_GROUPS_COUNT]);
		CU_ASSERT(rtransport.conn_sched.next_io_pg == rgroups[i]);

		rqpair.qpair.qid = 1;
		result = nvmf_rdma_get_optimal_poll_group(&rqpair.qpair);
		CU_ASSERT(result == groups[i]);
		CU_ASSERT(rtransport.conn_sched.next_admin_pg == rgroups[(i + 1) % TEST_GROUPS_COUNT]);
		CU_ASSERT(rtransport.conn_sched.next_io_pg == rgroups[(i + 1) % TEST_GROUPS_COUNT]);
	}
	/* wrap around, admin/io pg point to the first pg
	   Destroy all poll groups except of the last one */
	for (i = 0; i < TEST_GROUPS_COUNT - 1; i++) {
		nvmf_rdma_poll_group_destroy(groups[i]);
		CU_ASSERT(rtransport.conn_sched.next_admin_pg == rgroups[i + 1]);
		CU_ASSERT(rtransport.conn_sched.next_io_pg == rgroups[i + 1]);
	}

	CU_ASSERT(rtransport.conn_sched.next_admin_pg == rgroups[TEST_GROUPS_COUNT - 1]);
	CU_ASSERT(rtransport.conn_sched.next_io_pg == rgroups[TEST_GROUPS_COUNT - 1]);

	/* Check that pointers to the next admin/io poll groups are not changed */
	rqpair.qpair.qid = 0;
	result = nvmf_rdma_get_optimal_poll_group(&rqpair.qpair);
	CU_ASSERT(result == groups[TEST_GROUPS_COUNT - 1]);
	CU_ASSERT(rtransport.conn_sched.next_admin_pg == rgroups[TEST_GROUPS_COUNT - 1]);
	CU_ASSERT(rtransport.conn_sched.next_io_pg == rgroups[TEST_GROUPS_COUNT - 1]);

	rqpair.qpair.qid = 1;
	result = nvmf_rdma_get_optimal_poll_group(&rqpair.qpair);
	CU_ASSERT(result == groups[TEST_GROUPS_COUNT - 1]);
	CU_ASSERT(rtransport.conn_sched.next_admin_pg == rgroups[TEST_GROUPS_COUNT - 1]);
	CU_ASSERT(rtransport.conn_sched.next_io_pg == rgroups[TEST_GROUPS_COUNT - 1]);

	/* Remove the last poll group, check that pointers are NULL */
	nvmf_rdma_poll_group_destroy(groups[TEST_GROUPS_COUNT - 1]);
	CU_ASSERT(rtransport.conn_sched.next_admin_pg == NULL);
	CU_ASSERT(rtransport.conn_sched.next_io_pg == NULL);

	/* Request optimal poll group, result must be NULL */
	rqpair.qpair.qid = 0;
	result = nvmf_rdma_get_optimal_poll_group(&rqpair.qpair);
	CU_ASSERT(result == NULL);

	rqpair.qpair.qid = 1;
	result = nvmf_rdma_get_optimal_poll_group(&rqpair.qpair);
	CU_ASSERT(result == NULL);
}
#undef TEST_GROUPS_COUNT

static void
test_spdk_nvmf_rdma_request_parse_sgl_with_md(void)
{
	struct spdk_nvmf_rdma_transport rtransport;
	struct spdk_nvmf_rdma_device device;
	struct spdk_nvmf_rdma_request rdma_req = {};
	struct spdk_nvmf_rdma_recv recv;
	struct spdk_nvmf_rdma_poll_group group;
	struct spdk_nvmf_rdma_qpair rqpair;
	struct spdk_nvmf_rdma_poller poller;
	union nvmf_c2h_msg cpl;
	union nvmf_h2c_msg cmd;
	struct spdk_nvme_sgl_descriptor *sgl;
	struct spdk_nvme_sgl_descriptor sgl_desc[SPDK_NVMF_MAX_SGL_ENTRIES] = {{0}};
	char data_buffer[8192];
	struct spdk_nvmf_rdma_request_data *data = (struct spdk_nvmf_rdma_request_data *)data_buffer;
	char data2_buffer[8192];
	struct spdk_nvmf_rdma_request_data *data2 = (struct spdk_nvmf_rdma_request_data *)data2_buffer;
	const uint32_t data_bs = 512;
	const uint32_t md_size = 8;
	int rc, i;
	void *aligned_buffer;

	data->wr.sg_list = data->sgl;
	STAILQ_INIT(&group.group.buf_cache);
	group.group.buf_cache_size = 0;
	group.group.buf_cache_count = 0;
	group.group.transport = &rtransport.transport;
	poller.group = &group;
	rqpair.poller = &poller;
	rqpair.max_send_sge = SPDK_NVMF_MAX_SGL_ENTRIES;

	sgl = &cmd.nvme_cmd.dptr.sgl1;
	rdma_req.recv = &recv;
	rdma_req.req.cmd = &cmd;
	rdma_req.req.rsp = &cpl;
	rdma_req.data.wr.sg_list = rdma_req.data.sgl;
	rdma_req.req.qpair = &rqpair.qpair;
	rdma_req.req.xfer = SPDK_NVME_DATA_CONTROLLER_TO_HOST;

	rtransport.transport.opts = g_rdma_ut_transport_opts;
	rtransport.data_wr_pool = NULL;
	rtransport.transport.data_buf_pool = NULL;

	device.attr.device_cap_flags = 0;
	device.map = NULL;
	sgl->keyed.key = 0xEEEE;
	sgl->address = 0xFFFF;
	rdma_req.recv->buf = (void *)0xDDDD;

	/* Test 1: sgl type: keyed data block subtype: address */
	sgl->generic.type = SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK;
	sgl->keyed.subtype = SPDK_NVME_SGL_SUBTYPE_ADDRESS;

	/* Part 1: simple I/O, one SGL smaller than the transport io unit size, block size 512 */
	MOCK_SET(spdk_mempool_get, (void *)0x2000);
	reset_nvmf_rdma_request(&rdma_req);
	spdk_dif_ctx_init(&rdma_req.req.dif.dif_ctx, data_bs + md_size, md_size, true, false,
			  SPDK_DIF_TYPE1, SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_REFTAG_CHECK,
			  0, 0, 0, 0, 0);
	rdma_req.req.dif_enabled = true;
	rtransport.transport.opts.io_unit_size = data_bs * 8;
	rdma_req.req.qpair->transport = &rtransport.transport;
	sgl->keyed.length = data_bs * 4;

	rc = nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == 0);
	CU_ASSERT(rdma_req.req.data_from_pool == true);
	CU_ASSERT(rdma_req.req.length == data_bs * 4);
	CU_ASSERT(rdma_req.req.dif.orig_length == rdma_req.req.length);
	CU_ASSERT(rdma_req.req.dif.elba_length == (data_bs + md_size) * 4);
	CU_ASSERT((uint64_t)rdma_req.req.data == 0x2000);
	CU_ASSERT(rdma_req.data.wr.num_sge == 1);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.rkey == 0xEEEE);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.remote_addr == 0xFFFF);
	CU_ASSERT((uint64_t)rdma_req.req.buffers[0] == 0x2000);

	CU_ASSERT(rdma_req.data.wr.sg_list[0].addr == 0x2000);
	CU_ASSERT(rdma_req.data.wr.sg_list[0].length == rdma_req.req.length);
	CU_ASSERT(rdma_req.data.wr.sg_list[0].lkey == RDMA_UT_LKEY);

	/* Part 2: simple I/O, one SGL equal to io unit size, io_unit_size is not aligned with md_size,
		block size 512 */
	MOCK_SET(spdk_mempool_get, (void *)0x2000);
	reset_nvmf_rdma_request(&rdma_req);
	spdk_dif_ctx_init(&rdma_req.req.dif.dif_ctx, data_bs + md_size, md_size, true, false,
			  SPDK_DIF_TYPE1, SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_REFTAG_CHECK,
			  0, 0, 0, 0, 0);
	rdma_req.req.dif_enabled = true;
	rtransport.transport.opts.io_unit_size = data_bs * 4;
	sgl->keyed.length = data_bs * 4;

	rc = nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == 0);
	CU_ASSERT(rdma_req.req.data_from_pool == true);
	CU_ASSERT(rdma_req.req.length == data_bs * 4);
	CU_ASSERT(rdma_req.req.dif.orig_length == rdma_req.req.length);
	CU_ASSERT(rdma_req.req.dif.elba_length == (data_bs + md_size) * 4);
	CU_ASSERT((uint64_t)rdma_req.req.data == 0x2000);
	CU_ASSERT(rdma_req.data.wr.num_sge == 5);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.rkey == 0xEEEE);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.remote_addr == 0xFFFF);
	CU_ASSERT((uint64_t)rdma_req.req.buffers[0] == 0x2000);

	for (i = 0; i < 3; ++i) {
		CU_ASSERT(rdma_req.data.wr.sg_list[i].addr == 0x2000 + i * (data_bs + md_size));
		CU_ASSERT(rdma_req.data.wr.sg_list[i].length == data_bs);
		CU_ASSERT(rdma_req.data.wr.sg_list[i].lkey == RDMA_UT_LKEY);
	}
	CU_ASSERT(rdma_req.data.wr.sg_list[3].addr == 0x2000 + 3 * (data_bs + md_size));
	CU_ASSERT(rdma_req.data.wr.sg_list[3].length == 488);
	CU_ASSERT(rdma_req.data.wr.sg_list[3].lkey == RDMA_UT_LKEY);

	/* 2nd buffer consumed */
	CU_ASSERT(rdma_req.data.wr.sg_list[4].addr == 0x2000);
	CU_ASSERT(rdma_req.data.wr.sg_list[4].length == 24);
	CU_ASSERT(rdma_req.data.wr.sg_list[4].lkey == RDMA_UT_LKEY);

	/* Part 3: simple I/O, one SGL equal io unit size, io_unit_size is equal to block size 512 bytes */
	MOCK_SET(spdk_mempool_get, (void *)0x2000);
	reset_nvmf_rdma_request(&rdma_req);
	spdk_dif_ctx_init(&rdma_req.req.dif.dif_ctx, data_bs + md_size, md_size, true, false,
			  SPDK_DIF_TYPE1, SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_REFTAG_CHECK,
			  0, 0, 0, 0, 0);
	rdma_req.req.dif_enabled = true;
	rtransport.transport.opts.io_unit_size = data_bs;
	sgl->keyed.length = data_bs;

	rc = nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == 0);
	CU_ASSERT(rdma_req.req.data_from_pool == true);
	CU_ASSERT(rdma_req.req.length == data_bs);
	CU_ASSERT(rdma_req.req.dif.orig_length == rdma_req.req.length);
	CU_ASSERT(rdma_req.req.dif.elba_length == data_bs + md_size);
	CU_ASSERT((uint64_t)rdma_req.req.data == 0x2000);
	CU_ASSERT(rdma_req.data.wr.num_sge == 1);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.rkey == 0xEEEE);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.remote_addr == 0xFFFF);
	CU_ASSERT((uint64_t)rdma_req.req.buffers[0] == 0x2000);

	CU_ASSERT(rdma_req.data.wr.sg_list[0].addr == 0x2000);
	CU_ASSERT(rdma_req.data.wr.sg_list[0].length == data_bs);
	CU_ASSERT(rdma_req.data.wr.sg_list[0].lkey == RDMA_UT_LKEY);

	CU_ASSERT(rdma_req.req.iovcnt == 2);
	CU_ASSERT(rdma_req.req.iov[0].iov_base == (void *)((unsigned long)0x2000));
	CU_ASSERT(rdma_req.req.iov[0].iov_len == data_bs);
	/* 2nd buffer consumed for metadata */
	CU_ASSERT(rdma_req.req.iov[1].iov_base == (void *)((unsigned long)0x2000));
	CU_ASSERT(rdma_req.req.iov[1].iov_len == md_size);

	/* Part 4: simple I/O, one SGL equal io unit size, io_unit_size is aligned with md_size,
	   block size 512 */
	MOCK_SET(spdk_mempool_get, (void *)0x2000);
	reset_nvmf_rdma_request(&rdma_req);
	spdk_dif_ctx_init(&rdma_req.req.dif.dif_ctx, data_bs + md_size, md_size, true, false,
			  SPDK_DIF_TYPE1, SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_REFTAG_CHECK,
			  0, 0, 0, 0, 0);
	rdma_req.req.dif_enabled = true;
	rtransport.transport.opts.io_unit_size = (data_bs + md_size) * 4;
	sgl->keyed.length = data_bs * 4;

	rc = nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == 0);
	CU_ASSERT(rdma_req.req.data_from_pool == true);
	CU_ASSERT(rdma_req.req.length == data_bs * 4);
	CU_ASSERT(rdma_req.req.dif.orig_length == rdma_req.req.length);
	CU_ASSERT(rdma_req.req.dif.elba_length == (data_bs + md_size) * 4);
	CU_ASSERT((uint64_t)rdma_req.req.data == 0x2000);
	CU_ASSERT(rdma_req.data.wr.num_sge == 1);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.rkey == 0xEEEE);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.remote_addr == 0xFFFF);
	CU_ASSERT((uint64_t)rdma_req.req.buffers[0] == 0x2000);

	CU_ASSERT(rdma_req.data.wr.sg_list[0].addr == 0x2000);
	CU_ASSERT(rdma_req.data.wr.sg_list[0].length == rdma_req.req.length);
	CU_ASSERT(rdma_req.data.wr.sg_list[0].lkey == RDMA_UT_LKEY);

	/* Part 5: simple I/O, one SGL equal to 2x io unit size, io_unit_size is aligned with md_size,
	   block size 512 */
	MOCK_SET(spdk_mempool_get, (void *)0x2000);
	reset_nvmf_rdma_request(&rdma_req);
	spdk_dif_ctx_init(&rdma_req.req.dif.dif_ctx, data_bs + md_size, md_size, true, false,
			  SPDK_DIF_TYPE1, SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_REFTAG_CHECK,
			  0, 0, 0, 0, 0);
	rdma_req.req.dif_enabled = true;
	rtransport.transport.opts.io_unit_size = (data_bs + md_size) * 2;
	sgl->keyed.length = data_bs * 4;

	rc = nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == 0);
	CU_ASSERT(rdma_req.req.data_from_pool == true);
	CU_ASSERT(rdma_req.req.length == data_bs * 4);
	CU_ASSERT(rdma_req.req.dif.orig_length == rdma_req.req.length);
	CU_ASSERT(rdma_req.req.dif.elba_length == (data_bs + md_size) * 4);
	CU_ASSERT((uint64_t)rdma_req.req.data == 0x2000);
	CU_ASSERT(rdma_req.data.wr.num_sge == 2);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.rkey == 0xEEEE);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.remote_addr == 0xFFFF);
	CU_ASSERT((uint64_t)rdma_req.req.buffers[0] == 0x2000);

	for (i = 0; i < 2; ++i) {
		CU_ASSERT(rdma_req.data.wr.sg_list[i].addr == 0x2000);
		CU_ASSERT(rdma_req.data.wr.sg_list[i].length == data_bs * 2);
	}

	/* Part 6: simple I/O, one SGL larger than the transport io unit size, io_unit_size is not aligned to md_size,
	   block size 512 */
	MOCK_SET(spdk_mempool_get, (void *)0x2000);
	reset_nvmf_rdma_request(&rdma_req);
	spdk_dif_ctx_init(&rdma_req.req.dif.dif_ctx, data_bs + md_size, md_size, true, false,
			  SPDK_DIF_TYPE1, SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_REFTAG_CHECK,
			  0, 0, 0, 0, 0);
	rdma_req.req.dif_enabled = true;
	rtransport.transport.opts.io_unit_size = data_bs * 4;
	sgl->keyed.length = data_bs * 6;

	rc = nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == 0);
	CU_ASSERT(rdma_req.req.data_from_pool == true);
	CU_ASSERT(rdma_req.req.length == data_bs * 6);
	CU_ASSERT(rdma_req.req.dif.orig_length == rdma_req.req.length);
	CU_ASSERT(rdma_req.req.dif.elba_length == (data_bs + md_size) * 6);
	CU_ASSERT((uint64_t)rdma_req.req.data == 0x2000);
	CU_ASSERT(rdma_req.data.wr.num_sge == 7);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.rkey == 0xEEEE);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.remote_addr == 0xFFFF);
	CU_ASSERT((uint64_t)rdma_req.req.buffers[0] == 0x2000);

	for (i = 0; i < 3; ++i) {
		CU_ASSERT(rdma_req.data.wr.sg_list[i].addr == 0x2000 + i * (data_bs + md_size));
		CU_ASSERT(rdma_req.data.wr.sg_list[i].length == data_bs);
		CU_ASSERT(rdma_req.data.wr.sg_list[i].lkey == RDMA_UT_LKEY);
	}
	CU_ASSERT(rdma_req.data.wr.sg_list[3].addr == 0x2000 + 3 * (data_bs + md_size));
	CU_ASSERT(rdma_req.data.wr.sg_list[3].length == 488);
	CU_ASSERT(rdma_req.data.wr.sg_list[3].lkey == RDMA_UT_LKEY);

	/* 2nd IO buffer consumed */
	CU_ASSERT(rdma_req.data.wr.sg_list[4].addr == 0x2000);
	CU_ASSERT(rdma_req.data.wr.sg_list[4].length == 24);
	CU_ASSERT(rdma_req.data.wr.sg_list[4].lkey == RDMA_UT_LKEY);

	CU_ASSERT(rdma_req.data.wr.sg_list[5].addr == 0x2000 + 24 + md_size);
	CU_ASSERT(rdma_req.data.wr.sg_list[5].length == 512);
	CU_ASSERT(rdma_req.data.wr.sg_list[5].lkey == RDMA_UT_LKEY);

	CU_ASSERT(rdma_req.data.wr.sg_list[6].addr == 0x2000 + 24 + 512 + md_size * 2);
	CU_ASSERT(rdma_req.data.wr.sg_list[6].length == 512);
	CU_ASSERT(rdma_req.data.wr.sg_list[6].lkey == RDMA_UT_LKEY);

	/* Part 7: simple I/O, number of SGL entries exceeds the number of entries
	   one WR can hold. Additional WR is chained */
	MOCK_SET(spdk_mempool_get, data2_buffer);
	aligned_buffer = (void *)((uintptr_t)(data2_buffer + NVMF_DATA_BUFFER_MASK) &
				  ~NVMF_DATA_BUFFER_MASK);
	reset_nvmf_rdma_request(&rdma_req);
	spdk_dif_ctx_init(&rdma_req.req.dif.dif_ctx, data_bs + md_size, md_size, true, false,
			  SPDK_DIF_TYPE1, SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_REFTAG_CHECK,
			  0, 0, 0, 0, 0);
	rdma_req.req.dif_enabled = true;
	rtransport.transport.opts.io_unit_size = data_bs * 16;
	sgl->keyed.length = data_bs * 16;

	rc = nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == 0);
	CU_ASSERT(rdma_req.req.data_from_pool == true);
	CU_ASSERT(rdma_req.req.length == data_bs * 16);
	CU_ASSERT(rdma_req.req.iovcnt == 2);
	CU_ASSERT(rdma_req.req.dif.orig_length == rdma_req.req.length);
	CU_ASSERT(rdma_req.req.dif.elba_length == (data_bs + md_size) * 16);
	CU_ASSERT(rdma_req.req.data == aligned_buffer);
	CU_ASSERT(rdma_req.data.wr.num_sge == 16);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.rkey == 0xEEEE);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.remote_addr == 0xFFFF);

	for (i = 0; i < 15; ++i) {
		CU_ASSERT(rdma_req.data.wr.sg_list[i].addr == (uintptr_t)aligned_buffer + i * (data_bs + md_size));
		CU_ASSERT(rdma_req.data.wr.sg_list[i].length == data_bs);
		CU_ASSERT(rdma_req.data.wr.sg_list[i].lkey == RDMA_UT_LKEY);
	}

	/* 8192 - (512 + 8) * 15 = 392 */
	CU_ASSERT(rdma_req.data.wr.sg_list[i].addr == (uintptr_t)aligned_buffer + i * (data_bs + md_size));
	CU_ASSERT(rdma_req.data.wr.sg_list[i].length == 392);
	CU_ASSERT(rdma_req.data.wr.sg_list[i].lkey == RDMA_UT_LKEY);

	/* additional wr from pool */
	CU_ASSERT(rdma_req.data.wr.next == (void *)&data2->wr);
	CU_ASSERT(rdma_req.data.wr.next->num_sge == 1);
	CU_ASSERT(rdma_req.data.wr.next->next == &rdma_req.rsp.wr);
	/* 2nd IO buffer */
	CU_ASSERT(data2->wr.sg_list[0].addr == (uintptr_t)aligned_buffer);
	CU_ASSERT(data2->wr.sg_list[0].length == 120);
	CU_ASSERT(data2->wr.sg_list[0].lkey == RDMA_UT_LKEY);

	/* Part 8: simple I/O, data with metadata do not fit to 1 io_buffer */
	MOCK_SET(spdk_mempool_get, (void *)0x2000);
	reset_nvmf_rdma_request(&rdma_req);
	spdk_dif_ctx_init(&rdma_req.req.dif.dif_ctx, data_bs + md_size, md_size, true, false,
			  SPDK_DIF_TYPE1, SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_REFTAG_CHECK,
			  0, 0, 0, 0, 0);
	rdma_req.req.dif_enabled = true;
	rtransport.transport.opts.io_unit_size = 516;
	sgl->keyed.length = data_bs * 2;

	rc = nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == 0);
	CU_ASSERT(rdma_req.req.data_from_pool == true);
	CU_ASSERT(rdma_req.req.length == data_bs * 2);
	CU_ASSERT(rdma_req.req.iovcnt == 3);
	CU_ASSERT(rdma_req.req.dif.orig_length == rdma_req.req.length);
	CU_ASSERT(rdma_req.req.dif.elba_length == (data_bs + md_size) * 2);
	CU_ASSERT(rdma_req.req.data == (void *)0x2000);
	CU_ASSERT(rdma_req.data.wr.num_sge == 2);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.rkey == 0xEEEE);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.remote_addr == 0xFFFF);

	CU_ASSERT(rdma_req.data.wr.sg_list[0].addr == 0x2000);
	CU_ASSERT(rdma_req.data.wr.sg_list[0].length == 512);
	CU_ASSERT(rdma_req.data.wr.sg_list[0].lkey == RDMA_UT_LKEY);

	/* 2nd IO buffer consumed, offset 4 bytes due to part of the metadata
	  is located at the beginning of that buffer */
	CU_ASSERT(rdma_req.data.wr.sg_list[1].addr == 0x2000 + 4);
	CU_ASSERT(rdma_req.data.wr.sg_list[1].length == 512);
	CU_ASSERT(rdma_req.data.wr.sg_list[1].lkey == RDMA_UT_LKEY);

	/* Test 2: Multi SGL */
	sgl->generic.type = SPDK_NVME_SGL_TYPE_LAST_SEGMENT;
	sgl->unkeyed.subtype = SPDK_NVME_SGL_SUBTYPE_OFFSET;
	sgl->address = 0;
	rdma_req.recv->buf = (void *)&sgl_desc;
	MOCK_SET(spdk_mempool_get, data_buffer);
	aligned_buffer = (void *)((uintptr_t)(data_buffer + NVMF_DATA_BUFFER_MASK) &
				  ~NVMF_DATA_BUFFER_MASK);

	/* part 1: 2 segments each with 1 wr. io_unit_size is aligned with data_bs + md_size */
	reset_nvmf_rdma_request(&rdma_req);
	spdk_dif_ctx_init(&rdma_req.req.dif.dif_ctx, data_bs + md_size, md_size, true, false,
			  SPDK_DIF_TYPE1,
			  SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_REFTAG_CHECK, 0, 0, 0, 0, 0);
	rdma_req.req.dif_enabled = true;
	rtransport.transport.opts.io_unit_size = (data_bs + md_size) * 4;
	sgl->unkeyed.length = 2 * sizeof(struct spdk_nvme_sgl_descriptor);

	for (i = 0; i < 2; i++) {
		sgl_desc[i].keyed.type = SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK;
		sgl_desc[i].keyed.subtype = SPDK_NVME_SGL_SUBTYPE_ADDRESS;
		sgl_desc[i].keyed.length = data_bs * 4;
		sgl_desc[i].address = 0x4000 + i * data_bs * 4;
		sgl_desc[i].keyed.key = 0x44;
	}

	rc = nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == 0);
	CU_ASSERT(rdma_req.req.data_from_pool == true);
	CU_ASSERT(rdma_req.req.length == data_bs * 4 * 2);
	CU_ASSERT(rdma_req.req.dif.orig_length == rdma_req.req.length);
	CU_ASSERT(rdma_req.req.dif.elba_length == (data_bs + md_size) * 4 * 2);
	CU_ASSERT(rdma_req.data.wr.num_sge == 1);
	CU_ASSERT(rdma_req.data.wr.sg_list[0].addr == (uintptr_t)(aligned_buffer));
	CU_ASSERT(rdma_req.data.wr.sg_list[0].length == data_bs * 4);

	CU_ASSERT(rdma_req.data.wr.wr.rdma.rkey == 0x44);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.remote_addr == 0x4000);
	CU_ASSERT(rdma_req.data.wr.next == &data->wr);
	CU_ASSERT(data->wr.wr.rdma.rkey == 0x44);
	CU_ASSERT(data->wr.wr.rdma.remote_addr == 0x4000 + data_bs * 4);
	CU_ASSERT(data->wr.num_sge == 1);
	CU_ASSERT(data->wr.sg_list[0].addr == (uintptr_t)(aligned_buffer));
	CU_ASSERT(data->wr.sg_list[0].length == data_bs * 4);

	CU_ASSERT(data->wr.next == &rdma_req.rsp.wr);
	reset_nvmf_rdma_request(&rdma_req);
}

static void
test_nvmf_rdma_opts_init(void)
{
	struct spdk_nvmf_transport_opts	opts = {};

	nvmf_rdma_opts_init(&opts);
	CU_ASSERT(opts.max_queue_depth == SPDK_NVMF_RDMA_DEFAULT_MAX_QUEUE_DEPTH);
	CU_ASSERT(opts.max_qpairs_per_ctrlr ==	SPDK_NVMF_RDMA_DEFAULT_MAX_QPAIRS_PER_CTRLR);
	CU_ASSERT(opts.in_capsule_data_size ==	SPDK_NVMF_RDMA_DEFAULT_IN_CAPSULE_DATA_SIZE);
	CU_ASSERT(opts.max_io_size == SPDK_NVMF_RDMA_DEFAULT_MAX_IO_SIZE);
	CU_ASSERT(opts.io_unit_size == SPDK_NVMF_RDMA_MIN_IO_BUFFER_SIZE);
	CU_ASSERT(opts.max_aq_depth == SPDK_NVMF_RDMA_DEFAULT_AQ_DEPTH);
	CU_ASSERT(opts.num_shared_buffers == SPDK_NVMF_RDMA_DEFAULT_NUM_SHARED_BUFFERS);
	CU_ASSERT(opts.buf_cache_size == SPDK_NVMF_RDMA_DEFAULT_BUFFER_CACHE_SIZE);
	CU_ASSERT(opts.dif_insert_or_strip == SPDK_NVMF_RDMA_DIF_INSERT_OR_STRIP);
	CU_ASSERT(opts.abort_timeout_sec == SPDK_NVMF_RDMA_DEFAULT_ABORT_TIMEOUT_SEC);
	CU_ASSERT(opts.transport_specific == NULL);
}

static void
test_nvmf_rdma_request_free_data(void)
{
	struct spdk_nvmf_rdma_request rdma_req = {};
	struct spdk_nvmf_rdma_transport rtransport = {};
	struct spdk_nvmf_rdma_request_data *next_request_data = NULL;

	MOCK_CLEAR(spdk_mempool_get);
	rtransport.data_wr_pool = spdk_mempool_create("spdk_nvmf_rdma_wr_data",
				  SPDK_NVMF_MAX_SGL_ENTRIES,
				  sizeof(struct spdk_nvmf_rdma_request_data),
				  SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
				  SPDK_ENV_SOCKET_ID_ANY);
	next_request_data = spdk_mempool_get(rtransport.data_wr_pool);
	SPDK_CU_ASSERT_FATAL(((struct test_mempool *)rtransport.data_wr_pool)->count ==
			     SPDK_NVMF_MAX_SGL_ENTRIES - 1);
	next_request_data->wr.wr_id = 1;
	next_request_data->wr.num_sge = 2;
	next_request_data->wr.next = NULL;
	rdma_req.data.wr.next = &next_request_data->wr;
	rdma_req.data.wr.wr_id = 1;
	rdma_req.data.wr.num_sge = 2;

	nvmf_rdma_request_free_data(&rdma_req, &rtransport);
	/* Check if next_request_data put into memory pool */
	CU_ASSERT(((struct test_mempool *)rtransport.data_wr_pool)->count == SPDK_NVMF_MAX_SGL_ENTRIES);
	CU_ASSERT(rdma_req.data.wr.num_sge == 0);

	spdk_mempool_free(rtransport.data_wr_pool);
}

static void
test_nvmf_rdma_update_ibv_state(void)
{
	struct spdk_nvmf_rdma_qpair rqpair = {};
	struct spdk_rdma_qp rdma_qp = {};
	struct ibv_qp qp = {};
	int rc = 0;

	rqpair.rdma_qp = &rdma_qp;

	/* Case 1: Failed to get updated RDMA queue pair state */
	rqpair.ibv_state = IBV_QPS_INIT;
	rqpair.rdma_qp->qp = NULL;

	rc = nvmf_rdma_update_ibv_state(&rqpair);
	CU_ASSERT(rc == IBV_QPS_ERR + 1);

	/* Case 2: Bad state updated */
	rqpair.rdma_qp->qp = &qp;
	qp.state = IBV_QPS_ERR;
	rc = nvmf_rdma_update_ibv_state(&rqpair);
	CU_ASSERT(rqpair.ibv_state == 10);
	CU_ASSERT(rc == IBV_QPS_ERR + 1);

	/* Case 3: Pass */
	qp.state = IBV_QPS_INIT;
	rc = nvmf_rdma_update_ibv_state(&rqpair);
	CU_ASSERT(rqpair.ibv_state == IBV_QPS_INIT);
	CU_ASSERT(rc == IBV_QPS_INIT);
}

static void
test_nvmf_rdma_resources_create(void)
{
	static struct spdk_nvmf_rdma_resources *rdma_resource;
	struct spdk_nvmf_rdma_resource_opts opts = {};
	struct spdk_nvmf_rdma_qpair qpair = {};
	struct spdk_nvmf_rdma_recv *recv = NULL;
	struct spdk_nvmf_rdma_request *req = NULL;
	const int DEPTH = 128;

	opts.max_queue_depth = DEPTH;
	opts.in_capsule_data_size = 4096;
	opts.shared = true;
	opts.qpair = &qpair;

	rdma_resource = nvmf_rdma_resources_create(&opts);
	CU_ASSERT(rdma_resource != NULL);
	/* Just check first and last entry */
	recv = &rdma_resource->recvs[0];
	req = &rdma_resource->reqs[0];
	CU_ASSERT(recv->rdma_wr.type == RDMA_WR_TYPE_RECV);
	CU_ASSERT((uintptr_t)recv->buf == (uintptr_t)(rdma_resource->bufs));
	CU_ASSERT(recv->sgl[0].addr == (uintptr_t)&rdma_resource->cmds[0]);
	CU_ASSERT(recv->sgl[0].length == sizeof(rdma_resource->cmds[0]));
	CU_ASSERT(recv->sgl[0].lkey == RDMA_UT_LKEY);
	CU_ASSERT(recv->wr.num_sge == 2);
	CU_ASSERT(recv->wr.wr_id == (uintptr_t)&rdma_resource->recvs[0].rdma_wr);
	CU_ASSERT(recv->wr.sg_list == rdma_resource->recvs[0].sgl);
	CU_ASSERT(req->req.rsp == &rdma_resource->cpls[0]);
	CU_ASSERT(req->rsp.sgl[0].addr == (uintptr_t)&rdma_resource->cpls[0]);
	CU_ASSERT(req->rsp.sgl[0].length == sizeof(rdma_resource->cpls[0]));
	CU_ASSERT(req->rsp.sgl[0].lkey == RDMA_UT_LKEY);
	CU_ASSERT(req->rsp.rdma_wr.type == RDMA_WR_TYPE_SEND);
	CU_ASSERT(req->rsp.wr.wr_id == (uintptr_t)&rdma_resource->reqs[0].rsp.rdma_wr);
	CU_ASSERT(req->rsp.wr.next == NULL);
	CU_ASSERT(req->rsp.wr.opcode == IBV_WR_SEND);
	CU_ASSERT(req->rsp.wr.send_flags == IBV_SEND_SIGNALED);
	CU_ASSERT(req->rsp.wr.sg_list == rdma_resource->reqs[0].rsp.sgl);
	CU_ASSERT(req->rsp.wr.num_sge == NVMF_DEFAULT_RSP_SGE);
	CU_ASSERT(req->data.rdma_wr.type == RDMA_WR_TYPE_DATA);
	CU_ASSERT(req->data.wr.wr_id == (uintptr_t)&rdma_resource->reqs[0].data.rdma_wr);
	CU_ASSERT(req->data.wr.next == NULL);
	CU_ASSERT(req->data.wr.send_flags == IBV_SEND_SIGNALED);
	CU_ASSERT(req->data.wr.sg_list == rdma_resource->reqs[0].data.sgl);
	CU_ASSERT(req->data.wr.num_sge == SPDK_NVMF_MAX_SGL_ENTRIES);
	CU_ASSERT(req->state == RDMA_REQUEST_STATE_FREE);

	recv = &rdma_resource->recvs[DEPTH - 1];
	req = &rdma_resource->reqs[DEPTH - 1];
	CU_ASSERT(recv->rdma_wr.type == RDMA_WR_TYPE_RECV);
	CU_ASSERT((uintptr_t)recv->buf == (uintptr_t)(rdma_resource->bufs +
			(DEPTH - 1) * 4096));
	CU_ASSERT(recv->sgl[0].addr == (uintptr_t)&rdma_resource->cmds[DEPTH - 1]);
	CU_ASSERT(recv->sgl[0].length == sizeof(rdma_resource->cmds[DEPTH - 1]));
	CU_ASSERT(recv->sgl[0].lkey == RDMA_UT_LKEY);
	CU_ASSERT(recv->wr.num_sge == 2);
	CU_ASSERT(recv->wr.wr_id == (uintptr_t)&rdma_resource->recvs[DEPTH - 1].rdma_wr);
	CU_ASSERT(recv->wr.sg_list == rdma_resource->recvs[DEPTH - 1].sgl);
	CU_ASSERT(req->req.rsp == &rdma_resource->cpls[DEPTH - 1]);
	CU_ASSERT(req->rsp.sgl[0].addr == (uintptr_t)&rdma_resource->cpls[DEPTH - 1]);
	CU_ASSERT(req->rsp.sgl[0].length == sizeof(rdma_resource->cpls[DEPTH - 1]));
	CU_ASSERT(req->rsp.sgl[0].lkey == RDMA_UT_LKEY);
	CU_ASSERT(req->rsp.rdma_wr.type == RDMA_WR_TYPE_SEND);
	CU_ASSERT(req->rsp.wr.wr_id == (uintptr_t)
		  &req->rsp.rdma_wr);
	CU_ASSERT(req->rsp.wr.next == NULL);
	CU_ASSERT(req->rsp.wr.opcode == IBV_WR_SEND);
	CU_ASSERT(req->rsp.wr.send_flags == IBV_SEND_SIGNALED);
	CU_ASSERT(req->rsp.wr.sg_list == rdma_resource->reqs[DEPTH - 1].rsp.sgl);
	CU_ASSERT(req->rsp.wr.num_sge == NVMF_DEFAULT_RSP_SGE);
	CU_ASSERT(req->data.rdma_wr.type == RDMA_WR_TYPE_DATA);
	CU_ASSERT(req->data.wr.wr_id == (uintptr_t)
		  &req->data.rdma_wr);
	CU_ASSERT(req->data.wr.next == NULL);
	CU_ASSERT(req->data.wr.send_flags == IBV_SEND_SIGNALED);
	CU_ASSERT(req->data.wr.sg_list == rdma_resource->reqs[DEPTH - 1].data.sgl);
	CU_ASSERT(req->data.wr.num_sge == SPDK_NVMF_MAX_SGL_ENTRIES);
	CU_ASSERT(req->state == RDMA_REQUEST_STATE_FREE);

	nvmf_rdma_resources_destroy(rdma_resource);
}

static void
test_nvmf_rdma_qpair_compare(void)
{
	struct spdk_nvmf_rdma_qpair rqpair1 = {}, rqpair2 = {};

	rqpair1.qp_num = 0;
	rqpair2.qp_num = UINT32_MAX;

	CU_ASSERT(nvmf_rdma_qpair_compare(&rqpair1, &rqpair2) < 0);
	CU_ASSERT(nvmf_rdma_qpair_compare(&rqpair2, &rqpair1) > 0);
}

static void
test_nvmf_rdma_resize_cq(void)
{
	int rc = -1;
	int tnum_wr = 0;
	int tnum_cqe = 0;
	struct spdk_nvmf_rdma_qpair rqpair = {};
	struct spdk_nvmf_rdma_poller rpoller = {};
	struct spdk_nvmf_rdma_device rdevice = {};
	struct ibv_context ircontext = {};
	struct ibv_device idevice = {};

	rdevice.context = &ircontext;
	rqpair.poller = &rpoller;
	ircontext.device = &idevice;

	/* Test1: Current capacity support required size. */
	rpoller.required_num_wr = 10;
	rpoller.num_cqe = 20;
	rqpair.max_queue_depth = 2;
	tnum_wr = rpoller.required_num_wr;
	tnum_cqe = rpoller.num_cqe;

	rc = nvmf_rdma_resize_cq(&rqpair, &rdevice);
	CU_ASSERT(rc == 0);
	CU_ASSERT(rpoller.required_num_wr == 10 + MAX_WR_PER_QP(rqpair.max_queue_depth));
	CU_ASSERT(rpoller.required_num_wr > tnum_wr);
	CU_ASSERT(rpoller.num_cqe == tnum_cqe);

	/* Test2: iWARP doesn't support CQ resize. */
	tnum_wr = rpoller.required_num_wr;
	tnum_cqe = rpoller.num_cqe;
	idevice.transport_type = IBV_TRANSPORT_IWARP;

	rc = nvmf_rdma_resize_cq(&rqpair, &rdevice);
	CU_ASSERT(rc == -1);
	CU_ASSERT(rpoller.required_num_wr == tnum_wr);
	CU_ASSERT(rpoller.num_cqe == tnum_cqe);


	/* Test3: RDMA CQE requirement exceeds device max_cqe limitation. */
	tnum_wr = rpoller.required_num_wr;
	tnum_cqe = rpoller.num_cqe;
	idevice.transport_type = IBV_TRANSPORT_UNKNOWN;
	rdevice.attr.max_cqe = 3;

	rc = nvmf_rdma_resize_cq(&rqpair, &rdevice);
	CU_ASSERT(rc == -1);
	CU_ASSERT(rpoller.required_num_wr == tnum_wr);
	CU_ASSERT(rpoller.num_cqe == tnum_cqe);

	/* Test4: RDMA CQ resize failed. */
	tnum_wr = rpoller.required_num_wr;
	tnum_cqe = rpoller.num_cqe;
	idevice.transport_type = IBV_TRANSPORT_IB;
	rdevice.attr.max_cqe = 30;
	MOCK_SET(ibv_resize_cq, -1);

	rc = nvmf_rdma_resize_cq(&rqpair, &rdevice);
	CU_ASSERT(rc == -1);
	CU_ASSERT(rpoller.required_num_wr == tnum_wr);
	CU_ASSERT(rpoller.num_cqe == tnum_cqe);

	/* Test5: RDMA CQ resize success. rsize = MIN(MAX(num_cqe * 2, required_num_wr), device->attr.max_cqe). */
	tnum_wr = rpoller.required_num_wr;
	tnum_cqe = rpoller.num_cqe;
	MOCK_SET(ibv_resize_cq, 0);

	rc = nvmf_rdma_resize_cq(&rqpair, &rdevice);
	CU_ASSERT(rc == 0);
	CU_ASSERT(rpoller.num_cqe = 30);
	CU_ASSERT(rpoller.required_num_wr == 18 + MAX_WR_PER_QP(rqpair.max_queue_depth));
	CU_ASSERT(rpoller.required_num_wr > tnum_wr);
	CU_ASSERT(rpoller.num_cqe > tnum_cqe);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("nvmf", NULL, NULL);

	CU_ADD_TEST(suite, test_spdk_nvmf_rdma_request_parse_sgl);
	CU_ADD_TEST(suite, test_spdk_nvmf_rdma_request_process);
	CU_ADD_TEST(suite, test_nvmf_rdma_get_optimal_poll_group);
	CU_ADD_TEST(suite, test_spdk_nvmf_rdma_request_parse_sgl_with_md);
	CU_ADD_TEST(suite, test_nvmf_rdma_opts_init);
	CU_ADD_TEST(suite, test_nvmf_rdma_request_free_data);
	CU_ADD_TEST(suite, test_nvmf_rdma_update_ibv_state);
	CU_ADD_TEST(suite, test_nvmf_rdma_resources_create);
	CU_ADD_TEST(suite, test_nvmf_rdma_qpair_compare);
	CU_ADD_TEST(suite, test_nvmf_rdma_resize_cq);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
