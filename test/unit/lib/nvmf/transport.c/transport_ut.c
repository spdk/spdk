/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
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
#include "nvmf/transport.c"
#include "nvmf/rdma.c"
#include "common/lib/test_rdma.c"

SPDK_LOG_REGISTER_COMPONENT(nvmf)

#define RDMA_UT_UNITS_IN_MAX_IO 16
#define SPDK_NVMF_DEFAULT_BUFFER_CACHE_SIZE 32

struct spdk_nvmf_transport_opts g_rdma_ut_transport_opts = {
	.max_queue_depth = SPDK_NVMF_RDMA_DEFAULT_MAX_QUEUE_DEPTH,
	.max_qpairs_per_ctrlr = SPDK_NVMF_RDMA_DEFAULT_MAX_QPAIRS_PER_CTRLR,
	.in_capsule_data_size = SPDK_NVMF_RDMA_DEFAULT_IN_CAPSULE_DATA_SIZE,
	.max_io_size = (SPDK_NVMF_RDMA_MIN_IO_BUFFER_SIZE * RDMA_UT_UNITS_IN_MAX_IO),
	.io_unit_size = SPDK_NVMF_RDMA_MIN_IO_BUFFER_SIZE,
	.max_aq_depth = SPDK_NVMF_RDMA_DEFAULT_AQ_DEPTH,
	.num_shared_buffers = SPDK_NVMF_RDMA_DEFAULT_NUM_SHARED_BUFFERS,
	.opts_size = sizeof(g_rdma_ut_transport_opts)
};

DEFINE_STUB(spdk_nvme_transport_id_compare, int, (const struct spdk_nvme_transport_id *trid1,
		const struct spdk_nvme_transport_id *trid2), 0);
DEFINE_STUB_V(spdk_nvmf_tgt_new_qpair, (struct spdk_nvmf_tgt *tgt, struct spdk_nvmf_qpair *qpair));
DEFINE_STUB(spdk_nvmf_request_get_dif_ctx, bool, (struct spdk_nvmf_request *req,
		struct spdk_dif_ctx *dif_ctx), false);
DEFINE_STUB(spdk_nvmf_qpair_disconnect, int, (struct spdk_nvmf_qpair *qpair,
		nvmf_qpair_disconnect_cb cb_fn, void *ctx), 0);
DEFINE_STUB_V(spdk_nvmf_request_exec, (struct spdk_nvmf_request *req));
DEFINE_STUB_V(spdk_nvme_trid_populate_transport, (struct spdk_nvme_transport_id *trid,
		enum spdk_nvme_transport_type trtype));
DEFINE_STUB(nvmf_ctrlr_abort_request, int, (struct spdk_nvmf_request *req), 0);
DEFINE_STUB(spdk_nvmf_request_complete, int, (struct spdk_nvmf_request *req), 0);
DEFINE_STUB(ut_transport_create, struct spdk_nvmf_transport *,
	    (struct spdk_nvmf_transport_opts *opts), NULL);
DEFINE_STUB(ut_transport_destroy, int, (struct spdk_nvmf_transport *transport,
					spdk_nvmf_transport_destroy_done_cb cb_fn, void *cb_arg), 0);
DEFINE_STUB(ibv_get_device_name, const char *, (struct ibv_device *device), NULL);
DEFINE_STUB(ibv_query_qp, int, (struct ibv_qp *qp, struct ibv_qp_attr *attr,
				int attr_mask,
				struct ibv_qp_init_attr *init_attr), 0);
DEFINE_STUB(rdma_create_id, int, (struct rdma_event_channel *channel,
				  struct rdma_cm_id **id, void *context,
				  enum rdma_port_space ps), 0);
DEFINE_STUB(rdma_bind_addr, int, (struct rdma_cm_id *id, struct sockaddr *addr), 0);
DEFINE_STUB(rdma_listen, int, (struct rdma_cm_id *id, int backlog), 0);
DEFINE_STUB(rdma_destroy_id, int, (struct rdma_cm_id *id), 0);
DEFINE_STUB(ibv_dereg_mr, int, (struct ibv_mr *mr), 0);
DEFINE_STUB(rdma_reject, int, (struct rdma_cm_id *id,
			       const void *private_data, uint8_t private_data_len), 0);
DEFINE_STUB(ibv_resize_cq, int, (struct ibv_cq *cq, int cqe), 0);
DEFINE_STUB_V(rdma_destroy_qp, (struct rdma_cm_id *id));
DEFINE_STUB_V(rdma_destroy_event_channel, (struct rdma_event_channel *channel));
DEFINE_STUB(ibv_dealloc_pd, int, (struct ibv_pd *pd), 0);
DEFINE_STUB(rdma_create_event_channel, struct rdma_event_channel *, (void), NULL);
DEFINE_STUB(rdma_get_devices, struct ibv_context **, (int *num_devices), NULL);
DEFINE_STUB(ibv_query_device, int, (struct ibv_context *context,
				    struct ibv_device_attr *device_attr), 0);
DEFINE_STUB(ibv_alloc_pd, struct ibv_pd *, (struct ibv_context *context), NULL);
DEFINE_STUB_V(rdma_free_devices, (struct ibv_context **list));
DEFINE_STUB(ibv_get_async_event, int, (struct ibv_context *context, struct ibv_async_event *event),
	    0);
DEFINE_STUB(ibv_event_type_str, const char *, (enum ibv_event_type event_type), NULL);
DEFINE_STUB_V(ibv_ack_async_event, (struct ibv_async_event *event));
DEFINE_STUB(rdma_get_cm_event, int, (struct rdma_event_channel *channel,
				     struct rdma_cm_event **event), 0);
DEFINE_STUB(rdma_ack_cm_event, int, (struct rdma_cm_event *event), 0);
DEFINE_STUB(ibv_destroy_cq, int, (struct ibv_cq *cq), 0);
DEFINE_STUB(ibv_create_cq, struct ibv_cq *, (struct ibv_context *context, int cqe,
		void *cq_context,
		struct ibv_comp_channel *channel,
		int comp_vector), NULL);
DEFINE_STUB(ibv_wc_status_str, const char *, (enum ibv_wc_status status), NULL);
DEFINE_STUB(rdma_get_dst_port, __be16, (struct rdma_cm_id *id), 0);
DEFINE_STUB(rdma_get_src_port, __be16, (struct rdma_cm_id *id), 0);
DEFINE_STUB(spdk_nvmf_qpair_get_listen_trid, int, (struct spdk_nvmf_qpair *qpair,
		struct spdk_nvme_transport_id *trid), 0);
DEFINE_STUB(ibv_reg_mr_iova2, struct ibv_mr *, (struct ibv_pd *pd, void *addr, size_t length,
		uint64_t iova, unsigned int access), NULL);
DEFINE_STUB(spdk_nvme_transport_id_adrfam_str, const char *, (enum spdk_nvmf_adrfam adrfam), NULL);

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

static void
test_spdk_nvmf_transport_create(void)
{
	int rc;
	struct spdk_nvmf_transport ut_transport = {};
	struct spdk_nvmf_transport *transport = NULL;
	struct nvmf_transport_ops_list_element *ops_element;
	struct spdk_nvmf_transport_ops ops = {
		.name = "new_ops",
		.type = (enum spdk_nvme_transport_type)SPDK_NVMF_TRTYPE_RDMA,
		.create = ut_transport_create,
		.destroy = ut_transport_destroy
	};

	/* No available ops element */
	transport = spdk_nvmf_transport_create("new_ops", &g_rdma_ut_transport_opts);
	CU_ASSERT(transport == NULL);

	/* Create transport successfully */
	MOCK_SET(ut_transport_create, &ut_transport);
	spdk_nvmf_transport_register(&ops);

	transport = spdk_nvmf_transport_create("new_ops", &g_rdma_ut_transport_opts);
	CU_ASSERT(transport == &ut_transport);
	CU_ASSERT(!memcmp(&transport->opts, &g_rdma_ut_transport_opts, sizeof(g_rdma_ut_transport_opts)));
	CU_ASSERT(!memcmp(transport->ops, &ops, sizeof(ops)));
	CU_ASSERT(transport->data_buf_pool != NULL);

	rc = spdk_nvmf_transport_destroy(transport, NULL, NULL);
	CU_ASSERT(rc == 0);

	/* transport_opts parameter invalid */
	g_rdma_ut_transport_opts.max_io_size = 4096;

	transport = spdk_nvmf_transport_create("new_ops", &g_rdma_ut_transport_opts);
	CU_ASSERT(transport == NULL);
	g_rdma_ut_transport_opts.max_io_size = (SPDK_NVMF_RDMA_MIN_IO_BUFFER_SIZE *
						RDMA_UT_UNITS_IN_MAX_IO);

	ops_element = TAILQ_LAST(&g_spdk_nvmf_transport_ops, nvmf_transport_ops_list);
	TAILQ_REMOVE(&g_spdk_nvmf_transport_ops, ops_element, link);
	free(ops_element);
	MOCK_CLEAR(ut_transport_create);
}

static struct spdk_nvmf_transport_poll_group *
ut_poll_group_create(struct spdk_nvmf_transport *transport)
{
	struct spdk_nvmf_transport_poll_group *group;

	group = calloc(1, sizeof(*group));
	SPDK_CU_ASSERT_FATAL(group != NULL);
	return group;
}

static void
ut_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group)
{
	free(group);
}

static void
test_nvmf_transport_poll_group_create(void)
{
	struct spdk_nvmf_transport_poll_group *poll_group = NULL;
	struct spdk_nvmf_transport transport = {};
	struct spdk_nvmf_transport_ops ops = {};

	ops.poll_group_create = ut_poll_group_create;
	ops.poll_group_destroy = ut_poll_group_destroy;
	transport.ops = &ops;
	transport.opts.buf_cache_size = SPDK_NVMF_DEFAULT_BUFFER_CACHE_SIZE;
	transport.data_buf_pool = spdk_mempool_create("buf_pool", 32, 4096, 0, 0);

	poll_group = nvmf_transport_poll_group_create(&transport);
	SPDK_CU_ASSERT_FATAL(poll_group != NULL);
	CU_ASSERT(poll_group->transport == &transport);
	CU_ASSERT(poll_group->buf_cache_size == SPDK_NVMF_DEFAULT_BUFFER_CACHE_SIZE);
	CU_ASSERT(poll_group->buf_cache_count == SPDK_NVMF_DEFAULT_BUFFER_CACHE_SIZE);

	nvmf_transport_poll_group_destroy(poll_group);
	spdk_mempool_free(transport.data_buf_pool);

	/* Mempool members insufficient */
	transport.data_buf_pool = spdk_mempool_create("buf_pool", 31, 4096, 0, 0);

	poll_group = nvmf_transport_poll_group_create(&transport);
	SPDK_CU_ASSERT_FATAL(poll_group != NULL);
	CU_ASSERT(poll_group->transport == &transport);
	CU_ASSERT(poll_group->buf_cache_size == 31);
	CU_ASSERT(poll_group->buf_cache_count == 31);

	nvmf_transport_poll_group_destroy(poll_group);
	spdk_mempool_free(transport.data_buf_pool);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("nvmf", NULL, NULL);

	CU_ADD_TEST(suite, test_spdk_nvmf_transport_create);
	CU_ADD_TEST(suite, test_nvmf_transport_poll_group_create);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
