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
#include "spdk/nvmf_spec.h"
#include "spdk_cunit.h"

#include "spdk_internal/mock.h"
#include "spdk_internal/thread.h"

#include "common/lib/test_env.c"
#include "common/lib/test_sock.c"

#include "nvmf/ctrlr.c"
#include "nvmf/tcp.c"

#define UT_IPV4_ADDR "192.168.0.1"
#define UT_PORT "4420"
#define UT_NVMF_ADRFAM_INVALID 0xf
#define UT_MAX_QUEUE_DEPTH 128
#define UT_MAX_QPAIRS_PER_CTRLR 128
#define UT_IN_CAPSULE_DATA_SIZE 1024
#define UT_MAX_IO_SIZE 4096
#define UT_IO_UNIT_SIZE 1024
#define UT_MAX_AQ_DEPTH 64
#define UT_SQ_HEAD_MAX 128
#define UT_NUM_SHARED_BUFFERS 128

SPDK_LOG_REGISTER_COMPONENT("nvmf", SPDK_LOG_NVMF)
SPDK_LOG_REGISTER_COMPONENT("nvme", SPDK_LOG_NVME)

DEFINE_STUB(spdk_nvmf_qpair_get_listen_trid,
	    int,
	    (struct spdk_nvmf_qpair *qpair, struct spdk_nvme_transport_id *trid),
	    0);

DEFINE_STUB(spdk_nvmf_subsystem_add_ctrlr,
	    int,
	    (struct spdk_nvmf_subsystem *subsystem, struct spdk_nvmf_ctrlr *ctrlr),
	    0);

DEFINE_STUB(spdk_nvmf_subsystem_get_ctrlr,
	    struct spdk_nvmf_ctrlr *,
	    (struct spdk_nvmf_subsystem *subsystem, uint16_t cntlid),
	    NULL);

DEFINE_STUB(spdk_nvmf_tgt_find_subsystem,
	    struct spdk_nvmf_subsystem *,
	    (struct spdk_nvmf_tgt *tgt, const char *subnqn),
	    NULL);

DEFINE_STUB(spdk_nvmf_subsystem_listener_allowed,
	    bool,
	    (struct spdk_nvmf_subsystem *subsystem, struct spdk_nvme_transport_id *trid),
	    true);

DEFINE_STUB(spdk_nvmf_transport_qpair_set_sqsize,
	    int,
	    (struct spdk_nvmf_qpair *qpair),
	    0);

DEFINE_STUB_V(spdk_nvmf_get_discovery_log_page,
	      (struct spdk_nvmf_tgt *tgt, const char *hostnqn, struct iovec *iov,
	       uint32_t iovcnt, uint64_t offset, uint32_t length));

DEFINE_STUB_V(spdk_nvmf_subsystem_remove_ctrlr,
	      (struct spdk_nvmf_subsystem *subsystem, struct spdk_nvmf_ctrlr *ctrlr));

DEFINE_STUB(spdk_nvmf_subsystem_get_first_ns,
	    struct spdk_nvmf_ns *,
	    (struct spdk_nvmf_subsystem *subsystem),
	    NULL);

DEFINE_STUB(spdk_nvmf_subsystem_get_next_ns,
	    struct spdk_nvmf_ns *,
	    (struct spdk_nvmf_subsystem *subsystem, struct spdk_nvmf_ns *prev_ns),
	    NULL);

DEFINE_STUB(spdk_nvmf_subsystem_host_allowed,
	    bool,
	    (struct spdk_nvmf_subsystem *subsystem, const char *hostnqn),
	    true);

DEFINE_STUB(spdk_nvmf_ctrlr_dsm_supported,
	    bool,
	    (struct spdk_nvmf_ctrlr *ctrlr),
	    false);

DEFINE_STUB(spdk_nvmf_ctrlr_write_zeroes_supported,
	    bool,
	    (struct spdk_nvmf_ctrlr *ctrlr),
	    false);

DEFINE_STUB(spdk_nvmf_bdev_ctrlr_read_cmd,
	    int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     struct spdk_nvmf_request *req),
	    0);

DEFINE_STUB(spdk_nvmf_bdev_ctrlr_write_cmd,
	    int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     struct spdk_nvmf_request *req),
	    0);

DEFINE_STUB(spdk_nvmf_bdev_ctrlr_write_zeroes_cmd,
	    int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     struct spdk_nvmf_request *req),
	    0);

DEFINE_STUB(spdk_nvmf_bdev_ctrlr_flush_cmd,
	    int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     struct spdk_nvmf_request *req),
	    0);

DEFINE_STUB(spdk_nvmf_bdev_ctrlr_dsm_cmd,
	    int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     struct spdk_nvmf_request *req),
	    0);

DEFINE_STUB(spdk_nvmf_bdev_ctrlr_nvme_passthru_io,
	    int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     struct spdk_nvmf_request *req),
	    0);

DEFINE_STUB(spdk_nvmf_bdev_ctrlr_get_dif_ctx,
	    bool,
	    (struct spdk_bdev *bdev, struct spdk_nvme_cmd *cmd, struct spdk_dif_ctx *dif_ctx),
	    false);

DEFINE_STUB(spdk_nvmf_transport_req_complete,
	    int,
	    (struct spdk_nvmf_request *req),
	    0);

DEFINE_STUB_V(spdk_nvmf_request_free_buffers,
	      (struct spdk_nvmf_request *req, struct spdk_nvmf_transport_poll_group *group,
	       struct spdk_nvmf_transport *transport));

DEFINE_STUB(spdk_sock_get_optimal_sock_group,
	    int,
	    (struct spdk_sock *sock, struct spdk_sock_group **group),
	    0);

DEFINE_STUB(spdk_sock_group_get_ctx,
	    void *,
	    (struct spdk_sock_group *group),
	    NULL);

DEFINE_STUB(spdk_sock_set_priority,
	    int,
	    (struct spdk_sock *sock, int priority),
	    0);

DEFINE_STUB_V(spdk_nvmf_ns_reservation_request, (void *ctx));

struct spdk_trace_histories *g_trace_histories;

struct spdk_bdev {
	int ut_mock;
	uint64_t blockcnt;
};

int
spdk_nvme_transport_id_compare(const struct spdk_nvme_transport_id *trid1,
			       const struct spdk_nvme_transport_id *trid2)
{
	return 0;
}

void
spdk_trace_register_object(uint8_t type, char id_prefix)
{
}

void
spdk_trace_register_description(const char *name,
				uint16_t tpoint_id, uint8_t owner_type,
				uint8_t object_type, uint8_t new_object,
				uint8_t arg1_type, const char *arg1_name)
{
}

void
_spdk_trace_record(uint64_t tsc, uint16_t tpoint_id, uint16_t poller_id,
		   uint32_t size, uint64_t object_id, uint64_t arg1)
{
}

int
spdk_nvmf_qpair_disconnect(struct spdk_nvmf_qpair *qpair, nvmf_qpair_disconnect_cb cb_fn, void *ctx)
{
	return 0;
}


int
spdk_nvmf_request_get_buffers(struct spdk_nvmf_request *req,
			      struct spdk_nvmf_transport_poll_group *group,
			      struct spdk_nvmf_transport *transport,
			      uint32_t length)
{
	/* length more than 1 io unit length will fail. */
	if (length >= transport->opts.io_unit_size) {
		return -EINVAL;
	}

	req->iovcnt = 1;
	req->iov[0].iov_base = (void *)0xDEADBEEF;

	return 0;
}


void
spdk_nvmf_bdev_ctrlr_identify_ns(struct spdk_nvmf_ns *ns, struct spdk_nvme_ns_data *nsdata,
				 bool dif_insert_or_strip)
{
	uint64_t num_blocks;

	SPDK_CU_ASSERT_FATAL(ns->bdev != NULL);
	num_blocks = ns->bdev->blockcnt;
	nsdata->nsze = num_blocks;
	nsdata->ncap = num_blocks;
	nsdata->nuse = num_blocks;
	nsdata->nlbaf = 0;
	nsdata->flbas.format = 0;
	nsdata->lbaf[0].lbads = spdk_u32log2(512);
}

const char *
spdk_nvmf_subsystem_get_sn(const struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->sn;
}

const char *
spdk_nvmf_subsystem_get_mn(const struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->mn;
}

void
spdk_trace_add_register_fn(struct spdk_trace_register_fn *reg_fn)
{
}

static void
test_nvmf_tcp_create(void)
{
	struct spdk_thread *thread;
	struct spdk_nvmf_transport *transport;
	struct spdk_nvmf_tcp_transport *ttransport;
	struct spdk_nvmf_transport_opts opts;

	thread = spdk_thread_create(NULL, NULL);
	SPDK_CU_ASSERT_FATAL(thread != NULL);
	spdk_set_thread(thread);

	/* case 1 */
	memset(&opts, 0, sizeof(opts));
	opts.max_queue_depth = UT_MAX_QUEUE_DEPTH;
	opts.max_qpairs_per_ctrlr = UT_MAX_QPAIRS_PER_CTRLR;
	opts.in_capsule_data_size = UT_IN_CAPSULE_DATA_SIZE;
	opts.max_io_size = UT_MAX_IO_SIZE;
	opts.io_unit_size = UT_IO_UNIT_SIZE;
	opts.max_aq_depth = UT_MAX_AQ_DEPTH;
	opts.num_shared_buffers = UT_NUM_SHARED_BUFFERS;
	/* expect success */
	transport = spdk_nvmf_tcp_create(&opts);
	CU_ASSERT_PTR_NOT_NULL(transport);
	ttransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_tcp_transport, transport);
	SPDK_CU_ASSERT_FATAL(ttransport != NULL);
	transport->opts = opts;
	CU_ASSERT(transport->opts.max_queue_depth == UT_MAX_QUEUE_DEPTH);
	CU_ASSERT(transport->opts.max_io_size == UT_MAX_IO_SIZE);
	CU_ASSERT(transport->opts.in_capsule_data_size == UT_IN_CAPSULE_DATA_SIZE);
	CU_ASSERT(transport->opts.io_unit_size == UT_IO_UNIT_SIZE);
	/* destroy transport */
	spdk_mempool_free(ttransport->transport.data_buf_pool);
	free(ttransport);

	/* case 2 */
	memset(&opts, 0, sizeof(opts));
	opts.max_queue_depth = UT_MAX_QUEUE_DEPTH;
	opts.max_qpairs_per_ctrlr = UT_MAX_QPAIRS_PER_CTRLR;
	opts.in_capsule_data_size = UT_IN_CAPSULE_DATA_SIZE;
	opts.max_io_size = UT_MAX_IO_SIZE;
	opts.io_unit_size = UT_MAX_IO_SIZE + 1;
	opts.max_aq_depth = UT_MAX_AQ_DEPTH;
	opts.num_shared_buffers = UT_NUM_SHARED_BUFFERS;
	/* expect success */
	transport = spdk_nvmf_tcp_create(&opts);
	CU_ASSERT_PTR_NOT_NULL(transport);
	ttransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_tcp_transport, transport);
	SPDK_CU_ASSERT_FATAL(ttransport != NULL);
	transport->opts = opts;
	CU_ASSERT(transport->opts.max_queue_depth == UT_MAX_QUEUE_DEPTH);
	CU_ASSERT(transport->opts.max_io_size == UT_MAX_IO_SIZE);
	CU_ASSERT(transport->opts.in_capsule_data_size == UT_IN_CAPSULE_DATA_SIZE);
	CU_ASSERT(transport->opts.io_unit_size == UT_MAX_IO_SIZE);
	/* destroy transport */
	spdk_mempool_free(ttransport->transport.data_buf_pool);
	free(ttransport);

	/* case 3 */
	memset(&opts, 0, sizeof(opts));
	opts.max_queue_depth = UT_MAX_QUEUE_DEPTH;
	opts.max_qpairs_per_ctrlr = UT_MAX_QPAIRS_PER_CTRLR;
	opts.in_capsule_data_size = UT_IN_CAPSULE_DATA_SIZE;
	opts.max_io_size = UT_MAX_IO_SIZE;
	opts.io_unit_size = 16;
	opts.max_aq_depth = UT_MAX_AQ_DEPTH;
	/* expect failse */
	transport = spdk_nvmf_tcp_create(&opts);
	CU_ASSERT_PTR_NULL(transport);

	spdk_thread_exit(thread);
	spdk_thread_destroy(thread);
}

static void
test_nvmf_tcp_destroy(void)
{
	struct spdk_thread *thread;
	struct spdk_nvmf_transport *transport;
	struct spdk_nvmf_transport_opts opts;

	thread = spdk_thread_create(NULL, NULL);
	SPDK_CU_ASSERT_FATAL(thread != NULL);
	spdk_set_thread(thread);

	/* case 1 */
	memset(&opts, 0, sizeof(opts));
	opts.max_queue_depth = UT_MAX_QUEUE_DEPTH;
	opts.max_qpairs_per_ctrlr = UT_MAX_QPAIRS_PER_CTRLR;
	opts.in_capsule_data_size = UT_IN_CAPSULE_DATA_SIZE;
	opts.max_io_size = UT_MAX_IO_SIZE;
	opts.io_unit_size = UT_IO_UNIT_SIZE;
	opts.max_aq_depth = UT_MAX_AQ_DEPTH;
	opts.num_shared_buffers = UT_NUM_SHARED_BUFFERS;
	transport = spdk_nvmf_tcp_create(&opts);
	CU_ASSERT_PTR_NOT_NULL(transport);
	transport->opts = opts;
	/* destroy transport */
	CU_ASSERT(spdk_nvmf_tcp_destroy(transport) == 0);

	spdk_thread_exit(thread);
	spdk_thread_destroy(thread);
}

static void
test_nvmf_tcp_poll_group_create(void)
{
	struct spdk_nvmf_transport *transport;
	struct spdk_nvmf_transport_poll_group *group;
	struct spdk_thread *thread;
	struct spdk_nvmf_transport_opts opts;
	struct spdk_sock_group grp = {};

	thread = spdk_thread_create(NULL, NULL);
	SPDK_CU_ASSERT_FATAL(thread != NULL);
	spdk_set_thread(thread);

	memset(&opts, 0, sizeof(opts));
	opts.max_queue_depth = UT_MAX_QUEUE_DEPTH;
	opts.max_qpairs_per_ctrlr = UT_MAX_QPAIRS_PER_CTRLR;
	opts.in_capsule_data_size = UT_IN_CAPSULE_DATA_SIZE;
	opts.max_io_size = UT_MAX_IO_SIZE;
	opts.io_unit_size = UT_IO_UNIT_SIZE;
	opts.max_aq_depth = UT_MAX_AQ_DEPTH;
	opts.num_shared_buffers = UT_NUM_SHARED_BUFFERS;
	transport = spdk_nvmf_tcp_create(&opts);
	CU_ASSERT_PTR_NOT_NULL(transport);
	transport->opts = opts;
	MOCK_SET(spdk_sock_group_create, &grp);
	group = spdk_nvmf_tcp_poll_group_create(transport);
	MOCK_CLEAR_P(spdk_sock_group_create);
	SPDK_CU_ASSERT_FATAL(group);
	group->transport = transport;
	spdk_nvmf_tcp_poll_group_destroy(group);
	spdk_nvmf_tcp_destroy(transport);

	spdk_thread_exit(thread);
	spdk_thread_destroy(thread);
}

static void
test_nvmf_tcp_send_c2h_data(void)
{
	struct spdk_thread *thread;
	struct spdk_nvmf_tcp_transport ttransport = {};
	struct spdk_nvmf_tcp_qpair tqpair = {};
	struct spdk_nvmf_tcp_req tcp_req = {};
	struct nvme_tcp_pdu pdu = {};
	struct spdk_nvme_tcp_c2h_data_hdr *c2h_data;

	pdu.hdr = &pdu.hdr_mem;
	thread = spdk_thread_create(NULL, NULL);
	SPDK_CU_ASSERT_FATAL(thread != NULL);
	spdk_set_thread(thread);

	tqpair.qpair.transport = &ttransport.transport;
	TAILQ_INIT(&tqpair.free_queue);
	TAILQ_INIT(&tqpair.send_queue);
	STAILQ_INIT(&tqpair.queued_c2h_data_tcp_req);

	/* Set qpair state to make unrelated operations NOP */
	tqpair.state = NVME_TCP_QPAIR_STATE_RUNNING;
	tqpair.recv_state = NVME_TCP_PDU_RECV_STATE_ERROR;

	TAILQ_INSERT_TAIL(&tqpair.free_queue, &pdu, tailq);
	tqpair.free_pdu_num++;

	tcp_req.req.cmd = (union nvmf_h2c_msg *)&tcp_req.cmd;

	tcp_req.req.iov[0].iov_base = (void *)0xDEADBEEF;
	tcp_req.req.iov[0].iov_len = NVMF_TCP_PDU_MAX_C2H_DATA_SIZE;
	tcp_req.req.iov[1].iov_base = (void *)0xFEEDBEEF;
	tcp_req.req.iov[1].iov_len = NVMF_TCP_PDU_MAX_C2H_DATA_SIZE;
	tcp_req.req.iov[2].iov_base = (void *)0xC0FFEE;
	tcp_req.req.iov[2].iov_len = NVMF_TCP_PDU_MAX_C2H_DATA_SIZE;
	tcp_req.req.iovcnt = 3;
	tcp_req.req.length = NVMF_TCP_PDU_MAX_C2H_DATA_SIZE * 3;

	CU_ASSERT(spdk_nvmf_tcp_calc_c2h_data_pdu_num(&tcp_req) == 3);

	STAILQ_INSERT_TAIL(&tqpair.queued_c2h_data_tcp_req, &tcp_req, link);

	tcp_req.c2h_data_offset = NVMF_TCP_PDU_MAX_C2H_DATA_SIZE / 2;

	/* 1st C2H */
	spdk_nvmf_tcp_send_c2h_data(&tqpair, &tcp_req);

	CU_ASSERT(TAILQ_FIRST(&tqpair.send_queue) == &pdu);
	TAILQ_REMOVE(&tqpair.send_queue, &pdu, tailq);
	TAILQ_INSERT_TAIL(&tqpair.free_queue, &pdu, tailq);
	tqpair.free_pdu_num++;

	c2h_data = &pdu.hdr->c2h_data;
	CU_ASSERT(c2h_data->datao == NVMF_TCP_PDU_MAX_C2H_DATA_SIZE / 2);
	CU_ASSERT(c2h_data->datal = NVMF_TCP_PDU_MAX_C2H_DATA_SIZE);
	CU_ASSERT(c2h_data->common.plen == sizeof(*c2h_data) + NVMF_TCP_PDU_MAX_C2H_DATA_SIZE);
	CU_ASSERT(!(c2h_data->common.flags & SPDK_NVME_TCP_C2H_DATA_FLAGS_LAST_PDU));

	CU_ASSERT(pdu.data_iovcnt == 2);
	CU_ASSERT((uint64_t)pdu.data_iov[0].iov_base == 0xDEADBEEF + NVMF_TCP_PDU_MAX_C2H_DATA_SIZE / 2);
	CU_ASSERT(pdu.data_iov[0].iov_len == NVMF_TCP_PDU_MAX_C2H_DATA_SIZE / 2);
	CU_ASSERT((uint64_t)pdu.data_iov[1].iov_base == 0xFEEDBEEF);
	CU_ASSERT(pdu.data_iov[1].iov_len == NVMF_TCP_PDU_MAX_C2H_DATA_SIZE / 2);

	CU_ASSERT(tcp_req.c2h_data_offset == (NVMF_TCP_PDU_MAX_C2H_DATA_SIZE / 2) * 3);
	CU_ASSERT(STAILQ_FIRST(&tqpair.queued_c2h_data_tcp_req) == &tcp_req);

	/* 2nd C2H */
	spdk_nvmf_tcp_send_c2h_data(&tqpair, &tcp_req);

	CU_ASSERT(TAILQ_FIRST(&tqpair.send_queue) == &pdu);
	TAILQ_REMOVE(&tqpair.send_queue, &pdu, tailq);
	TAILQ_INSERT_TAIL(&tqpair.free_queue, &pdu, tailq);
	tqpair.free_pdu_num++;

	c2h_data = &pdu.hdr->c2h_data;
	CU_ASSERT(c2h_data->datao == (NVMF_TCP_PDU_MAX_C2H_DATA_SIZE / 2) * 3);
	CU_ASSERT(c2h_data->datal = NVMF_TCP_PDU_MAX_C2H_DATA_SIZE);
	CU_ASSERT(c2h_data->common.plen == sizeof(*c2h_data) + NVMF_TCP_PDU_MAX_C2H_DATA_SIZE);
	CU_ASSERT(!(c2h_data->common.flags & SPDK_NVME_TCP_C2H_DATA_FLAGS_LAST_PDU));

	CU_ASSERT(pdu.data_iovcnt == 2);
	CU_ASSERT((uint64_t)pdu.data_iov[0].iov_base == 0xFEEDBEEF + NVMF_TCP_PDU_MAX_C2H_DATA_SIZE / 2);
	CU_ASSERT(pdu.data_iov[0].iov_len == NVMF_TCP_PDU_MAX_C2H_DATA_SIZE / 2);
	CU_ASSERT((uint64_t)pdu.data_iov[1].iov_base == 0xC0FFEE);
	CU_ASSERT(pdu.data_iov[1].iov_len == NVMF_TCP_PDU_MAX_C2H_DATA_SIZE / 2);

	CU_ASSERT(tcp_req.c2h_data_offset == (NVMF_TCP_PDU_MAX_C2H_DATA_SIZE / 2) * 5);
	CU_ASSERT(STAILQ_FIRST(&tqpair.queued_c2h_data_tcp_req) == &tcp_req);

	/* 3rd C2H */
	spdk_nvmf_tcp_send_c2h_data(&tqpair, &tcp_req);

	CU_ASSERT(TAILQ_FIRST(&tqpair.send_queue) == &pdu);
	TAILQ_REMOVE(&tqpair.send_queue, &pdu, tailq);
	CU_ASSERT(TAILQ_EMPTY(&tqpair.send_queue));

	c2h_data = &pdu.hdr->c2h_data;
	CU_ASSERT(c2h_data->datao == (NVMF_TCP_PDU_MAX_C2H_DATA_SIZE / 2) * 5);
	CU_ASSERT(c2h_data->datal = NVMF_TCP_PDU_MAX_C2H_DATA_SIZE / 2);
	CU_ASSERT(c2h_data->common.plen == sizeof(*c2h_data) + NVMF_TCP_PDU_MAX_C2H_DATA_SIZE / 2);
	CU_ASSERT(c2h_data->common.flags & SPDK_NVME_TCP_C2H_DATA_FLAGS_LAST_PDU);

	CU_ASSERT(pdu.data_iovcnt == 1);
	CU_ASSERT((uint64_t)pdu.data_iov[0].iov_base == 0xC0FFEE + NVMF_TCP_PDU_MAX_C2H_DATA_SIZE / 2);
	CU_ASSERT(pdu.data_iov[0].iov_len == NVMF_TCP_PDU_MAX_C2H_DATA_SIZE / 2);

	CU_ASSERT(tcp_req.c2h_data_offset == NVMF_TCP_PDU_MAX_C2H_DATA_SIZE * 3);
	CU_ASSERT(tqpair.c2h_data_pdu_cnt == 3);
	CU_ASSERT(STAILQ_EMPTY(&tqpair.queued_c2h_data_tcp_req));

	spdk_thread_exit(thread);
	spdk_thread_destroy(thread);
}

static void
test_nvmf_tcp_h2c_data_hdr_handle(void)
{
	struct spdk_nvmf_tcp_transport ttransport = {};
	struct spdk_nvmf_tcp_qpair tqpair = {};
	struct nvme_tcp_pdu pdu = {};
	struct spdk_nvmf_tcp_req tcp_req = {};
	struct spdk_nvme_tcp_h2c_data_hdr *h2c_data;

	pdu.hdr = &pdu.hdr_mem;
	TAILQ_INIT(&tqpair.state_queue[TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER]);
	tqpair.maxh2cdata = NVMF_TCP_PDU_MAX_H2C_DATA_SIZE;

	/* Set qpair state to make unrelated operations NOP */
	tqpair.state = NVME_TCP_QPAIR_STATE_RUNNING;
	tqpair.recv_state = NVME_TCP_PDU_RECV_STATE_ERROR;

	tcp_req.req.iov[0].iov_base = (void *)0xDEADBEEF;
	tcp_req.req.iov[0].iov_len = (NVMF_TCP_PDU_MAX_H2C_DATA_SIZE / 2) * 5;
	tcp_req.req.iov[1].iov_base = (void *)0xFEEDBEEF;
	tcp_req.req.iov[1].iov_len = NVMF_TCP_PDU_MAX_H2C_DATA_SIZE / 2;
	tcp_req.req.iovcnt = 2;
	tcp_req.req.length = NVMF_TCP_PDU_MAX_H2C_DATA_SIZE * 3;

	tcp_req.req.cmd = (union nvmf_h2c_msg *)&tcp_req.cmd;
	tcp_req.req.cmd->nvme_cmd.cid = 1;
	tcp_req.req.length = NVMF_TCP_PDU_MAX_H2C_DATA_SIZE * 3;
	tcp_req.ttag = 2;
	tcp_req.next_expected_r2t_offset = NVMF_TCP_PDU_MAX_H2C_DATA_SIZE * 2;

	TAILQ_INSERT_TAIL(&tqpair.state_queue[TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER],
			  &tcp_req, state_link);

	h2c_data = &pdu.hdr->h2c_data;
	h2c_data->cccid = 1;
	h2c_data->ttag = 2;
	h2c_data->datao = NVMF_TCP_PDU_MAX_H2C_DATA_SIZE * 2;
	h2c_data->datal = NVMF_TCP_PDU_MAX_H2C_DATA_SIZE;

	spdk_nvmf_tcp_h2c_data_hdr_handle(&ttransport, &tqpair, &pdu);

	CU_ASSERT(pdu.data_iovcnt == 2);
	CU_ASSERT((uint64_t)pdu.data_iov[0].iov_base == 0xDEADBEEF + NVMF_TCP_PDU_MAX_H2C_DATA_SIZE * 2);
	CU_ASSERT(pdu.data_iov[0].iov_len == NVMF_TCP_PDU_MAX_H2C_DATA_SIZE / 2);
	CU_ASSERT((uint64_t)pdu.data_iov[1].iov_base == 0xFEEDBEEF);
	CU_ASSERT(pdu.data_iov[1].iov_len == NVMF_TCP_PDU_MAX_H2C_DATA_SIZE / 2);

	CU_ASSERT(TAILQ_FIRST(&tqpair.state_queue[TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER]) ==
		  &tcp_req);
	TAILQ_REMOVE(&tqpair.state_queue[TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER],
		     &tcp_req, state_link);
}


static void
test_nvmf_tcp_incapsule_data_handle(void)
{
	struct spdk_nvmf_tcp_transport ttransport = {};
	struct spdk_nvmf_tcp_qpair tqpair = {};
	struct nvme_tcp_pdu pdu = {};
	union nvmf_c2h_msg rsp0 = {};
	union nvmf_c2h_msg rsp = {};

	struct spdk_nvmf_request *req_temp = NULL;
	struct spdk_nvmf_tcp_req tcp_req2 = {};
	struct spdk_nvmf_tcp_req tcp_req1 = {};
	union nvme_tcp_pdu_hdr hdr = {};

	struct spdk_nvme_tcp_cmd *capsule_data;
	struct spdk_nvmf_capsule_cmd *nvmf_capsule_data;
	struct spdk_nvme_sgl_descriptor *sgl;

	struct spdk_nvmf_transport_poll_group *group;
	struct spdk_nvmf_tcp_poll_group tcp_group = {};
	struct spdk_sock_group grp = {};
	int i = 0;

	ttransport.transport.opts.max_io_size = UT_MAX_IO_SIZE;
	ttransport.transport.opts.io_unit_size = UT_IO_UNIT_SIZE;

	tcp_group.sock_group = &grp;
	TAILQ_INIT(&tcp_group.qpairs);
	group = &tcp_group.group;
	group->transport = &ttransport.transport;
	STAILQ_INIT(&group->pending_buf_queue);
	tqpair.group = &tcp_group;

	/* init tqpair, add pdu to pdu_in_progress and wait for the buff */
	pdu.hdr = &pdu.hdr_mem;
	for (i = TCP_REQUEST_STATE_FREE; i < TCP_REQUEST_NUM_STATES; i++) {
		TAILQ_INIT(&tqpair.state_queue[i]);
	}
	TAILQ_INIT(&tqpair.free_queue);
	TAILQ_INIT(&tqpair.send_queue);
	STAILQ_INIT(&tqpair.queued_c2h_data_tcp_req);

	TAILQ_INSERT_TAIL(&tqpair.state_queue[TCP_REQUEST_STATE_FREE], &tcp_req2, state_link);
	tqpair.state_cntr[TCP_REQUEST_STATE_FREE]++;
	tqpair.qpair.transport = &ttransport.transport;
	tqpair.pdu_in_progress.hdr = &hdr;
	tqpair.state = NVME_TCP_QPAIR_STATE_RUNNING;
	tqpair.recv_state = NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH;

	/* init a null tcp_req into tqpair TCP_REQUEST_STATE_FREE queue */
	tcp_req2.req.qpair = &tqpair.qpair;
	tcp_req2.req.cmd = (union nvmf_h2c_msg *)&tcp_req2.cmd;
	tcp_req2.req.rsp = &rsp;

	/* init tcp_req1 */
	tcp_req1.req.qpair = &tqpair.qpair;
	tcp_req1.req.cmd = (union nvmf_h2c_msg *)&tcp_req1.cmd;
	tcp_req1.req.rsp = &rsp0;
	tcp_req1.state = TCP_REQUEST_STATE_NEW;

	TAILQ_INSERT_TAIL(&tqpair.state_queue[TCP_REQUEST_STATE_NEW], &tcp_req1, state_link);
	tqpair.state_cntr[TCP_REQUEST_STATE_NEW]++;

	/* init pdu, make pdu need sgl buff */
	capsule_data = &pdu.hdr->capsule_cmd;
	nvmf_capsule_data = (struct spdk_nvmf_capsule_cmd *)&pdu.hdr->capsule_cmd.ccsqe;
	sgl = &capsule_data->ccsqe.dptr.sgl1;

	capsule_data->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD;
	capsule_data->common.hlen = sizeof(*capsule_data);
	capsule_data->common.plen = 1096;
	capsule_data->ccsqe.opc = SPDK_NVME_OPC_FABRIC;

	sgl->unkeyed.subtype = SPDK_NVME_SGL_SUBTYPE_TRANSPORT;
	sgl->generic.type = SPDK_NVME_SGL_TYPE_TRANSPORT_DATA_BLOCK;
	sgl = &pdu.hdr->capsule_cmd.ccsqe.dptr.sgl1;
	sgl->unkeyed.length = UT_IO_UNIT_SIZE;

	nvmf_capsule_data->fctype = SPDK_NVMF_FABRIC_COMMAND_CONNECT;
	tqpair.pdu_in_progress = pdu;

	/* insert tcp_req1 to pending_buf_queue, And this req takes precedence over the next req. */
	spdk_nvmf_tcp_req_process(&ttransport, &tcp_req1);
	CU_ASSERT(STAILQ_FIRST(&group->pending_buf_queue) == &tcp_req1.req);

	sgl->unkeyed.length = UT_IO_UNIT_SIZE - 1;

	/* process tqpair capsule req. but we still remain req in pending_buff. */
	spdk_nvmf_tcp_capsule_cmd_hdr_handle(&ttransport, &tqpair, &tqpair.pdu_in_progress);
	CU_ASSERT(tqpair.recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD);
	CU_ASSERT(STAILQ_FIRST(&group->pending_buf_queue) == &tcp_req1.req);
	STAILQ_FOREACH(req_temp, &group->pending_buf_queue, buf_link) {
		if (req_temp == &tcp_req2.req) {
			break;
		}
	}
	CU_ASSERT(req_temp == NULL);
	CU_ASSERT(tqpair.pdu_in_progress.req == (void *)&tcp_req2);
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
		CU_add_test(suite, "nvmf_tcp_create", test_nvmf_tcp_create) == NULL ||
		CU_add_test(suite, "nvmf_tcp_destroy", test_nvmf_tcp_destroy) == NULL ||
		CU_add_test(suite, "nvmf_tcp_poll_group_create", test_nvmf_tcp_poll_group_create) == NULL ||
		CU_add_test(suite, "nvmf_tcp_send_c2h_data", test_nvmf_tcp_send_c2h_data) == NULL ||
		CU_add_test(suite, "nvmf_tcp_h2c_data_hdr_handle", test_nvmf_tcp_h2c_data_hdr_handle) == NULL	||
		CU_add_test(suite, "nvmf_tcp_incapsule_test", test_nvmf_tcp_incapsule_data_handle) == NULL
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
