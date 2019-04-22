/*
 *   BSD LICENSE
 *
 *   Copyright (c) 2018 Broadcom.  All Rights Reserved.
 *   The term "Broadcom" refers to Broadcom Limited and/or its subsidiaries.
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
#include "spdk_internal/event.h"
#include "spdk/endian.h"
#include "spdk/trace.h"
#include "spdk_internal/log.h"

#include "ut_multithread.c"

#include "transport.h"
#include "nvmf_internal.h"
#include "nvmf_fc.h"
#include "spdk/fc_adm_api.h"

#include "json/json_util.c"
#include "json/json_write.c"
#include "nvmf/nvmf.c"
#include "nvmf/transport.c"
#include "nvmf/subsystem.c"
#include "nvmf/fc.c"
#include "nvmf/fc_ls.c"
#include "nvmf/fc_poller_api.c"
#include "nvmf/fc_adm_api/fc_adm_api.c"

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
	.qpair_is_idle = NULL,
	.qpair_get_peer_trid = NULL,
	.qpair_get_local_trid = NULL,
	.qpair_get_listen_trid = NULL,
};
#endif

const struct spdk_nvmf_transport_ops spdk_nvmf_transport_tcp = {
	.type = SPDK_NVME_TRANSPORT_TCP,
};

struct rte_mempool *
rte_mempool_create(const char *name, unsigned n, unsigned elt_size,
		   unsigned cache_size, unsigned private_data_size,
		   rte_mempool_ctor_t *mp_init, void *mp_init_arg,
		   rte_mempool_obj_cb_t *obj_init, void *obj_init_arg,
		   int socket_id, unsigned flags)
{
	/* call spdk_mempool_create in test_env.c */
	return (struct rte_mempool *)spdk_mempool_create(name, n, elt_size, cache_size, socket_id);
}

struct spdk_trace_histories *g_trace_histories;
void _spdk_trace_record(uint64_t tsc, uint16_t tpoint_id, uint16_t poller_id,
			uint32_t size, uint64_t object_id, uint64_t arg1)
{
}

uint32_t
spdk_env_get_current_core(void)
{
	return 0;
}

uint32_t
spdk_env_get_last_core(void)
{
	return (uint32_t) - 1;
}

uint32_t
spdk_env_get_core_count(void)
{
	return 4;
}

int
spdk_nvme_transport_id_compare(const struct spdk_nvme_transport_id *trid1,
			       const struct spdk_nvme_transport_id *trid2)
{
	return 0;
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

int
spdk_nvmf_request_abort(struct spdk_nvmf_request *req)
{
	return -1;
}

const char *
spdk_bdev_get_name(const struct spdk_bdev *bdev)
{
	return "fc_ut_test";
}

void
spdk_nvmf_ctrlr_destruct(struct spdk_nvmf_ctrlr *ctrlr)
{
}

void
spdk_nvmf_qpair_free_aer(struct spdk_nvmf_qpair *qpair)
{
}

struct spdk_io_channel *
spdk_bdev_get_io_channel(struct spdk_bdev_desc *desc)
{
	return NULL;
}

void
spdk_nvmf_request_exec(struct spdk_nvmf_request *req)
{
}

void
spdk_nvmf_ctrlr_ns_changed(struct spdk_nvmf_ctrlr *ctrlr, uint32_t nsid)
{
}

int
spdk_bdev_open(struct spdk_bdev *bdev, bool write, spdk_bdev_remove_cb_t remove_cb,
	       void *remove_ctx, struct spdk_bdev_desc **desc)
{
	return 0;
}

void
spdk_bdev_close(struct spdk_bdev_desc *desc)
{
}

int
spdk_bdev_module_claim_bdev(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			    struct spdk_bdev_module *module)
{
	return 0;
}

void
spdk_bdev_module_release_bdev(struct spdk_bdev *bdev)
{
}

const struct spdk_uuid *
spdk_bdev_get_uuid(const struct spdk_bdev *bdev)
{
	return &bdev->uuid;
}

uint32_t
spdk_bdev_get_block_size(const struct spdk_bdev *bdev)
{
	return 512;
}

static bool g_lld_init_called = false;

static int
lld_init(void)
{
	g_lld_init_called = true;
	return 0;
}

static bool g_lld_fini_called = false;

static void
lld_fini(void)
{
	g_lld_fini_called = true;
}

static int
lld_init_q(struct spdk_nvmf_fc_hwqp *hwqp)
{
	return 0;
}

static int
lld_init_q_buffers(struct spdk_nvmf_fc_hwqp *hwqp)
{
	return 0;
}

static int
lld_set_q_online_state(struct spdk_nvmf_fc_hwqp *hwqp, bool online)
{
	return 0;
}

static uint32_t
lld_poll_queue(struct spdk_nvmf_fc_hwqp *hwqp)
{
	hwqp->lcore_id++;
	return 0; /* always return 0 or else it will poll forever */
}

static struct spdk_nvmf_fc_xchg *
lld_get_xchg(struct spdk_nvmf_fc_hwqp *hwqp)
{
	static struct spdk_nvmf_fc_xchg xchg;

	xchg.xchg_id = 1;
	return &xchg;
}

static struct spdk_thread *
lld_get_rsvd_thread(void)
{
	return NULL;
}

struct spdk_nvmf_fc_ll_drvr_ops spdk_nvmf_fc_lld_ops = {
	.lld_init = lld_init,
	.lld_fini = lld_fini,
	.lld_start = NULL,
	.init_q = lld_init_q,
	.reinit_q = NULL,
	.init_q_buffers = lld_init_q_buffers,
	.set_q_online_state = lld_set_q_online_state,
	.get_xchg = lld_get_xchg,
	.put_xchg = NULL,
	.poll_queue = lld_poll_queue,
	.recv_data = NULL,
	.send_data = NULL,
	.q_buffer_release = NULL,
	.xmt_rsp = NULL,
	.xmt_ls_rsp = NULL,
	.issue_abort = NULL,
	.xmt_bls_rsp = NULL,
	.xmt_srsr_req = NULL,
	.q_sync_available = NULL,
	.issue_q_sync = NULL,
	.assign_conn_to_hwqp = NULL,
	.get_hwqp_from_conn_id = NULL,
	.release_conn = NULL,
	.dump_all_queues = NULL,
	.get_xchg_info = NULL,
	.get_rsvd_thread = lld_get_rsvd_thread
};

#define MAX_FC_UT_POLL_THREADS 8
static struct spdk_nvmf_poll_group *g_poll_groups[MAX_FC_UT_POLL_THREADS] = {0};
#define MAX_FC_UT_HWQPS MAX_FC_UT_POLL_THREADS
static struct spdk_nvmf_tgt *g_nvmf_tgt = NULL;
static struct spdk_nvmf_transport *g_nvmf_tprt = NULL;
uint8_t g_fc_port_handle = 0xff;
char lld_q[MAX_FC_UT_HWQPS];

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

	allocate_threads(8);
	set_thread(0);

	g_nvmf_tgt = spdk_nvmf_tgt_create(2);
	CU_ASSERT(g_nvmf_tgt != NULL);

	if (g_nvmf_tgt == NULL) {
		return;
	}

	ops = spdk_nvmf_get_transport_ops((enum spdk_nvme_transport_type) SPDK_NVMF_TRTYPE_FC);

	CU_ASSERT(ops != NULL);
	if (!ops) {
		return;
	}

	ops->opts_init(&opts);

	g_lld_init_called = false;
	g_nvmf_tprt = spdk_nvmf_transport_create((enum spdk_nvme_transport_type) SPDK_NVMF_TRTYPE_FC,
			&opts);
	CU_ASSERT(g_nvmf_tprt != NULL);

	if (g_nvmf_tprt) {
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

		/* Add transport agsin - should get error */
		spdk_nvmf_tgt_add_transport(g_nvmf_tgt, g_nvmf_tprt,
					    _add_transport_done_dup_err, 0);
		poll_thread(0);

		/* create transport with bad args/options */
#ifndef SPDK_CONFIG_RDMA
		CU_ASSERT(spdk_nvmf_transport_create(SPDK_NVMF_TRTYPE_RDMA, &opts) == NULL);
#endif
		CU_ASSERT(spdk_nvmf_transport_create(998, &opts) == NULL);
		opts.max_io_size = 1024 ^ 3;
		CU_ASSERT(spdk_nvmf_transport_create((enum spdk_nvme_transport_type) SPDK_NVMF_TRTYPE_FC,
						     &opts) == NULL);
		opts.max_io_size = 999;
		opts.io_unit_size = 1024;
		CU_ASSERT(spdk_nvmf_transport_create((enum spdk_nvme_transport_type) SPDK_NVMF_TRTYPE_FC,
						     &opts) == NULL);
	}
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

	if (!g_nvmf_tprt) {
		CU_FAIL("No transport available");
		return;
	}

	init_args.port_handle = 2;
	init_args.io_queue_cnt = MAX_FC_UT_HWQPS;
	init_args.ls_queue_size = 100;
	init_args.io_queue_size = 100;
	init_args.io_queues = (void *)lld_q;

	set_thread(0);
	err = spdk_nvmf_fc_master_enqueue_event(SPDK_FC_HW_PORT_INIT, (void *)&init_args, port_init_cb);
	CU_ASSERT(err == 0);
	poll_thread(0);

	fc_port = spdk_nvmf_fc_port_list_get(g_fc_port_handle);
	CU_ASSERT(fc_port != NULL);
}

static void
online_fc_port_test(void)
{
	struct spdk_nvmf_fc_port *fc_port;
	struct spdk_nvmf_fc_hw_port_online_args args;
	int err;

	if (!g_nvmf_tprt) {
		CU_FAIL("No transport available");
		return;
	}

	fc_port = spdk_nvmf_fc_port_list_get(g_fc_port_handle);
	CU_ASSERT(fc_port != NULL);

	if (fc_port) {
		set_thread(0);
		args.port_handle = g_fc_port_handle;
		err = spdk_nvmf_fc_master_enqueue_event(SPDK_FC_HW_PORT_ONLINE, (void *)&args, port_init_cb);
		CU_ASSERT(err == 0);
		poll_threads();
		set_thread(0);
		if (err == 0) {
			uint32_t i;
			for (i = 0; i < fc_port->num_io_queues; i++) {
				CU_ASSERT(fc_port->io_queues[i].fc_poll_group != 0);
				CU_ASSERT(fc_port->io_queues[i].fc_poll_group != 0);
				CU_ASSERT(fc_port->io_queues[i].fc_poll_group->hwqp_count == 1);
			}
		}
	}
}

static void
create_poll_groups_test(void)
{
	if (g_nvmf_tprt) {
		unsigned i;
		for (i = 0; i < MAX_FC_UT_POLL_THREADS; i++) {
			set_thread(i);
			g_poll_groups[i] = spdk_nvmf_poll_group_create(g_nvmf_tgt);
			poll_thread(i);
			CU_ASSERT(g_poll_groups[i] != NULL);
		}
		set_thread(0);
	} else {
		CU_FAIL("No transport available");
	}
}

static void
poll_group_poll_test(void)
{
	unsigned i;
	unsigned poll_cnt =  10;
	struct spdk_nvmf_fc_port *fc_port = NULL;

	if (!g_nvmf_tprt) {
		CU_FAIL("No transport available");
		return;
	}

	set_thread(0);
	fc_port = spdk_nvmf_fc_port_list_get(g_fc_port_handle);
	CU_ASSERT(fc_port != NULL);
	if (!fc_port) {
		return;
	}

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

	if (!g_nvmf_tprt) {
		CU_FAIL("No transport available");
		return;
	}

	fc_port = spdk_nvmf_fc_port_list_get(g_fc_port_handle);
	CU_ASSERT(fc_port != NULL);
	if (!fc_port) {
		return;
	}


	for (i = 0; i < fc_port->num_io_queues; i++) {
		spdk_nvmf_fc_remove_hwqp_from_poller(&fc_port->io_queues[i]);
		poll_threads();
		CU_ASSERT(fc_port->io_queues[i].fc_poll_group == 0);
	}
}

static void
destroy_transport_test(void)
{
	set_thread(0);
	if (g_nvmf_tprt) {
		unsigned i;
		for (i = 0; i < MAX_FC_UT_POLL_THREADS; i++) {
			set_thread(i);
			spdk_nvmf_poll_group_destroy(g_poll_groups[i]);
			poll_thread(0);
		}
	} else {
		CU_FAIL("No transport available");
	}

	if (g_nvmf_tgt) {
		g_lld_fini_called = false;
		spdk_nvmf_tgt_destroy(g_nvmf_tgt, NULL, NULL);
		poll_threads();
		CU_ASSERT(g_lld_fini_called == true);
	} else {
		CU_FAIL("No nvmf target available");
	}
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

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("NVMf-FC", nvmf_fc_tests_init, nvmf_fc_tests_fini);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (CU_add_test(suite, "Create Target & FC Transport",
			create_transport_test) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (CU_add_test(suite, "Create Poll Groups",
			create_poll_groups_test) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (CU_add_test(suite, "Create FC Port",
			create_fc_port_test) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}


	if (CU_add_test(suite, "Online FC Port",
			online_fc_port_test) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (CU_add_test(suite, "PG poll", poll_group_poll_test) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (CU_add_test(suite, "Remove HWQP's from PG's",
			remove_hwqps_from_poll_groups_test) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (CU_add_test(suite, "Destroy Transport & Target",
			destroy_transport_test) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
