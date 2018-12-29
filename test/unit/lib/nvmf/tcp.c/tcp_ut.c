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
#include "spdk_internal/mock.h"
#include "spdk_internal/thread.h"

#include "common/lib/test_env.c"
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
	      (struct spdk_nvmf_tgt *tgt, struct iovec *iov, uint32_t iovcnt, uint64_t offset, uint32_t length));

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

DEFINE_STUB(spdk_nvmf_transport_req_complete,
	    int,
	    (struct spdk_nvmf_request *req),
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
spdk_trace_register_description(const char *name, const char *short_name,
				uint16_t tpoint_id, uint8_t owner_type,
				uint8_t object_type, uint8_t new_object,
				uint8_t arg1_is_ptr, const char *arg1_name)
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

void
spdk_nvmf_bdev_ctrlr_identify_ns(struct spdk_nvmf_ns *ns, struct spdk_nvme_ns_data *nsdata)
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

	thread = spdk_thread_create(NULL);
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
}

static void
test_nvmf_tcp_destroy(void)
{
	struct spdk_thread *thread;
	struct spdk_nvmf_transport *transport;
	struct spdk_nvmf_transport_opts opts;

	thread = spdk_thread_create(NULL);
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
}

static void
test_nvmf_tcp_poll_group_create(void)
{
	struct spdk_nvmf_transport *transport;
	struct spdk_nvmf_transport_poll_group *group;
	struct spdk_thread *thread;
	struct spdk_nvmf_transport_opts opts;

	thread = spdk_thread_create(NULL);
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
	group = spdk_nvmf_tcp_poll_group_create(transport);
	SPDK_CU_ASSERT_FATAL(group);
	group->transport = transport;
	spdk_nvmf_tcp_poll_group_destroy(group);
	spdk_nvmf_tcp_destroy(transport);

	spdk_thread_exit(thread);
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
		CU_add_test(suite, "nvmf_tcp_poll_group_create", test_nvmf_tcp_poll_group_create) == NULL
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
