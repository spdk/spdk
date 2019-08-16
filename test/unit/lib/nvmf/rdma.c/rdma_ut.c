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
#include "nvmf/rdma.c"

uint64_t g_mr_size;
struct ibv_mr g_rdma_mr;

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

SPDK_LOG_REGISTER_COMPONENT("nvmf", SPDK_LOG_NVMF)
DEFINE_STUB(spdk_mem_map_set_translation, int, (struct spdk_mem_map *map, uint64_t vaddr,
		uint64_t size, uint64_t translation), 0);
DEFINE_STUB(spdk_mem_map_clear_translation, int, (struct spdk_mem_map *map, uint64_t vaddr,
		uint64_t size), 0);
DEFINE_STUB(spdk_mem_map_alloc, struct spdk_mem_map *, (uint64_t default_translation,
		const struct spdk_mem_map_ops *ops, void *cb_ctx), NULL);
DEFINE_STUB(spdk_nvmf_qpair_disconnect, int, (struct spdk_nvmf_qpair *qpair,
		nvmf_qpair_disconnect_cb cb_fn, void *ctx), 0);
DEFINE_STUB_V(spdk_mem_map_free, (struct spdk_mem_map **pmap));

struct spdk_trace_histories *g_trace_histories;
DEFINE_STUB_V(spdk_trace_add_register_fn, (struct spdk_trace_register_fn *reg_fn));
DEFINE_STUB_V(spdk_trace_register_object, (uint8_t type, char id_prefix));
DEFINE_STUB_V(spdk_trace_register_description, (const char *name,
		uint16_t tpoint_id, uint8_t owner_type, uint8_t object_type, uint8_t new_object,
		uint8_t arg1_type, const char *arg1_name));
DEFINE_STUB_V(_spdk_trace_record, (uint64_t tsc, uint16_t tpoint_id, uint16_t poller_id,
				   uint32_t size, uint64_t object_id, uint64_t arg1));

DEFINE_STUB_V(spdk_nvmf_request_exec, (struct spdk_nvmf_request *req));
DEFINE_STUB(spdk_nvme_transport_id_compare, int, (const struct spdk_nvme_transport_id *trid1,
		const struct spdk_nvme_transport_id *trid2), 0);
DEFINE_STUB_V(spdk_nvmf_ctrlr_abort_aer, (struct spdk_nvmf_ctrlr *ctrlr));
DEFINE_STUB(spdk_nvmf_request_get_dif_ctx, bool, (struct spdk_nvmf_request *req,
		struct spdk_dif_ctx *dif_ctx), false);

uint64_t
spdk_mem_map_translate(const struct spdk_mem_map *map, uint64_t vaddr, uint64_t *size)
{
	if (g_mr_size != 0) {
		*(uint32_t *)size = g_mr_size;
	}

	return (uint64_t)&g_rdma_mr;
}

static void reset_nvmf_rdma_request(struct spdk_nvmf_rdma_request *rdma_req)
{
	int i;

	rdma_req->req.length = 0;
	rdma_req->data_from_pool = false;
	rdma_req->req.data = NULL;
	rdma_req->data.wr.num_sge = 0;
	rdma_req->data.wr.wr.rdma.remote_addr = 0;
	rdma_req->data.wr.wr.rdma.rkey = 0;
	rdma_req->elba_length = 0;
	rdma_req->orig_length = 0;
	rdma_req->dif_insert_or_strip = false;

	for (i = 0; i < SPDK_NVMF_MAX_SGL_ENTRIES; i++) {
		rdma_req->req.iov[i].iov_base = 0;
		rdma_req->req.iov[i].iov_len = 0;
		rdma_req->buffers[i] = 0;
		rdma_req->data.wr.sg_list[i].addr = 0;
		rdma_req->data.wr.sg_list[i].length = 0;
		rdma_req->data.wr.sg_list[i].lkey = 0;
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

	data.wr.sg_list = data.sgl;
	STAILQ_INIT(&group.group.buf_cache);
	group.group.buf_cache_size = 0;
	group.group.buf_cache_count = 0;
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
	g_rdma_mr.lkey = 0xABCD;
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
	rc = spdk_nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);
	CU_ASSERT(rc == 0);
	CU_ASSERT(rdma_req.data_from_pool == true);
	CU_ASSERT(rdma_req.req.length == rtransport.transport.opts.io_unit_size / 2);
	CU_ASSERT((uint64_t)rdma_req.req.data == 0x2000);
	CU_ASSERT(rdma_req.data.wr.num_sge == 1);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.rkey == 0xEEEE);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.remote_addr == 0xFFFF);
	CU_ASSERT((uint64_t)rdma_req.buffers[0] == 0x2000);
	CU_ASSERT(rdma_req.data.wr.sg_list[0].addr == 0x2000);
	CU_ASSERT(rdma_req.data.wr.sg_list[0].length == rtransport.transport.opts.io_unit_size / 2);
	CU_ASSERT(rdma_req.data.wr.sg_list[0].lkey == g_rdma_mr.lkey);

	/* Part 2: simple I/O, one SGL larger than the transport io unit size (equal to the max io size) */
	reset_nvmf_rdma_request(&rdma_req);
	sgl->keyed.length = rtransport.transport.opts.io_unit_size * RDMA_UT_UNITS_IN_MAX_IO;
	rc = spdk_nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == 0);
	CU_ASSERT(rdma_req.data_from_pool == true);
	CU_ASSERT(rdma_req.req.length == rtransport.transport.opts.io_unit_size * RDMA_UT_UNITS_IN_MAX_IO);
	CU_ASSERT(rdma_req.data.wr.num_sge == RDMA_UT_UNITS_IN_MAX_IO);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.rkey == 0xEEEE);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.remote_addr == 0xFFFF);
	for (i = 0; i < RDMA_UT_UNITS_IN_MAX_IO; i++) {
		CU_ASSERT((uint64_t)rdma_req.buffers[i] == 0x2000);
		CU_ASSERT(rdma_req.data.wr.sg_list[i].addr == 0x2000);
		CU_ASSERT(rdma_req.data.wr.sg_list[i].length == rtransport.transport.opts.io_unit_size);
		CU_ASSERT(rdma_req.data.wr.sg_list[i].lkey == g_rdma_mr.lkey);
	}

	/* Part 3: simple I/O one SGL larger than the transport max io size */
	reset_nvmf_rdma_request(&rdma_req);
	sgl->keyed.length = rtransport.transport.opts.max_io_size * 2;
	rc = spdk_nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == -1);

	/* Part 4: Pretend there are no buffer pools */
	MOCK_SET(spdk_mempool_get, NULL);
	reset_nvmf_rdma_request(&rdma_req);
	sgl->keyed.length = rtransport.transport.opts.io_unit_size * RDMA_UT_UNITS_IN_MAX_IO;
	rc = spdk_nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == 0);
	CU_ASSERT(rdma_req.data_from_pool == false);
	CU_ASSERT(rdma_req.req.data == NULL);
	CU_ASSERT(rdma_req.data.wr.num_sge == 0);
	CU_ASSERT(rdma_req.buffers[0] == NULL);
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
	rc = spdk_nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == 0);
	CU_ASSERT(rdma_req.req.data == (void *)0xDDDD);
	CU_ASSERT(rdma_req.req.length == rtransport.transport.opts.in_capsule_data_size);
	CU_ASSERT(rdma_req.data_from_pool == false);

	/* Part 2: I/O offset + length too large */
	reset_nvmf_rdma_request(&rdma_req);
	sgl->address = rtransport.transport.opts.in_capsule_data_size;
	sgl->unkeyed.length = rtransport.transport.opts.in_capsule_data_size;
	rc = spdk_nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == -1);

	/* Part 3: I/O too large */
	reset_nvmf_rdma_request(&rdma_req);
	sgl->address = 0;
	sgl->unkeyed.length = rtransport.transport.opts.in_capsule_data_size * 2;
	rc = spdk_nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

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

	rc = spdk_nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == 0);
	CU_ASSERT(rdma_req.data_from_pool == true);
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

	rc = spdk_nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == 0);
	CU_ASSERT(rdma_req.data_from_pool == true);
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

	rc = spdk_nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == 0);
	CU_ASSERT(rdma_req.data_from_pool == true);
	CU_ASSERT(rdma_req.req.length == rtransport.transport.opts.io_unit_size * 16);
	CU_ASSERT(rdma_req.req.iovcnt == 17);
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

	/* Test 4: use PG buffer cache */
	sgl->generic.type = SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK;
	sgl->keyed.subtype = SPDK_NVME_SGL_SUBTYPE_ADDRESS;
	sgl->address = 0xFFFF;
	rdma_req.recv->buf = (void *)0xDDDD;
	g_rdma_mr.lkey = 0xABCD;
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
	rc = spdk_nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(rdma_req.data_from_pool == true);
	CU_ASSERT(rdma_req.req.length == rtransport.transport.opts.io_unit_size * 4);
	CU_ASSERT((uint64_t)rdma_req.req.data == (((uint64_t)&bufs[0] + NVMF_DATA_BUFFER_MASK) &
			~NVMF_DATA_BUFFER_MASK));
	CU_ASSERT(rdma_req.data.wr.num_sge == 4);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.rkey == 0xEEEE);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.remote_addr == 0xFFFF);
	CU_ASSERT(group.group.buf_cache_count == 0);
	CU_ASSERT(STAILQ_EMPTY(&group.group.buf_cache));
	for (i = 0; i < 4; i++) {
		CU_ASSERT((uint64_t)rdma_req.buffers[i] == (uint64_t)&bufs[i]);
		CU_ASSERT(rdma_req.data.wr.sg_list[i].addr == (((uint64_t)&bufs[i] + NVMF_DATA_BUFFER_MASK) &
				~NVMF_DATA_BUFFER_MASK));
		CU_ASSERT(rdma_req.data.wr.sg_list[i].length == rtransport.transport.opts.io_unit_size);
	}
	/* part 2: now that we have used the buffers from the cache, try again. We should get mempool buffers. */

	reset_nvmf_rdma_request(&rdma_req);
	rc = spdk_nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(rdma_req.data_from_pool == true);
	CU_ASSERT(rdma_req.req.length == rtransport.transport.opts.io_unit_size * 4);
	CU_ASSERT((uint64_t)rdma_req.req.data == 0x2000);
	CU_ASSERT(rdma_req.data.wr.num_sge == 4);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.rkey == 0xEEEE);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.remote_addr == 0xFFFF);
	CU_ASSERT(group.group.buf_cache_count == 0);
	CU_ASSERT(STAILQ_EMPTY(&group.group.buf_cache));
	for (i = 0; i < 4; i++) {
		CU_ASSERT((uint64_t)rdma_req.buffers[i] == 0x2000);
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
	rc = spdk_nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(rdma_req.data_from_pool == true);
	CU_ASSERT(rdma_req.req.length == rtransport.transport.opts.io_unit_size * 4);
	CU_ASSERT((uint64_t)rdma_req.req.data == (((uint64_t)&bufs[0] + NVMF_DATA_BUFFER_MASK) &
			~NVMF_DATA_BUFFER_MASK));
	CU_ASSERT(rdma_req.data.wr.num_sge == 4);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.rkey == 0xEEEE);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.remote_addr == 0xFFFF);
	CU_ASSERT(group.group.buf_cache_count == 0);
	for (i = 0; i < 2; i++) {
		CU_ASSERT((uint64_t)rdma_req.buffers[i] == (uint64_t)&bufs[i]);
		CU_ASSERT(rdma_req.data.wr.sg_list[i].addr == (((uint64_t)&bufs[i] + NVMF_DATA_BUFFER_MASK) &
				~NVMF_DATA_BUFFER_MASK));
		CU_ASSERT(rdma_req.data.wr.sg_list[i].length == rtransport.transport.opts.io_unit_size);
	}
	for (i = 2; i < 4; i++) {
		CU_ASSERT((uint64_t)rdma_req.buffers[i] == 0x2000);
		CU_ASSERT(rdma_req.data.wr.sg_list[i].addr == 0x2000);
		CU_ASSERT(rdma_req.data.wr.sg_list[i].length == rtransport.transport.opts.io_unit_size);
	}
}

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
	struct spdk_nvmf_rdma_request_data data;
	uint32_t data_bs;
	const uint32_t md_size = 8;
	int rc, i, c;
	void *aligned_buffer;

	data.wr.sg_list = data.sgl;
	STAILQ_INIT(&group.group.buf_cache);
	group.group.buf_cache_size = 0;
	group.group.buf_cache_count = 0;
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
	g_rdma_mr.lkey = 0xABCD;
	sgl->keyed.key = 0xEEEE;
	sgl->address = 0xFFFF;
	rdma_req.recv->buf = (void *)0xDDDD;

	/* Test 1: sgl type: keyed data block subtype: address */
	sgl->generic.type = SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK;
	sgl->keyed.subtype = SPDK_NVME_SGL_SUBTYPE_ADDRESS;

	/* Part 1: simple I/O, one SGL smaller than the transport io unit size, block size 512 */
	MOCK_SET(spdk_mempool_get, (void *)0x2000);
	reset_nvmf_rdma_request(&rdma_req);
	data_bs = 512;
	spdk_dif_ctx_init(&rdma_req.dif_ctx, data_bs + md_size, md_size, true, false, SPDK_DIF_TYPE1,
			  SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_REFTAG_CHECK, 0, 0, 0, 0, 0);
	rdma_req.dif_insert_or_strip = true;
	rtransport.transport.opts.io_unit_size = data_bs * 8;
	sgl->keyed.length = data_bs * 4;
	device.map = (void *)0x0;

	rc = spdk_nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == 0);
	CU_ASSERT(rdma_req.data_from_pool == true);
	CU_ASSERT(rdma_req.req.length == data_bs * 4);
	CU_ASSERT(rdma_req.orig_length == rdma_req.req.length);
	CU_ASSERT(rdma_req.elba_length == (data_bs + md_size) * 4);
	CU_ASSERT((uint64_t)rdma_req.req.data == 0x2000);
	CU_ASSERT(rdma_req.data.wr.num_sge == 4);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.rkey == 0xEEEE);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.remote_addr == 0xFFFF);
	CU_ASSERT((uint64_t)rdma_req.buffers[0] == 0x2000);

	for (i = 0; i < 4; ++i) {
		CU_ASSERT(rdma_req.data.wr.sg_list[i].addr == 0x2000 + i * (data_bs + md_size));
		CU_ASSERT(rdma_req.data.wr.sg_list[i].length == data_bs);
		CU_ASSERT(rdma_req.data.wr.sg_list[i].lkey == g_rdma_mr.lkey);
	}

	/* Part 2: simple I/O, one SGL equal to io unit size, io_unit_size is not aligned with md_size,
		block size 512 */
	MOCK_SET(spdk_mempool_get, (void *)0x2000);
	reset_nvmf_rdma_request(&rdma_req);
	rdma_req.dif_insert_or_strip = true;
	rtransport.transport.opts.io_unit_size = data_bs * 4;
	sgl->keyed.length = data_bs * 4;
	device.map = (void *)0x0;

	rc = spdk_nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == 0);
	CU_ASSERT(rdma_req.data_from_pool == true);
	CU_ASSERT(rdma_req.req.length == data_bs * 4);
	CU_ASSERT(rdma_req.orig_length == rdma_req.req.length);
	CU_ASSERT(rdma_req.elba_length == (data_bs + md_size) * 4);
	CU_ASSERT((uint64_t)rdma_req.req.data == 0x2000);
	CU_ASSERT(rdma_req.data.wr.num_sge == 5);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.rkey == 0xEEEE);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.remote_addr == 0xFFFF);
	CU_ASSERT((uint64_t)rdma_req.buffers[0] == 0x2000);

	for (i = 0; i < 3; ++i) {
		CU_ASSERT(rdma_req.data.wr.sg_list[i].addr == 0x2000 + i * (data_bs + md_size));
		CU_ASSERT(rdma_req.data.wr.sg_list[i].length == data_bs);
		CU_ASSERT(rdma_req.data.wr.sg_list[i].lkey == g_rdma_mr.lkey);
	}
	CU_ASSERT(rdma_req.data.wr.sg_list[3].addr == 0x2000 + 3 * (data_bs + md_size));
	CU_ASSERT(rdma_req.data.wr.sg_list[3].length == 488);
	CU_ASSERT(rdma_req.data.wr.sg_list[3].lkey == g_rdma_mr.lkey);

	/* 2nd buffer consumed */
	CU_ASSERT(rdma_req.data.wr.sg_list[4].addr == 0x2000);
	CU_ASSERT(rdma_req.data.wr.sg_list[4].length == 24);
	CU_ASSERT(rdma_req.data.wr.sg_list[4].lkey == g_rdma_mr.lkey);

	/* Part 3: simple I/O, one SGL equal io unit size, io_unit_size is equal to block size 512 bytes */
	MOCK_SET(spdk_mempool_get, (void *)0x2000);
	reset_nvmf_rdma_request(&rdma_req);
	rdma_req.dif_insert_or_strip = true;
	rtransport.transport.opts.io_unit_size = data_bs;
	sgl->keyed.length = data_bs;
	device.map = (void *)0x0;

	rc = spdk_nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == 0);
	CU_ASSERT(rdma_req.data_from_pool == true);
	CU_ASSERT(rdma_req.req.length == data_bs);
	CU_ASSERT(rdma_req.orig_length == rdma_req.req.length);
	CU_ASSERT(rdma_req.elba_length == data_bs + md_size);
	CU_ASSERT((uint64_t)rdma_req.req.data == 0x2000);
	CU_ASSERT(rdma_req.data.wr.num_sge == 1);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.rkey == 0xEEEE);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.remote_addr == 0xFFFF);
	CU_ASSERT((uint64_t)rdma_req.buffers[0] == 0x2000);

	CU_ASSERT(rdma_req.data.wr.sg_list[0].addr == 0x2000);
	CU_ASSERT(rdma_req.data.wr.sg_list[0].length == data_bs);
	CU_ASSERT(rdma_req.data.wr.sg_list[0].lkey == g_rdma_mr.lkey);

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
	rdma_req.dif_insert_or_strip = true;
	rtransport.transport.opts.io_unit_size = (data_bs + md_size) * 4;
	sgl->keyed.length = data_bs * 4;
	device.map = (void *)0x0;

	rc = spdk_nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == 0);
	CU_ASSERT(rdma_req.data_from_pool == true);
	CU_ASSERT(rdma_req.req.length == data_bs * 4);
	CU_ASSERT(rdma_req.orig_length == rdma_req.req.length);
	CU_ASSERT(rdma_req.elba_length == (data_bs + md_size) * 4);
	CU_ASSERT((uint64_t)rdma_req.req.data == 0x2000);
	CU_ASSERT(rdma_req.data.wr.num_sge == 4);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.rkey == 0xEEEE);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.remote_addr == 0xFFFF);
	CU_ASSERT((uint64_t)rdma_req.buffers[0] == 0x2000);

	for (i = 0; i < 4; ++i) {
		CU_ASSERT(rdma_req.data.wr.sg_list[i].addr == 0x2000 + i * (data_bs + md_size));
		CU_ASSERT(rdma_req.data.wr.sg_list[i].length == data_bs);
		CU_ASSERT(rdma_req.data.wr.sg_list[i].lkey == g_rdma_mr.lkey);
	}

	/* Part 5: simple I/O, one SGL equal to 2x io unit size, io_unit_size is aligned with md_size,
	   block size 512 */
	MOCK_SET(spdk_mempool_get, (void *)0x2000);
	reset_nvmf_rdma_request(&rdma_req);
	rdma_req.dif_insert_or_strip = true;
	rtransport.transport.opts.io_unit_size = (data_bs + md_size) * 2;
	sgl->keyed.length = data_bs * 4;
	device.map = (void *)0x0;

	rc = spdk_nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == 0);
	CU_ASSERT(rdma_req.data_from_pool == true);
	CU_ASSERT(rdma_req.req.length == data_bs * 4);
	CU_ASSERT(rdma_req.orig_length == rdma_req.req.length);
	CU_ASSERT(rdma_req.elba_length == (data_bs + md_size) * 4);
	CU_ASSERT((uint64_t)rdma_req.req.data == 0x2000);
	CU_ASSERT(rdma_req.data.wr.num_sge == 4);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.rkey == 0xEEEE);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.remote_addr == 0xFFFF);
	CU_ASSERT((uint64_t)rdma_req.buffers[0] == 0x2000);

	for (i = 0; i < 2; ++i) {
		CU_ASSERT(rdma_req.data.wr.sg_list[i].addr == 0x2000 + i * (data_bs + md_size));
		CU_ASSERT(rdma_req.data.wr.sg_list[i].length == data_bs);
	}
	for (i = 0; i < 2; ++i) {
		CU_ASSERT(rdma_req.data.wr.sg_list[i + 2].addr == 0x2000 + i * (data_bs + md_size));
		CU_ASSERT(rdma_req.data.wr.sg_list[i + 2].length == data_bs);
	}

	/* Part 6: simple I/O, one SGL larger than the transport io unit size, io_unit_size is not aligned to md_size,
	   block size 512 */
	MOCK_SET(spdk_mempool_get, (void *)0x2000);
	reset_nvmf_rdma_request(&rdma_req);
	rdma_req.dif_insert_or_strip = true;
	rtransport.transport.opts.io_unit_size = data_bs * 4;
	sgl->keyed.length = data_bs * 6;
	device.map = (void *)0x0;

	rc = spdk_nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == 0);
	CU_ASSERT(rdma_req.data_from_pool == true);
	CU_ASSERT(rdma_req.req.length == data_bs * 6);
	CU_ASSERT(rdma_req.orig_length == rdma_req.req.length);
	CU_ASSERT(rdma_req.elba_length == (data_bs + md_size) * 6);
	CU_ASSERT((uint64_t)rdma_req.req.data == 0x2000);
	CU_ASSERT(rdma_req.data.wr.num_sge == 7);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.rkey == 0xEEEE);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.remote_addr == 0xFFFF);
	CU_ASSERT((uint64_t)rdma_req.buffers[0] == 0x2000);

	for (i = 0; i < 3; ++i) {
		CU_ASSERT(rdma_req.data.wr.sg_list[i].addr == 0x2000 + i * (data_bs + md_size));
		CU_ASSERT(rdma_req.data.wr.sg_list[i].length == data_bs);
		CU_ASSERT(rdma_req.data.wr.sg_list[i].lkey == g_rdma_mr.lkey);
	}
	CU_ASSERT(rdma_req.data.wr.sg_list[3].addr == 0x2000 + 3 * (data_bs + md_size));
	CU_ASSERT(rdma_req.data.wr.sg_list[3].length == 488);
	CU_ASSERT(rdma_req.data.wr.sg_list[3].lkey == g_rdma_mr.lkey);

	/* 2nd buffer consumed */
	CU_ASSERT(rdma_req.data.wr.sg_list[4].addr == 0x2000);
	CU_ASSERT(rdma_req.data.wr.sg_list[4].length == 24);
	CU_ASSERT(rdma_req.data.wr.sg_list[4].lkey == g_rdma_mr.lkey);

	CU_ASSERT(rdma_req.data.wr.sg_list[5].addr == 0x2000 + 24 + md_size);
	CU_ASSERT(rdma_req.data.wr.sg_list[5].length == 512);
	CU_ASSERT(rdma_req.data.wr.sg_list[5].lkey == g_rdma_mr.lkey);

	CU_ASSERT(rdma_req.data.wr.sg_list[6].addr == 0x2000 + 24 + 512 + md_size * 2);
	CU_ASSERT(rdma_req.data.wr.sg_list[6].length == 512);
	CU_ASSERT(rdma_req.data.wr.sg_list[6].lkey == g_rdma_mr.lkey);

	/* Part 7: simple I/O, number of SGL exceeds the limitation */
	/* workaround to prevent free in spdk_mempool_put */
	ut_spdk_mempool_get_mocked = false;
	reset_nvmf_rdma_request(&rdma_req);
	rdma_req.dif_insert_or_strip = true;
	rtransport.transport.opts.io_unit_size = data_bs * 4;
	sgl->keyed.length = data_bs * 17;

	rc = spdk_nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == -1);

	/* Test 2: sgl type: keyed data block subtype: offset (in capsule data) */
	sgl->generic.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
	sgl->unkeyed.subtype = SPDK_NVME_SGL_SUBTYPE_OFFSET;

	/* Part 1: Normal I/O smaller than in capsule data size no offset */
	reset_nvmf_rdma_request(&rdma_req);
	rdma_req.dif_insert_or_strip = true;
	rtransport.transport.opts.in_capsule_data_size = data_bs * 4;
	c = 0;
	sgl->address = 0;
	sgl->unkeyed.length = data_bs * 3;
	rc = spdk_nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == 0);
	CU_ASSERT(rdma_req.req.data == (void *)0xDDDD);
	CU_ASSERT(rdma_req.req.length == data_bs * 3);
	CU_ASSERT(rdma_req.elba_length == (data_bs + md_size) * 3);
	CU_ASSERT(rdma_req.orig_length == rdma_req.req.length);
	CU_ASSERT(rdma_req.data_from_pool == false);
	CU_ASSERT(rdma_req.data.wr.num_sge == 0);
	CU_ASSERT(rdma_req.req.iovcnt == 6);

	for (i = 0; i < 6; i += 2) {
		CU_ASSERT(rdma_req.req.iov[i].iov_base == rdma_req.req.data + c++ * data_bs);
		CU_ASSERT(rdma_req.req.iov[i].iov_len == data_bs);
		CU_ASSERT(rdma_req.req.iov[i + 1].iov_len == md_size);
	}

	/* Test 3: Multi SGL */
	sgl->generic.type = SPDK_NVME_SGL_TYPE_LAST_SEGMENT;
	sgl->unkeyed.subtype = SPDK_NVME_SGL_SUBTYPE_OFFSET;
	sgl->address = 0;
	rdma_req.recv->buf = (void *)&sgl_desc;
	MOCK_SET(spdk_mempool_get, &data);
	aligned_buffer = (void *)((uintptr_t)((char *)&data + NVMF_DATA_BUFFER_MASK) &
				  ~NVMF_DATA_BUFFER_MASK);

	/* part 1: 2 segments each with 1 wr. io_unit_size is aligned with data_bs + md_size */
	reset_nvmf_rdma_request(&rdma_req);
	rdma_req.dif_insert_or_strip = true;
	rtransport.transport.opts.io_unit_size = (data_bs + md_size) * 4;
	sgl->unkeyed.length = 2 * sizeof(struct spdk_nvme_sgl_descriptor);

	for (i = 0; i < 2; i++) {
		sgl_desc[i].keyed.type = SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK;
		sgl_desc[i].keyed.subtype = SPDK_NVME_SGL_SUBTYPE_ADDRESS;
		sgl_desc[i].keyed.length = data_bs * 4;
		sgl_desc[i].address = 0x4000 + i * data_bs * 4;
		sgl_desc[i].keyed.key = 0x44;
	}

	rc = spdk_nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == 0);
	CU_ASSERT(rdma_req.data_from_pool == true);
	CU_ASSERT(rdma_req.req.length == data_bs * 4 * 2);
	CU_ASSERT(rdma_req.orig_length == rdma_req.req.length);
	CU_ASSERT(rdma_req.elba_length == (data_bs + md_size) * 4 * 2);
	CU_ASSERT(rdma_req.data.wr.num_sge == 4);
	for (i = 0; i < 4; ++i) {
		CU_ASSERT(rdma_req.data.wr.sg_list[i].addr == (uintptr_t)((unsigned char *)aligned_buffer) + i *
			  (data_bs + md_size));
		CU_ASSERT(rdma_req.data.wr.sg_list[i].length == data_bs);
	}

	CU_ASSERT(rdma_req.data.wr.wr.rdma.rkey == 0x44);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.remote_addr == 0x4000);
	CU_ASSERT(rdma_req.data.wr.next == &data.wr);
	CU_ASSERT(data.wr.wr.rdma.rkey == 0x44);
	CU_ASSERT(data.wr.wr.rdma.remote_addr == 0x4000 + data_bs * 4);
	CU_ASSERT(data.wr.num_sge == 4);
	for (i = 0; i < 4; ++i) {
		CU_ASSERT(data.wr.sg_list[i].addr == (uintptr_t)((unsigned char *)aligned_buffer) + i *
			  (data_bs + md_size));
		CU_ASSERT(data.wr.sg_list[i].length == data_bs);
	}

	CU_ASSERT(data.wr.next == &rdma_req.rsp.wr);
}


int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("nvmf", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "test_parse_sgl", test_spdk_nvmf_rdma_request_parse_sgl) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "test_parse_sgl_with_md",
			    test_spdk_nvmf_rdma_request_parse_sgl_with_md) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
