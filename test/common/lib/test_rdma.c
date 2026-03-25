/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation. All rights reserved.
 *   Copyright (c) 2020, 2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk_internal/rdma_cm.h"
#include "spdk_internal/rdma_provider.h"
#include "spdk_internal/rdma_utils.h"
#include "spdk_internal/mock.h"

#define RDMA_UT_LKEY 123
#define RDMA_UT_RKEY 312

struct spdk_nvme_transport_opts g_spdk_nvme_transport_opts = {};
struct spdk_rdma_provider_qp g_spdk_rdma_qp = {};
struct spdk_rdma_provider_srq g_spdk_rdma_srq = {};

DEFINE_STUB(spdk_rdma_provider_qp_create, struct spdk_rdma_provider_qp *, (struct rdma_cm_id *cm_id,
		struct spdk_rdma_provider_qp_init_attr *qp_attr), &g_spdk_rdma_qp);
DEFINE_STUB(spdk_rdma_provider_qp_accept, int, (struct spdk_rdma_provider_qp *spdk_rdma_qp,
		struct rdma_conn_param *conn_param), 0);
DEFINE_STUB(spdk_rdma_provider_qp_complete_connect, int,
	    (struct spdk_rdma_provider_qp *spdk_rdma_qp), 0);
DEFINE_STUB_V(spdk_rdma_provider_qp_destroy, (struct spdk_rdma_provider_qp *spdk_rdma_qp));
DEFINE_STUB(spdk_rdma_provider_qp_disconnect, int, (struct spdk_rdma_provider_qp *spdk_rdma_qp), 0);
DEFINE_STUB(spdk_rdma_provider_qp_queue_send_wrs, bool, (struct spdk_rdma_provider_qp *spdk_rdma_qp,
		struct ibv_send_wr *first), true);
DEFINE_STUB(spdk_rdma_provider_qp_flush_send_wrs, int, (struct spdk_rdma_provider_qp *spdk_rdma_qp,
		struct ibv_send_wr **bad_wr), 0);
DEFINE_STUB(spdk_rdma_provider_srq_create, struct spdk_rdma_provider_srq *,
	    (struct spdk_rdma_provider_srq_init_attr *init_attr), &g_spdk_rdma_srq);
DEFINE_STUB(spdk_rdma_provider_srq_destroy, int, (struct spdk_rdma_provider_srq *rdma_srq), 0);
DEFINE_STUB(spdk_rdma_provider_srq_queue_recv_wrs, bool, (struct spdk_rdma_provider_srq *rdma_srq,
		struct ibv_recv_wr *first), true);
DEFINE_STUB(spdk_rdma_provider_srq_flush_recv_wrs, int, (struct spdk_rdma_provider_srq *rdma_srq,
		struct ibv_recv_wr **bad_wr), 0);
DEFINE_STUB(spdk_rdma_provider_qp_queue_recv_wrs, bool, (struct spdk_rdma_provider_qp *spdk_rdma_qp,
		struct ibv_recv_wr *first), true);
DEFINE_STUB(spdk_rdma_provider_qp_flush_recv_wrs, int, (struct spdk_rdma_provider_qp *spdk_rdma_qp,
		struct ibv_recv_wr **bad_wr), 0);
DEFINE_STUB(spdk_rdma_utils_create_mem_map, struct spdk_rdma_utils_mem_map *, (struct ibv_pd *pd,
		struct spdk_nvme_rdma_hooks *hooks, uint32_t access_flags), NULL)
DEFINE_STUB_V(spdk_rdma_utils_free_mem_map, (struct spdk_rdma_utils_mem_map **map));
DEFINE_STUB(spdk_rdma_utils_get_memory_domain, struct spdk_memory_domain *, (struct ibv_pd *pd),
	    NULL);
DEFINE_STUB(spdk_rdma_utils_put_memory_domain, int, (struct spdk_memory_domain *domain), 0);

/* used to mock out having to split an SGL over a memory region */
size_t g_mr_size;
uint64_t g_mr_next_size;
struct ibv_mr g_rdma_mr = {
	.addr = (void *)0xC0FFEE,
	.lkey = RDMA_UT_LKEY,
	.rkey = RDMA_UT_RKEY
};

DEFINE_RETURN_MOCK(spdk_rdma_utils_get_translation, int);
int
spdk_rdma_utils_get_translation(struct spdk_rdma_utils_mem_map *map, void *address,
				size_t length, struct spdk_rdma_utils_memory_translation *translation)
{
	translation->mr_or_key.mr = &g_rdma_mr;
	translation->translation_type = SPDK_RDMA_UTILS_TRANSLATION_MR;
	HANDLE_RETURN_MOCK(spdk_rdma_utils_get_translation);

	if (g_mr_size && length > g_mr_size) {
		if (g_mr_next_size) {
			g_mr_size = g_mr_next_size;
		}
		return -ERANGE;
	}

	return 0;
}

DEFINE_RETURN_MOCK(spdk_rdma_utils_get_pd, struct ibv_pd *);
struct ibv_pd *
spdk_rdma_utils_get_pd(struct ibv_context *context)
{
	HANDLE_RETURN_MOCK(spdk_rdma_utils_get_pd);
	return NULL;
}

DEFINE_STUB_V(spdk_rdma_utils_put_pd, (struct ibv_pd *pd));

/* RDMA CM Stubs */
DEFINE_STUB_V(spdk_rdma_cm_destroy_event_channel, (struct rdma_event_channel *channel));
DEFINE_STUB(spdk_rdma_cm_create_id, int, (struct rdma_event_channel *channel,
		struct rdma_cm_id **id, void *context, enum rdma_port_space ps), 0);
DEFINE_STUB(spdk_rdma_cm_destroy_id, int, (struct rdma_cm_id *id), 0);
DEFINE_STUB(spdk_rdma_cm_set_option, int, (struct rdma_cm_id *id, int level, int optname,
		void *optval, size_t optlen), 0);
DEFINE_STUB(spdk_rdma_cm_bind_addr, int, (struct rdma_cm_id *id, struct sockaddr *addr), 0);
DEFINE_STUB(spdk_rdma_cm_resolve_addr, int, (struct rdma_cm_id *id, struct sockaddr *src_addr,
		struct sockaddr *dst_addr, int timeout_ms), 0);
DEFINE_STUB(spdk_rdma_cm_resolve_route, int, (struct rdma_cm_id *id, int timeout_ms), 0);
DEFINE_STUB(spdk_rdma_cm_create_qp, int, (struct rdma_cm_id *id, struct ibv_pd *pd,
		struct ibv_qp_init_attr *qp_init_attr), 0);
DEFINE_STUB(spdk_rdma_cm_create_qp_ex, int, (struct rdma_cm_id *id,
		struct ibv_qp_init_attr_ex *qp_init_attr), 0);
DEFINE_STUB_V(spdk_rdma_cm_destroy_qp, (struct rdma_cm_id *id));
DEFINE_STUB(spdk_rdma_cm_init_qp_attr, int, (struct rdma_cm_id *id, struct ibv_qp_attr *qp_attr,
		int *qp_attr_mask), 0);
DEFINE_STUB(spdk_rdma_cm_connect, int, (struct rdma_cm_id *id, struct rdma_conn_param *conn_param),
	    0);
DEFINE_STUB(spdk_rdma_cm_establish, int, (struct rdma_cm_id *id), 0);
DEFINE_STUB(spdk_rdma_cm_listen, int, (struct rdma_cm_id *id, int backlog), 0);
DEFINE_STUB(spdk_rdma_cm_accept, int, (struct rdma_cm_id *id, struct rdma_conn_param *conn_param),
	    0);
DEFINE_STUB(spdk_rdma_cm_reject, int, (struct rdma_cm_id *id, const void *private_data,
				       uint8_t private_data_len), 0);
DEFINE_STUB(spdk_rdma_cm_disconnect, int, (struct rdma_cm_id *id), 0);
DEFINE_STUB(spdk_rdma_cm_get_cm_event, int, (struct rdma_event_channel *channel,
		struct rdma_cm_event **event), 0);
DEFINE_STUB(spdk_rdma_cm_ack_cm_event, int, (struct rdma_cm_event *event), 0);
DEFINE_STUB(spdk_rdma_cm_get_src_port, __be16, (struct rdma_cm_id *id), 0);
DEFINE_STUB(spdk_rdma_cm_get_dst_port, __be16, (struct rdma_cm_id *id), 0);
DEFINE_STUB_V(spdk_rdma_cm_free_devices, (struct ibv_context **list));

DEFINE_RETURN_MOCK(spdk_rdma_cm_get_devices, struct ibv_context **);
struct ibv_context **
spdk_rdma_cm_get_devices(int *num_devices)
{
	static struct ibv_context *_contexts[] = {
		(struct ibv_context *)0xDEADBEEF,
		(struct ibv_context *)0xFEEDBEEF,
		NULL
	};

	HANDLE_RETURN_MOCK(spdk_rdma_cm_get_devices);
	return _contexts;
}

DEFINE_RETURN_MOCK(spdk_rdma_cm_create_event_channel, struct rdma_event_channel *);
struct rdma_event_channel *
spdk_rdma_cm_create_event_channel(void)
{
	HANDLE_RETURN_MOCK(spdk_rdma_cm_create_event_channel);
	return NULL;
}
