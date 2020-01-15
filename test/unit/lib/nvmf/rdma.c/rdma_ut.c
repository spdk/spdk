/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
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
#include "nvmf/transport.c"

uint64_t g_mr_size;
uint64_t g_mr_next_size;
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
DEFINE_STUB_V(spdk_nvme_trid_populate_transport, (struct spdk_nvme_transport_id *trid,
		enum spdk_nvme_transport_type trtype));

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

uint64_t
spdk_mem_map_translate(const struct spdk_mem_map *map, uint64_t vaddr, uint64_t *size)
{
	if (g_mr_size != 0) {
		*(uint32_t *)size = g_mr_size;
		if (g_mr_next_size != 0) {
			g_mr_size = g_mr_next_size;
		}
	}

	return (uint64_t)&g_rdma_mr;
}

static void reset_nvmf_rdma_request(struct spdk_nvmf_rdma_request *rdma_req)
{
	int i;

	rdma_req->req.length = 0;
	rdma_req->req.data_from_pool = false;
	rdma_req->req.data = NULL;
	rdma_req->data.wr.num_sge = 0;
	rdma_req->data.wr.wr.rdma.remote_addr = 0;
	rdma_req->data.wr.wr.rdma.rkey = 0;
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
	struct spdk_nvmf_transport_pg_cache_buf	buffer;
	struct spdk_nvmf_transport_pg_cache_buf	*buffer_ptr;
	int rc, i;

	data.wr.sg_list = data.sgl;
	STAILQ_INIT(&group.group.buf_cache);
	group.group.buf_cache_size = 0;
	group.group.buf_cache_count = 0;
	group.group.transport = &rtransport.transport;
	STAILQ_INIT(&group.retired_bufs);
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
	CU_ASSERT(rdma_req.req.data_from_pool == true);
	CU_ASSERT(rdma_req.req.length == rtransport.transport.opts.io_unit_size / 2);
	CU_ASSERT((uint64_t)rdma_req.req.data == 0x2000);
	CU_ASSERT(rdma_req.data.wr.num_sge == 1);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.rkey == 0xEEEE);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.remote_addr == 0xFFFF);
	CU_ASSERT((uint64_t)rdma_req.req.buffers[0] == 0x2000);
	CU_ASSERT(rdma_req.data.wr.sg_list[0].addr == 0x2000);
	CU_ASSERT(rdma_req.data.wr.sg_list[0].length == rtransport.transport.opts.io_unit_size / 2);
	CU_ASSERT(rdma_req.data.wr.sg_list[0].lkey == g_rdma_mr.lkey);

	/* Part 2: simple I/O, one SGL larger than the transport io unit size (equal to the max io size) */
	reset_nvmf_rdma_request(&rdma_req);
	sgl->keyed.length = rtransport.transport.opts.io_unit_size * RDMA_UT_UNITS_IN_MAX_IO;
	rc = spdk_nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

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
	rc = spdk_nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == 0);
	CU_ASSERT(rdma_req.req.data == (void *)0xDDDD);
	CU_ASSERT(rdma_req.req.length == rtransport.transport.opts.in_capsule_data_size);
	CU_ASSERT(rdma_req.req.data_from_pool == false);

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

	rc = spdk_nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

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

	rc = spdk_nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == 0);
	CU_ASSERT(rdma_req.req.data_from_pool == true);
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
	rc = spdk_nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

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
	rc = spdk_nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

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
	/* Test 5 dealing with a buffer split over two Memory Regions */
	MOCK_SET(spdk_mempool_get, (void *)&buffer);
	sgl->generic.type = SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK;
	sgl->keyed.subtype = SPDK_NVME_SGL_SUBTYPE_ADDRESS;
	sgl->keyed.length = rtransport.transport.opts.io_unit_size / 2;
	g_mr_size = rtransport.transport.opts.io_unit_size / 4;
	g_mr_next_size = rtransport.transport.opts.io_unit_size / 2;

	rc = spdk_nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(rdma_req.req.data_from_pool == true);
	CU_ASSERT(rdma_req.req.length == rtransport.transport.opts.io_unit_size / 2);
	CU_ASSERT((uint64_t)rdma_req.req.data == (((uint64_t)&buffer + NVMF_DATA_BUFFER_MASK) &
			~NVMF_DATA_BUFFER_MASK));
	CU_ASSERT(rdma_req.data.wr.num_sge == 1);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.rkey == 0xEEEE);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.remote_addr == 0xFFFF);
	CU_ASSERT(rdma_req.req.buffers[0] == &buffer);
	CU_ASSERT(rdma_req.data.wr.sg_list[0].addr == (((uint64_t)&buffer + NVMF_DATA_BUFFER_MASK) &
			~NVMF_DATA_BUFFER_MASK));
	CU_ASSERT(rdma_req.data.wr.sg_list[0].length == rtransport.transport.opts.io_unit_size / 2);
	CU_ASSERT(rdma_req.data.wr.sg_list[0].lkey == g_rdma_mr.lkey);
	buffer_ptr = STAILQ_FIRST(&group.retired_bufs);
	CU_ASSERT(buffer_ptr == &buffer);
	STAILQ_REMOVE(&group.retired_bufs, buffer_ptr, spdk_nvmf_transport_pg_cache_buf, link);
	CU_ASSERT(STAILQ_EMPTY(&group.retired_bufs));
	g_mr_size = 0;
	g_mr_next_size = 0;

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
	    struct spdk_nvmf_rdma_resources *resources)
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
	resources->recvs_to_post.first = resources->recvs_to_post.last = NULL;
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
	qpair_reset(&rqpair, &poller, &device, &resources);

	rtransport.transport.opts = g_rdma_ut_transport_opts;
	rtransport.transport.data_buf_pool = spdk_mempool_create("test_data_pool", 16, 128, 0, 0);
	rtransport.data_wr_pool = spdk_mempool_create("test_wr_pool", 128,
				  sizeof(struct spdk_nvmf_rdma_request_data),
				  0, 0);
	MOCK_CLEAR(spdk_mempool_get);

	device.attr.device_cap_flags = 0;
	device.map = (void *)0x0;
	g_rdma_mr.lkey = 0xABCD;

	/* Test 1: single SGL READ request */
	rdma_recv = create_recv(&rqpair, SPDK_NVME_OPC_READ);
	rdma_req = create_req(&rqpair, rdma_recv);
	rqpair.current_recv_depth = 1;
	/* NEW -> EXECUTING */
	progress = spdk_nvmf_rdma_request_process(&rtransport, rdma_req);
	CU_ASSERT(progress == true);
	CU_ASSERT(rdma_req->state == RDMA_REQUEST_STATE_EXECUTING);
	CU_ASSERT(rdma_req->req.xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST);
	/* EXECUTED -> TRANSFERRING_C2H */
	rdma_req->state = RDMA_REQUEST_STATE_EXECUTED;
	progress = spdk_nvmf_rdma_request_process(&rtransport, rdma_req);
	CU_ASSERT(progress == true);
	CU_ASSERT(rdma_req->state == RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST);
	CU_ASSERT(rdma_req->recv == NULL);
	CU_ASSERT(rqpair.sends_to_post.first == &rdma_req->data.wr);
	CU_ASSERT(rqpair.sends_to_post.last == &rdma_req->rsp.wr);
	CU_ASSERT(resources.recvs_to_post.first == &rdma_recv->wr);
	CU_ASSERT(resources.recvs_to_post.last == &rdma_recv->wr);
	/* COMPLETED -> FREE */
	rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
	progress = spdk_nvmf_rdma_request_process(&rtransport, rdma_req);
	CU_ASSERT(progress == true);
	CU_ASSERT(rdma_req->state == RDMA_REQUEST_STATE_FREE);

	free_recv(rdma_recv);
	free_req(rdma_req);
	poller_reset(&poller, &group);
	qpair_reset(&rqpair, &poller, &device, &resources);

	/* Test 2: single SGL WRITE request */
	rdma_recv = create_recv(&rqpair, SPDK_NVME_OPC_WRITE);
	rdma_req = create_req(&rqpair, rdma_recv);
	rqpair.current_recv_depth = 1;
	/* NEW -> TRANSFERRING_H2C */
	progress = spdk_nvmf_rdma_request_process(&rtransport, rdma_req);
	CU_ASSERT(progress == true);
	CU_ASSERT(rdma_req->state == RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER);
	CU_ASSERT(rdma_req->req.xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER);
	CU_ASSERT(rqpair.sends_to_post.first == &rdma_req->data.wr);
	CU_ASSERT(rqpair.sends_to_post.last == &rdma_req->data.wr);
	rqpair.sends_to_post.first = rqpair.sends_to_post.last = NULL;
	STAILQ_INIT(&poller.qpairs_pending_send);
	/* READY_TO_EXECUTE -> EXECUTING */
	rdma_req->state = RDMA_REQUEST_STATE_READY_TO_EXECUTE;
	progress = spdk_nvmf_rdma_request_process(&rtransport, rdma_req);
	CU_ASSERT(progress == true);
	CU_ASSERT(rdma_req->state == RDMA_REQUEST_STATE_EXECUTING);
	/* EXECUTED -> COMPLETING */
	rdma_req->state = RDMA_REQUEST_STATE_EXECUTED;
	progress = spdk_nvmf_rdma_request_process(&rtransport, rdma_req);
	CU_ASSERT(progress == true);
	CU_ASSERT(rdma_req->state == RDMA_REQUEST_STATE_COMPLETING);
	CU_ASSERT(rdma_req->recv == NULL);
	CU_ASSERT(rqpair.sends_to_post.first == &rdma_req->rsp.wr);
	CU_ASSERT(rqpair.sends_to_post.last == &rdma_req->rsp.wr);
	CU_ASSERT(resources.recvs_to_post.first == &rdma_recv->wr);
	CU_ASSERT(resources.recvs_to_post.last == &rdma_recv->wr);
	/* COMPLETED -> FREE */
	rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
	progress = spdk_nvmf_rdma_request_process(&rtransport, rdma_req);
	CU_ASSERT(progress == true);
	CU_ASSERT(rdma_req->state == RDMA_REQUEST_STATE_FREE);

	free_recv(rdma_recv);
	free_req(rdma_req);
	poller_reset(&poller, &group);
	qpair_reset(&rqpair, &poller, &device, &resources);

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
		spdk_nvmf_rdma_request_process(&rtransport, req1);
		CU_ASSERT(req1->state == RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER);
		/* WRITE 1 is the first in batching list */
		CU_ASSERT(rqpair.sends_to_post.first == &req1->data.wr);
		CU_ASSERT(rqpair.sends_to_post.last == &req1->data.wr);

		/* WRITE 2: NEW -> TRANSFERRING_H2C */
		rqpair.current_recv_depth = 2;
		spdk_nvmf_rdma_request_process(&rtransport, req2);
		CU_ASSERT(req2->state == RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER);
		/* WRITE 2 is now also in the batching list */
		CU_ASSERT(rqpair.sends_to_post.first->next == &req2->data.wr);
		CU_ASSERT(rqpair.sends_to_post.last == &req2->data.wr);

		/* Send everything */
		rqpair.sends_to_post.first = rqpair.sends_to_post.last = NULL;
		STAILQ_INIT(&poller.qpairs_pending_send);

		/* WRITE 1 completes before WRITE 2 has finished RDMA reading */
		/* WRITE 1: READY_TO_EXECUTE -> EXECUTING */
		req1->state = RDMA_REQUEST_STATE_READY_TO_EXECUTE;
		spdk_nvmf_rdma_request_process(&rtransport, req1);
		CU_ASSERT(req1->state == RDMA_REQUEST_STATE_EXECUTING);
		/* WRITE 1: EXECUTED -> COMPLETING */
		req1->state = RDMA_REQUEST_STATE_EXECUTED;
		spdk_nvmf_rdma_request_process(&rtransport, req1);
		CU_ASSERT(req1->state == RDMA_REQUEST_STATE_COMPLETING);
		CU_ASSERT(rqpair.sends_to_post.first == &req1->rsp.wr);
		CU_ASSERT(rqpair.sends_to_post.last == &req1->rsp.wr);
		rqpair.sends_to_post.first = rqpair.sends_to_post.last = NULL;
		STAILQ_INIT(&poller.qpairs_pending_send);
		/* WRITE 1: COMPLETED -> FREE */
		req1->state = RDMA_REQUEST_STATE_COMPLETED;
		spdk_nvmf_rdma_request_process(&rtransport, req1);
		CU_ASSERT(req1->state == RDMA_REQUEST_STATE_FREE);

		/* Now WRITE 2 has finished reading and completes */
		/* WRITE 2: COMPLETED -> FREE */
		/* WRITE 2: READY_TO_EXECUTE -> EXECUTING */
		req2->state = RDMA_REQUEST_STATE_READY_TO_EXECUTE;
		spdk_nvmf_rdma_request_process(&rtransport, req2);
		CU_ASSERT(req2->state == RDMA_REQUEST_STATE_EXECUTING);
		/* WRITE 1: EXECUTED -> COMPLETING */
		req2->state = RDMA_REQUEST_STATE_EXECUTED;
		spdk_nvmf_rdma_request_process(&rtransport, req2);
		CU_ASSERT(req2->state == RDMA_REQUEST_STATE_COMPLETING);
		CU_ASSERT(rqpair.sends_to_post.first == &req2->rsp.wr);
		CU_ASSERT(rqpair.sends_to_post.last == &req2->rsp.wr);
		rqpair.sends_to_post.first = rqpair.sends_to_post.last = NULL;
		STAILQ_INIT(&poller.qpairs_pending_send);
		/* WRITE 1: COMPLETED -> FREE */
		req2->state = RDMA_REQUEST_STATE_COMPLETED;
		spdk_nvmf_rdma_request_process(&rtransport, req2);
		CU_ASSERT(req2->state == RDMA_REQUEST_STATE_FREE);

		free_recv(recv1);
		free_req(req1);
		free_recv(recv2);
		free_req(req2);
		poller_reset(&poller, &group);
		qpair_reset(&rqpair, &poller, &device, &resources);
	}

	spdk_mempool_free(rtransport.transport.data_buf_pool);
	spdk_mempool_free(rtransport.data_wr_pool);
}

#define TEST_GROUPS_COUNT 5
static void
test_spdk_nvmf_rdma_get_optimal_poll_group(void)
{
	struct spdk_nvmf_rdma_transport rtransport = {};
	struct spdk_nvmf_transport *transport = &rtransport.transport;
	struct spdk_nvmf_rdma_qpair rqpair = {};
	struct spdk_nvmf_transport_poll_group *groups[TEST_GROUPS_COUNT];
	struct spdk_nvmf_rdma_poll_group *rgroups[TEST_GROUPS_COUNT];
	struct spdk_nvmf_transport_poll_group *result;
	uint32_t i;

	rqpair.qpair.transport = transport;
	pthread_mutex_init(&rtransport.lock, NULL);
	TAILQ_INIT(&rtransport.poll_groups);

	for (i = 0; i < TEST_GROUPS_COUNT; i++) {
		groups[i] = spdk_nvmf_rdma_poll_group_create(transport);
		CU_ASSERT(groups[i] != NULL);
		rgroups[i] = SPDK_CONTAINEROF(groups[i], struct spdk_nvmf_rdma_poll_group, group);
		groups[i]->transport = transport;
	}
	CU_ASSERT(rtransport.conn_sched.next_admin_pg == rgroups[0]);
	CU_ASSERT(rtransport.conn_sched.next_io_pg == rgroups[0]);

	/* Emulate connection of %TEST_GROUPS_COUNT% initiators - each creates 1 admin and 1 io qp */
	for (i = 0; i < TEST_GROUPS_COUNT; i++) {
		rqpair.qpair.qid = 0;
		result = spdk_nvmf_rdma_get_optimal_poll_group(&rqpair.qpair);
		CU_ASSERT(result == groups[i]);
		CU_ASSERT(rtransport.conn_sched.next_admin_pg == rgroups[(i + 1) % TEST_GROUPS_COUNT]);
		CU_ASSERT(rtransport.conn_sched.next_io_pg == rgroups[i]);

		rqpair.qpair.qid = 1;
		result = spdk_nvmf_rdma_get_optimal_poll_group(&rqpair.qpair);
		CU_ASSERT(result == groups[i]);
		CU_ASSERT(rtransport.conn_sched.next_admin_pg == rgroups[(i + 1) % TEST_GROUPS_COUNT]);
		CU_ASSERT(rtransport.conn_sched.next_io_pg == rgroups[(i + 1) % TEST_GROUPS_COUNT]);
	}
	/* wrap around, admin/io pg point to the first pg
	   Destroy all poll groups except of the last one */
	for (i = 0; i < TEST_GROUPS_COUNT - 1; i++) {
		spdk_nvmf_rdma_poll_group_destroy(groups[i]);
		CU_ASSERT(rtransport.conn_sched.next_admin_pg == rgroups[i + 1]);
		CU_ASSERT(rtransport.conn_sched.next_io_pg == rgroups[i + 1]);
	}

	CU_ASSERT(rtransport.conn_sched.next_admin_pg == rgroups[TEST_GROUPS_COUNT - 1]);
	CU_ASSERT(rtransport.conn_sched.next_io_pg == rgroups[TEST_GROUPS_COUNT - 1]);

	/* Check that pointers to the next admin/io poll groups are not changed */
	rqpair.qpair.qid = 0;
	result = spdk_nvmf_rdma_get_optimal_poll_group(&rqpair.qpair);
	CU_ASSERT(result == groups[TEST_GROUPS_COUNT - 1]);
	CU_ASSERT(rtransport.conn_sched.next_admin_pg == rgroups[TEST_GROUPS_COUNT - 1]);
	CU_ASSERT(rtransport.conn_sched.next_io_pg == rgroups[TEST_GROUPS_COUNT - 1]);

	rqpair.qpair.qid = 1;
	result = spdk_nvmf_rdma_get_optimal_poll_group(&rqpair.qpair);
	CU_ASSERT(result == groups[TEST_GROUPS_COUNT - 1]);
	CU_ASSERT(rtransport.conn_sched.next_admin_pg == rgroups[TEST_GROUPS_COUNT - 1]);
	CU_ASSERT(rtransport.conn_sched.next_io_pg == rgroups[TEST_GROUPS_COUNT - 1]);

	/* Remove the last poll group, check that pointers are NULL */
	spdk_nvmf_rdma_poll_group_destroy(groups[TEST_GROUPS_COUNT - 1]);
	CU_ASSERT(rtransport.conn_sched.next_admin_pg == NULL);
	CU_ASSERT(rtransport.conn_sched.next_io_pg == NULL);

	/* Request optimal poll group, result must be NULL */
	rqpair.qpair.qid = 0;
	result = spdk_nvmf_rdma_get_optimal_poll_group(&rqpair.qpair);
	CU_ASSERT(result == NULL);

	rqpair.qpair.qid = 1;
	result = spdk_nvmf_rdma_get_optimal_poll_group(&rqpair.qpair);
	CU_ASSERT(result == NULL);

	pthread_mutex_destroy(&rtransport.lock);
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
	struct spdk_nvmf_rdma_request_data data;
	struct spdk_nvmf_transport_pg_cache_buf	buffer;
	struct spdk_nvmf_transport_pg_cache_buf	*buffer_ptr;
	const uint32_t data_bs = 512;
	const uint32_t md_size = 8;
	int rc, i;
	void *aligned_buffer;

	data.wr.sg_list = data.sgl;
	STAILQ_INIT(&group.group.buf_cache);
	group.group.buf_cache_size = 0;
	group.group.buf_cache_count = 0;
	group.group.transport = &rtransport.transport;
	STAILQ_INIT(&group.retired_bufs);
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
	spdk_dif_ctx_init(&rdma_req.req.dif.dif_ctx, data_bs + md_size, md_size, true, false,
			  SPDK_DIF_TYPE1, SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_REFTAG_CHECK,
			  0, 0, 0, 0, 0);
	rdma_req.req.dif.dif_insert_or_strip = true;
	rtransport.transport.opts.io_unit_size = data_bs * 8;
	sgl->keyed.length = data_bs * 4;

	rc = spdk_nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == 0);
	CU_ASSERT(rdma_req.req.data_from_pool == true);
	CU_ASSERT(rdma_req.req.length == data_bs * 4);
	CU_ASSERT(rdma_req.req.dif.orig_length == rdma_req.req.length);
	CU_ASSERT(rdma_req.req.dif.elba_length == (data_bs + md_size) * 4);
	CU_ASSERT((uint64_t)rdma_req.req.data == 0x2000);
	CU_ASSERT(rdma_req.data.wr.num_sge == 4);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.rkey == 0xEEEE);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.remote_addr == 0xFFFF);
	CU_ASSERT((uint64_t)rdma_req.req.buffers[0] == 0x2000);

	for (i = 0; i < 4; ++i) {
		CU_ASSERT(rdma_req.data.wr.sg_list[i].addr == 0x2000 + i * (data_bs + md_size));
		CU_ASSERT(rdma_req.data.wr.sg_list[i].length == data_bs);
		CU_ASSERT(rdma_req.data.wr.sg_list[i].lkey == g_rdma_mr.lkey);
	}

	/* Part 2: simple I/O, one SGL equal to io unit size, io_unit_size is not aligned with md_size,
		block size 512 */
	MOCK_SET(spdk_mempool_get, (void *)0x2000);
	reset_nvmf_rdma_request(&rdma_req);
	spdk_dif_ctx_init(&rdma_req.req.dif.dif_ctx, data_bs + md_size, md_size, true, false,
			  SPDK_DIF_TYPE1, SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_REFTAG_CHECK,
			  0, 0, 0, 0, 0);
	rdma_req.req.dif.dif_insert_or_strip = true;
	rtransport.transport.opts.io_unit_size = data_bs * 4;
	sgl->keyed.length = data_bs * 4;

	rc = spdk_nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

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
	spdk_dif_ctx_init(&rdma_req.req.dif.dif_ctx, data_bs + md_size, md_size, true, false,
			  SPDK_DIF_TYPE1, SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_REFTAG_CHECK,
			  0, 0, 0, 0, 0);
	rdma_req.req.dif.dif_insert_or_strip = true;
	rtransport.transport.opts.io_unit_size = data_bs;
	sgl->keyed.length = data_bs;

	rc = spdk_nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

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
	spdk_dif_ctx_init(&rdma_req.req.dif.dif_ctx, data_bs + md_size, md_size, true, false,
			  SPDK_DIF_TYPE1, SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_REFTAG_CHECK,
			  0, 0, 0, 0, 0);
	rdma_req.req.dif.dif_insert_or_strip = true;
	rtransport.transport.opts.io_unit_size = (data_bs + md_size) * 4;
	sgl->keyed.length = data_bs * 4;

	rc = spdk_nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == 0);
	CU_ASSERT(rdma_req.req.data_from_pool == true);
	CU_ASSERT(rdma_req.req.length == data_bs * 4);
	CU_ASSERT(rdma_req.req.dif.orig_length == rdma_req.req.length);
	CU_ASSERT(rdma_req.req.dif.elba_length == (data_bs + md_size) * 4);
	CU_ASSERT((uint64_t)rdma_req.req.data == 0x2000);
	CU_ASSERT(rdma_req.data.wr.num_sge == 4);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.rkey == 0xEEEE);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.remote_addr == 0xFFFF);
	CU_ASSERT((uint64_t)rdma_req.req.buffers[0] == 0x2000);

	for (i = 0; i < 4; ++i) {
		CU_ASSERT(rdma_req.data.wr.sg_list[i].addr == 0x2000 + i * (data_bs + md_size));
		CU_ASSERT(rdma_req.data.wr.sg_list[i].length == data_bs);
		CU_ASSERT(rdma_req.data.wr.sg_list[i].lkey == g_rdma_mr.lkey);
	}

	/* Part 5: simple I/O, one SGL equal to 2x io unit size, io_unit_size is aligned with md_size,
	   block size 512 */
	MOCK_SET(spdk_mempool_get, (void *)0x2000);
	reset_nvmf_rdma_request(&rdma_req);
	spdk_dif_ctx_init(&rdma_req.req.dif.dif_ctx, data_bs + md_size, md_size, true, false,
			  SPDK_DIF_TYPE1, SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_REFTAG_CHECK,
			  0, 0, 0, 0, 0);
	rdma_req.req.dif.dif_insert_or_strip = true;
	rtransport.transport.opts.io_unit_size = (data_bs + md_size) * 2;
	sgl->keyed.length = data_bs * 4;

	rc = spdk_nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

	CU_ASSERT(rc == 0);
	CU_ASSERT(rdma_req.req.data_from_pool == true);
	CU_ASSERT(rdma_req.req.length == data_bs * 4);
	CU_ASSERT(rdma_req.req.dif.orig_length == rdma_req.req.length);
	CU_ASSERT(rdma_req.req.dif.elba_length == (data_bs + md_size) * 4);
	CU_ASSERT((uint64_t)rdma_req.req.data == 0x2000);
	CU_ASSERT(rdma_req.data.wr.num_sge == 4);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.rkey == 0xEEEE);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.remote_addr == 0xFFFF);
	CU_ASSERT((uint64_t)rdma_req.req.buffers[0] == 0x2000);

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
	spdk_dif_ctx_init(&rdma_req.req.dif.dif_ctx, data_bs + md_size, md_size, true, false,
			  SPDK_DIF_TYPE1, SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_REFTAG_CHECK,
			  0, 0, 0, 0, 0);
	rdma_req.req.dif.dif_insert_or_strip = true;
	rtransport.transport.opts.io_unit_size = data_bs * 4;
	sgl->keyed.length = data_bs * 6;

	rc = spdk_nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

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
		CU_ASSERT(rdma_req.data.wr.sg_list[i].lkey == g_rdma_mr.lkey);
	}
	CU_ASSERT(rdma_req.data.wr.sg_list[3].addr == 0x2000 + 3 * (data_bs + md_size));
	CU_ASSERT(rdma_req.data.wr.sg_list[3].length == 488);
	CU_ASSERT(rdma_req.data.wr.sg_list[3].lkey == g_rdma_mr.lkey);

	/* 2nd IO buffer consumed */
	CU_ASSERT(rdma_req.data.wr.sg_list[4].addr == 0x2000);
	CU_ASSERT(rdma_req.data.wr.sg_list[4].length == 24);
	CU_ASSERT(rdma_req.data.wr.sg_list[4].lkey == g_rdma_mr.lkey);

	CU_ASSERT(rdma_req.data.wr.sg_list[5].addr == 0x2000 + 24 + md_size);
	CU_ASSERT(rdma_req.data.wr.sg_list[5].length == 512);
	CU_ASSERT(rdma_req.data.wr.sg_list[5].lkey == g_rdma_mr.lkey);

	CU_ASSERT(rdma_req.data.wr.sg_list[6].addr == 0x2000 + 24 + 512 + md_size * 2);
	CU_ASSERT(rdma_req.data.wr.sg_list[6].length == 512);
	CU_ASSERT(rdma_req.data.wr.sg_list[6].lkey == g_rdma_mr.lkey);

	/* Part 7: simple I/O, number of SGL entries exceeds the number of entries
	   one WR can hold. Additional WR is chained */
	MOCK_SET(spdk_mempool_get, &data);
	aligned_buffer = (void *)((uintptr_t)((char *)&data + NVMF_DATA_BUFFER_MASK) &
				  ~NVMF_DATA_BUFFER_MASK);
	reset_nvmf_rdma_request(&rdma_req);
	spdk_dif_ctx_init(&rdma_req.req.dif.dif_ctx, data_bs + md_size, md_size, true, false,
			  SPDK_DIF_TYPE1, SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_REFTAG_CHECK,
			  0, 0, 0, 0, 0);
	rdma_req.req.dif.dif_insert_or_strip = true;
	rtransport.transport.opts.io_unit_size = data_bs * 16;
	sgl->keyed.length = data_bs * 16;

	rc = spdk_nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

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
	/* additional wr from pool */
	CU_ASSERT(rdma_req.data.wr.next == (void *)&data.wr);
	CU_ASSERT(rdma_req.data.wr.next->num_sge == 1);
	CU_ASSERT(rdma_req.data.wr.next->next == &rdma_req.rsp.wr);

	/* Part 8: simple I/O, data with metadata do not fit to 1 io_buffer */
	MOCK_SET(spdk_mempool_get, (void *)0x2000);
	reset_nvmf_rdma_request(&rdma_req);
	spdk_dif_ctx_init(&rdma_req.req.dif.dif_ctx, data_bs + md_size, md_size, true, false,
			  SPDK_DIF_TYPE1, SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_REFTAG_CHECK,
			  0, 0, 0, 0, 0);
	rdma_req.req.dif.dif_insert_or_strip = true;
	rtransport.transport.opts.io_unit_size = 516;
	sgl->keyed.length = data_bs * 2;

	rc = spdk_nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);

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
	CU_ASSERT(rdma_req.data.wr.sg_list[0].lkey == g_rdma_mr.lkey);

	/* 2nd IO buffer consumed, offset 4 bytes due to part of the metadata
	  is located at the beginning of that buffer */
	CU_ASSERT(rdma_req.data.wr.sg_list[1].addr == 0x2000 + 4);
	CU_ASSERT(rdma_req.data.wr.sg_list[1].length == 512);
	CU_ASSERT(rdma_req.data.wr.sg_list[1].lkey == g_rdma_mr.lkey);

	/* Test 9 dealing with a buffer split over two Memory Regions */
	MOCK_SET(spdk_mempool_get, (void *)&buffer);
	reset_nvmf_rdma_request(&rdma_req);
	spdk_dif_ctx_init(&rdma_req.req.dif.dif_ctx, data_bs + md_size, md_size, true, false,
			  SPDK_DIF_TYPE1, SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_REFTAG_CHECK,
			  0, 0, 0, 0, 0);
	rdma_req.req.dif.dif_insert_or_strip = true;
	rtransport.transport.opts.io_unit_size = data_bs * 4;
	sgl->keyed.length = data_bs * 2;
	g_mr_size = data_bs;
	g_mr_next_size = rtransport.transport.opts.io_unit_size;

	rc = spdk_nvmf_rdma_request_parse_sgl(&rtransport, &device, &rdma_req);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(rdma_req.req.data_from_pool == true);
	CU_ASSERT(rdma_req.req.length == rtransport.transport.opts.io_unit_size / 2);
	CU_ASSERT((uint64_t)rdma_req.req.data == (((uint64_t)&buffer + NVMF_DATA_BUFFER_MASK) &
			~NVMF_DATA_BUFFER_MASK));
	CU_ASSERT(rdma_req.data.wr.num_sge == 2);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.rkey == 0xEEEE);
	CU_ASSERT(rdma_req.data.wr.wr.rdma.remote_addr == 0xFFFF);
	CU_ASSERT(rdma_req.req.buffers[0] == &buffer);
	for (i = 0; i < 2; i++) {
		CU_ASSERT(rdma_req.data.wr.sg_list[i].addr == (uint64_t)rdma_req.req.data + i *
			  (data_bs + md_size));
		CU_ASSERT(rdma_req.data.wr.sg_list[i].length == data_bs);
		CU_ASSERT(rdma_req.data.wr.sg_list[i].lkey == g_rdma_mr.lkey);
	}
	buffer_ptr = STAILQ_FIRST(&group.retired_bufs);
	CU_ASSERT(buffer_ptr == &buffer);
	STAILQ_REMOVE(&group.retired_bufs, buffer_ptr, spdk_nvmf_transport_pg_cache_buf, link);
	CU_ASSERT(STAILQ_EMPTY(&group.retired_bufs));
	g_mr_size = 0;
	g_mr_next_size = 0;

	/* Test 2: Multi SGL */
	sgl->generic.type = SPDK_NVME_SGL_TYPE_LAST_SEGMENT;
	sgl->unkeyed.subtype = SPDK_NVME_SGL_SUBTYPE_OFFSET;
	sgl->address = 0;
	rdma_req.recv->buf = (void *)&sgl_desc;
	MOCK_SET(spdk_mempool_get, &data);
	aligned_buffer = (void *)((uintptr_t)((char *)&data + NVMF_DATA_BUFFER_MASK) &
				  ~NVMF_DATA_BUFFER_MASK);

	/* part 1: 2 segments each with 1 wr. io_unit_size is aligned with data_bs + md_size */
	reset_nvmf_rdma_request(&rdma_req);
	spdk_dif_ctx_init(&rdma_req.req.dif.dif_ctx, data_bs + md_size, md_size, true, false,
			  SPDK_DIF_TYPE1,
			  SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_REFTAG_CHECK, 0, 0, 0, 0, 0);
	rdma_req.req.dif.dif_insert_or_strip = true;
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
	CU_ASSERT(rdma_req.req.data_from_pool == true);
	CU_ASSERT(rdma_req.req.length == data_bs * 4 * 2);
	CU_ASSERT(rdma_req.req.dif.orig_length == rdma_req.req.length);
	CU_ASSERT(rdma_req.req.dif.elba_length == (data_bs + md_size) * 4 * 2);
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

	if (!CU_add_test(suite, "test_parse_sgl", test_spdk_nvmf_rdma_request_parse_sgl) ||
	    !CU_add_test(suite, "test_request_process", test_spdk_nvmf_rdma_request_process) ||
	    !CU_add_test(suite, "test_optimal_pg", test_spdk_nvmf_rdma_get_optimal_poll_group) ||
	    !CU_add_test(suite, "test_parse_sgl_with_md", test_spdk_nvmf_rdma_request_parse_sgl_with_md)) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
