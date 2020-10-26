/*
 *   BSD LICENSE
 *
 *   Copyright (c) 2018-2019 Broadcom.  All Rights Reserved.
 *   The term "Broadcom" refers to Broadcom Inc. and/or its subsidiaries.
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

/* NVMF FC Transport Unit Test */

#include "spdk/env.h"
#include "spdk_cunit.h"
#include "spdk/nvmf.h"
#include "spdk/endian.h"
#include "spdk/trace.h"
#include "spdk/log.h"

#include "ut_multithread.c"

#include "transport.h"
#include "nvmf_internal.h"

#include "nvmf_fc.h"

#include "json/json_util.c"
#include "json/json_write.c"
#include "nvmf/nvmf.c"
#include "nvmf/transport.c"
#include "nvmf/subsystem.c"
#include "nvmf/fc.c"
#include "nvmf/fc_ls.c"

/*
 * SPDK Stuff
 */

#ifdef SPDK_CONFIG_RDMA
const struct spdk_nvmf_transport_ops spdk_nvmf_transport_rdma = {
	.type = SPDK_NVME_TRANSPORT_RDMA,
	.opts_init = NULL,
	.create = NULL,
	.destroy = NULL,

	.listen = NULL,
	.stop_listen = NULL,
	.accept = NULL,

	.listener_discover = NULL,

	.poll_group_create = NULL,
	.poll_group_destroy = NULL,
	.poll_group_add = NULL,
	.poll_group_poll = NULL,

	.req_free = NULL,
	.req_complete = NULL,

	.qpair_fini = NULL,
	.qpair_get_peer_trid = NULL,
	.qpair_get_local_trid = NULL,
	.qpair_get_listen_trid = NULL,
};
#endif

const struct spdk_nvmf_transport_ops spdk_nvmf_transport_tcp = {
	.type = SPDK_NVME_TRANSPORT_TCP,
};

struct spdk_trace_histories *g_trace_histories;

DEFINE_STUB_V(_spdk_trace_record, (uint64_t tsc, uint16_t tpoint_id, uint16_t poller_id,
				   uint32_t size, uint64_t object_id, uint64_t arg1));
DEFINE_STUB(spdk_nvme_transport_id_compare, int,
	    (const struct spdk_nvme_transport_id *trid1,
	     const struct spdk_nvme_transport_id *trid2), 0);
DEFINE_STUB_V(spdk_trace_register_object, (uint8_t type, char id_prefix));
DEFINE_STUB_V(spdk_trace_register_description,
	      (const char *name, uint16_t tpoint_id, uint8_t owner_type,
	       uint8_t object_type, uint8_t new_object, uint8_t arg1_type,
	       const char *arg1_name));
DEFINE_STUB_V(spdk_trace_add_register_fn, (struct spdk_trace_register_fn *reg_fn));
DEFINE_STUB(spdk_bdev_get_name, const char *, (const struct spdk_bdev *bdev), "fc_ut_test");
DEFINE_STUB_V(nvmf_ctrlr_destruct, (struct spdk_nvmf_ctrlr *ctrlr));
DEFINE_STUB_V(nvmf_qpair_free_aer, (struct spdk_nvmf_qpair *qpair));
DEFINE_STUB(spdk_bdev_get_io_channel, struct spdk_io_channel *, (struct spdk_bdev_desc *desc),
	    NULL);
DEFINE_STUB_V(spdk_nvmf_request_exec, (struct spdk_nvmf_request *req));
DEFINE_STUB_V(nvmf_ctrlr_ns_changed, (struct spdk_nvmf_ctrlr *ctrlr, uint32_t nsid));
DEFINE_STUB(spdk_bdev_open, int, (struct spdk_bdev *bdev, bool write,
				  spdk_bdev_remove_cb_t remove_cb,
				  void *remove_ctx, struct spdk_bdev_desc **desc), 0);
DEFINE_STUB_V(spdk_bdev_close, (struct spdk_bdev_desc *desc));
DEFINE_STUB(spdk_bdev_module_claim_bdev, int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
	     struct spdk_bdev_module *module), 0);
DEFINE_STUB_V(spdk_bdev_module_release_bdev, (struct spdk_bdev *bdev));
DEFINE_STUB(spdk_bdev_get_block_size, uint32_t, (const struct spdk_bdev *bdev), 512);
DEFINE_STUB(spdk_bdev_get_num_blocks, uint64_t, (const struct spdk_bdev *bdev), 1024);

DEFINE_STUB(nvmf_ctrlr_async_event_ns_notice, int, (struct spdk_nvmf_ctrlr *ctrlr), 0);
DEFINE_STUB(nvmf_ctrlr_async_event_ana_change_notice, int,
	    (struct spdk_nvmf_ctrlr *ctrlr), 0);
DEFINE_STUB_V(spdk_nvme_trid_populate_transport, (struct spdk_nvme_transport_id *trid,
		enum spdk_nvme_transport_type trtype));
DEFINE_STUB_V(spdk_nvmf_ctrlr_data_init, (struct spdk_nvmf_transport_opts *opts,
		struct spdk_nvmf_ctrlr_data *cdata));
DEFINE_STUB(spdk_nvmf_request_complete, int, (struct spdk_nvmf_request *req),
	    -ENOSPC);

DEFINE_STUB_V(nvmf_update_discovery_log,
	      (struct spdk_nvmf_tgt *tgt, const char *hostnqn));

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

const char *
spdk_nvme_transport_id_adrfam_str(enum spdk_nvmf_adrfam adrfam)
{
	switch (adrfam) {
	case SPDK_NVMF_ADRFAM_IPV4:
		return "IPv4";
	case SPDK_NVMF_ADRFAM_IPV6:
		return "IPv6";
	case SPDK_NVMF_ADRFAM_IB:
		return "IB";
	case SPDK_NVMF_ADRFAM_FC:
		return "FC";
	default:
		return NULL;
	}
}

const struct spdk_uuid *
spdk_bdev_get_uuid(const struct spdk_bdev *bdev)
{
	return &bdev->uuid;
}

static bool g_lld_init_called = false;

int
nvmf_fc_lld_init(void)
{
	g_lld_init_called = true;
	return 0;
}

static bool g_lld_fini_called = false;

void
nvmf_fc_lld_fini(void)
{
	g_lld_fini_called = true;
}

DEFINE_STUB_V(nvmf_fc_lld_start, (void));
DEFINE_STUB(nvmf_fc_init_q, int, (struct spdk_nvmf_fc_hwqp *hwqp), 0);
DEFINE_STUB_V(nvmf_fc_reinit_q, (void *queues_prev, void *queues_curr));
DEFINE_STUB(nvmf_fc_init_rqpair_buffers, int, (struct spdk_nvmf_fc_hwqp *hwqp), 0);
DEFINE_STUB(nvmf_fc_set_q_online_state, int, (struct spdk_nvmf_fc_hwqp *hwqp, bool online), 0);
DEFINE_STUB(nvmf_fc_put_xchg, int, (struct spdk_nvmf_fc_hwqp *hwqp, struct spdk_nvmf_fc_xchg *xri),
	    0);
DEFINE_STUB(nvmf_fc_recv_data, int, (struct spdk_nvmf_fc_request *fc_req), 0);
DEFINE_STUB(nvmf_fc_send_data, int, (struct spdk_nvmf_fc_request *fc_req), 0);
DEFINE_STUB_V(nvmf_fc_rqpair_buffer_release, (struct spdk_nvmf_fc_hwqp *hwqp, uint16_t buff_idx));
DEFINE_STUB(nvmf_fc_xmt_rsp, int, (struct spdk_nvmf_fc_request *fc_req, uint8_t *ersp_buf,
				   uint32_t ersp_len), 0);
DEFINE_STUB(nvmf_fc_xmt_ls_rsp, int, (struct spdk_nvmf_fc_nport *tgtport,
				      struct spdk_nvmf_fc_ls_rqst *ls_rqst), 0);
DEFINE_STUB(nvmf_fc_issue_abort, int, (struct spdk_nvmf_fc_hwqp *hwqp,
				       struct spdk_nvmf_fc_xchg *xri,
				       spdk_nvmf_fc_caller_cb cb, void *cb_args), 0);
DEFINE_STUB(nvmf_fc_xmt_bls_rsp, int, (struct spdk_nvmf_fc_hwqp *hwqp,
				       uint16_t ox_id, uint16_t rx_id,
				       uint16_t rpi, bool rjt, uint8_t rjt_exp,
				       spdk_nvmf_fc_caller_cb cb, void *cb_args), 0);
DEFINE_STUB(nvmf_fc_alloc_srsr_bufs, struct spdk_nvmf_fc_srsr_bufs *, (size_t rqst_len,
		size_t rsp_len), NULL);
DEFINE_STUB_V(nvmf_fc_free_srsr_bufs, (struct spdk_nvmf_fc_srsr_bufs *srsr_bufs));
DEFINE_STUB(nvmf_fc_xmt_srsr_req, int, (struct spdk_nvmf_fc_hwqp *hwqp,
					struct spdk_nvmf_fc_srsr_bufs *xmt_srsr_bufs,
					spdk_nvmf_fc_caller_cb cb, void *cb_args), 0);
DEFINE_STUB(nvmf_fc_q_sync_available, bool, (void), true);
DEFINE_STUB(nvmf_fc_issue_q_sync, int, (struct spdk_nvmf_fc_hwqp *hwqp, uint64_t u_id,
					uint16_t skip_rq), 0);
DEFINE_STUB(nvmf_fc_assign_conn_to_hwqp, bool, (struct spdk_nvmf_fc_hwqp *hwqp,
		uint64_t *conn_id, uint32_t sq_size), true);
DEFINE_STUB(nvmf_fc_get_hwqp_from_conn_id, struct spdk_nvmf_fc_hwqp *,
	    (struct spdk_nvmf_fc_hwqp *queues,
	     uint32_t num_queues, uint64_t conn_id), NULL);
DEFINE_STUB_V(nvmf_fc_release_conn, (struct spdk_nvmf_fc_hwqp *hwqp, uint64_t conn_id,
				     uint32_t sq_size));
DEFINE_STUB_V(nvmf_fc_dump_all_queues, (struct spdk_nvmf_fc_hwqp *ls_queue,
					struct spdk_nvmf_fc_hwqp *io_queues,
					uint32_t num_io_queues,
					struct spdk_nvmf_fc_queue_dump_info *dump_info));
DEFINE_STUB_V(nvmf_fc_get_xri_info, (struct spdk_nvmf_fc_hwqp *hwqp,
				     struct spdk_nvmf_fc_xchg_info *info));
DEFINE_STUB(nvmf_fc_get_rsvd_thread, struct spdk_thread *, (void), NULL);

uint32_t
nvmf_fc_process_queue(struct spdk_nvmf_fc_hwqp *hwqp)
{
	hwqp->lcore_id++;
	return 0; /* always return 0 or else it will poll forever */
}

struct spdk_nvmf_fc_xchg *
nvmf_fc_get_xri(struct spdk_nvmf_fc_hwqp *hwqp)
{
	static struct spdk_nvmf_fc_xchg xchg;

	xchg.xchg_id = 1;
	return &xchg;
}

#define MAX_FC_UT_POLL_THREADS 8
static struct spdk_nvmf_poll_group *g_poll_groups[MAX_FC_UT_POLL_THREADS] = {0};
#define MAX_FC_UT_HWQPS MAX_FC_UT_POLL_THREADS
static struct spdk_nvmf_tgt *g_nvmf_tgt = NULL;
static struct spdk_nvmf_transport *g_nvmf_tprt = NULL;
uint8_t g_fc_port_handle = 0xff;
struct spdk_nvmf_fc_hwqp lld_q[MAX_FC_UT_HWQPS];

static void
_add_transport_done(void *arg, int status)
{
	CU_ASSERT(status == 0);
}

static void
_add_transport_done_dup_err(void *arg, int status)
{
	CU_ASSERT(status == -EEXIST);
}

static void
create_transport_test(void)
{
	const struct spdk_nvmf_transport_ops *ops = NULL;
	struct spdk_nvmf_transport_opts opts = { 0 };
	struct spdk_nvmf_target_opts tgt_opts = {
		.name = "nvmf_test_tgt",
		.max_subsystems = 0
	};

	allocate_threads(8);
	set_thread(0);

	g_nvmf_tgt = spdk_nvmf_tgt_create(&tgt_opts);
	SPDK_CU_ASSERT_FATAL(g_nvmf_tgt != NULL);

	ops = nvmf_get_transport_ops(SPDK_NVME_TRANSPORT_NAME_FC);
	SPDK_CU_ASSERT_FATAL(ops != NULL);

	ops->opts_init(&opts);

	g_lld_init_called = false;
	g_nvmf_tprt = spdk_nvmf_transport_create("FC", &opts);
	SPDK_CU_ASSERT_FATAL(g_nvmf_tprt != NULL);

	CU_ASSERT(g_lld_init_called == true);
	CU_ASSERT(opts.max_queue_depth == g_nvmf_tprt->opts.max_queue_depth);
	CU_ASSERT(opts.max_qpairs_per_ctrlr == g_nvmf_tprt->opts.max_qpairs_per_ctrlr);
	CU_ASSERT(opts.in_capsule_data_size == g_nvmf_tprt->opts.in_capsule_data_size);
	CU_ASSERT(opts.max_io_size == g_nvmf_tprt->opts.max_io_size);
	CU_ASSERT(opts.io_unit_size == g_nvmf_tprt->opts.io_unit_size);
	CU_ASSERT(opts.max_aq_depth == g_nvmf_tprt->opts.max_aq_depth);

	set_thread(0);

	spdk_nvmf_tgt_add_transport(g_nvmf_tgt, g_nvmf_tprt,
				    _add_transport_done, 0);
	poll_thread(0);

	/* Add transport again - should get error */
	spdk_nvmf_tgt_add_transport(g_nvmf_tgt, g_nvmf_tprt,
				    _add_transport_done_dup_err, 0);
	poll_thread(0);

	/* create transport with bad args/options */
#ifndef SPDK_CONFIG_RDMA
	CU_ASSERT(spdk_nvmf_transport_create("RDMA", &opts) == NULL);
#endif
	CU_ASSERT(spdk_nvmf_transport_create("Bogus Transport", &opts) == NULL);
	opts.max_io_size = 1024 ^ 3;
	CU_ASSERT(spdk_nvmf_transport_create("FC", &opts) == NULL);
	opts.max_io_size = 999;
	opts.io_unit_size = 1024;
	CU_ASSERT(spdk_nvmf_transport_create("FC", &opts) == NULL);
}

static void
port_init_cb(uint8_t port_handle, enum spdk_fc_event event_type, void *arg, int err)
{
	CU_ASSERT(err == 0);
	CU_ASSERT(port_handle == 2);
	g_fc_port_handle = port_handle;
}

static void
create_fc_port_test(void)
{
	struct spdk_nvmf_fc_hw_port_init_args init_args = { 0 };
	struct spdk_nvmf_fc_port *fc_port = NULL;
	int err;

	SPDK_CU_ASSERT_FATAL(g_nvmf_tprt != NULL);

	init_args.port_handle = 2;
	init_args.io_queue_cnt = spdk_min(MAX_FC_UT_HWQPS, spdk_env_get_core_count());
	init_args.ls_queue_size = 100;
	init_args.io_queue_size = 100;
	init_args.io_queues = (void *)lld_q;

	set_thread(0);
	err = nvmf_fc_master_enqueue_event(SPDK_FC_HW_PORT_INIT, (void *)&init_args, port_init_cb);
	CU_ASSERT(err == 0);
	poll_thread(0);

	fc_port = nvmf_fc_port_lookup(g_fc_port_handle);
	CU_ASSERT(fc_port != NULL);
}

static void
online_fc_port_test(void)
{
	struct spdk_nvmf_fc_port *fc_port;
	struct spdk_nvmf_fc_hw_port_online_args args;
	int err;

	SPDK_CU_ASSERT_FATAL(g_nvmf_tprt != NULL);

	fc_port = nvmf_fc_port_lookup(g_fc_port_handle);
	SPDK_CU_ASSERT_FATAL(fc_port != NULL);

	set_thread(0);
	args.port_handle = g_fc_port_handle;
	err = nvmf_fc_master_enqueue_event(SPDK_FC_HW_PORT_ONLINE, (void *)&args, port_init_cb);
	CU_ASSERT(err == 0);
	poll_threads();
	set_thread(0);
	if (err == 0) {
		uint32_t i;
		for (i = 0; i < fc_port->num_io_queues; i++) {
			CU_ASSERT(fc_port->io_queues[i].fgroup != 0);
			CU_ASSERT(fc_port->io_queues[i].fgroup != 0);
			CU_ASSERT(fc_port->io_queues[i].fgroup->hwqp_count != 0);
		}
	}
}

static void
create_poll_groups_test(void)
{
	unsigned i;

	SPDK_CU_ASSERT_FATAL(g_nvmf_tprt != NULL);

	for (i = 0; i < MAX_FC_UT_POLL_THREADS; i++) {
		set_thread(i);
		g_poll_groups[i] = spdk_nvmf_poll_group_create(g_nvmf_tgt);
		poll_thread(i);
		CU_ASSERT(g_poll_groups[i] != NULL);
	}
	set_thread(0);
}

static void
poll_group_poll_test(void)
{
	unsigned i;
	unsigned poll_cnt =  10;
	struct spdk_nvmf_fc_port *fc_port = NULL;

	SPDK_CU_ASSERT_FATAL(g_nvmf_tprt != NULL);

	set_thread(0);
	fc_port = nvmf_fc_port_lookup(g_fc_port_handle);
	SPDK_CU_ASSERT_FATAL(fc_port != NULL);

	for (i = 0; i < fc_port->num_io_queues; i++) {
		fc_port->io_queues[i].lcore_id = 0;
	}

	for (i = 0; i < poll_cnt; i++) {
		/* this should cause spdk_nvmf_fc_poll_group_poll to be called() */
		poll_threads();
	}

	/* check if hwqp's lcore_id has been updated */
	for (i = 0; i < fc_port->num_io_queues; i++) {
		CU_ASSERT(fc_port->io_queues[i].lcore_id == poll_cnt);
	}
}

static void
remove_hwqps_from_poll_groups_test(void)
{
	unsigned i;
	struct spdk_nvmf_fc_port *fc_port = NULL;

	SPDK_CU_ASSERT_FATAL(g_nvmf_tprt != NULL);

	fc_port = nvmf_fc_port_lookup(g_fc_port_handle);
	SPDK_CU_ASSERT_FATAL(fc_port != NULL);

	for (i = 0; i < fc_port->num_io_queues; i++) {
		nvmf_fc_poll_group_remove_hwqp(&fc_port->io_queues[i]);
		poll_threads();
		CU_ASSERT(fc_port->io_queues[i].fgroup == 0);
	}
}

static void
destroy_transport_test(void)
{
	unsigned i;

	SPDK_CU_ASSERT_FATAL(g_nvmf_tprt != NULL);

	for (i = 0; i < MAX_FC_UT_POLL_THREADS; i++) {
		set_thread(i);
		spdk_nvmf_poll_group_destroy(g_poll_groups[i], NULL, NULL);
		poll_thread(0);
	}

	set_thread(0);
	SPDK_CU_ASSERT_FATAL(g_nvmf_tgt != NULL);
	g_lld_fini_called = false;
	spdk_nvmf_tgt_destroy(g_nvmf_tgt, NULL, NULL);
	poll_threads();
	CU_ASSERT(g_lld_fini_called == true);
}

static int
nvmf_fc_tests_init(void)
{
	return 0;
}

static int
nvmf_fc_tests_fini(void)
{
	free_threads();
	return 0;
}

int main(int argc, char **argv)
{
	unsigned int num_failures = 0;
	CU_pSuite suite = NULL;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("NVMf-FC", nvmf_fc_tests_init, nvmf_fc_tests_fini);

	CU_ADD_TEST(suite, create_transport_test);
	CU_ADD_TEST(suite, create_poll_groups_test);
	CU_ADD_TEST(suite, create_fc_port_test);
	CU_ADD_TEST(suite, online_fc_port_test);
	CU_ADD_TEST(suite, poll_group_poll_test);
	CU_ADD_TEST(suite, remove_hwqps_from_poll_groups_test);
	CU_ADD_TEST(suite, destroy_transport_test);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
