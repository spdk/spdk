/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation. All rights reserved.
 *   Copyright (c) 2020, 2021 Mellanox Technologies LTD. All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk_internal/rdma.h"
#include "spdk_internal/mock.h"

#define RDMA_UT_LKEY 123
#define RDMA_UT_RKEY 312

struct spdk_nvme_transport_opts g_spdk_nvme_transport_opts = {};
struct spdk_rdma_qp g_spdk_rdma_qp = {};
struct spdk_rdma_srq g_spdk_rdma_srq = {};
DEFINE_STUB(spdk_rdma_qp_create, struct spdk_rdma_qp *, (struct rdma_cm_id *cm_id,
		struct spdk_rdma_qp_init_attr *qp_attr), &g_spdk_rdma_qp);
DEFINE_STUB(spdk_rdma_qp_accept, int, (struct spdk_rdma_qp *spdk_rdma_qp,
				       struct rdma_conn_param *conn_param), 0);
DEFINE_STUB(spdk_rdma_qp_complete_connect, int, (struct spdk_rdma_qp *spdk_rdma_qp), 0);
DEFINE_STUB_V(spdk_rdma_qp_destroy, (struct spdk_rdma_qp *spdk_rdma_qp));
DEFINE_STUB(spdk_rdma_qp_disconnect, int, (struct spdk_rdma_qp *spdk_rdma_qp), 0);
DEFINE_STUB(spdk_rdma_qp_queue_send_wrs, bool, (struct spdk_rdma_qp *spdk_rdma_qp,
		struct ibv_send_wr *first), true);
DEFINE_STUB(spdk_rdma_qp_flush_send_wrs, int, (struct spdk_rdma_qp *spdk_rdma_qp,
		struct ibv_send_wr **bad_wr), 0);
DEFINE_STUB(spdk_rdma_srq_create, struct spdk_rdma_srq *,
	    (struct spdk_rdma_srq_init_attr *init_attr), &g_spdk_rdma_srq);
DEFINE_STUB(spdk_rdma_srq_destroy, int, (struct spdk_rdma_srq *rdma_srq), 0);
DEFINE_STUB(spdk_rdma_srq_queue_recv_wrs, bool, (struct spdk_rdma_srq *rdma_srq,
		struct ibv_recv_wr *first), true);
DEFINE_STUB(spdk_rdma_srq_flush_recv_wrs, int, (struct spdk_rdma_srq *rdma_srq,
		struct ibv_recv_wr **bad_wr), 0);
DEFINE_STUB(spdk_rdma_qp_queue_recv_wrs, bool, (struct spdk_rdma_qp *spdk_rdma_qp,
		struct ibv_recv_wr *first), true);
DEFINE_STUB(spdk_rdma_qp_flush_recv_wrs, int, (struct spdk_rdma_qp *spdk_rdma_qp,
		struct ibv_recv_wr **bad_wr), 0);
DEFINE_STUB(spdk_rdma_create_mem_map, struct spdk_rdma_mem_map *, (struct ibv_pd *pd,
		struct spdk_nvme_rdma_hooks *hooks, enum spdk_rdma_memory_map_role role), NULL)
DEFINE_STUB_V(spdk_rdma_free_mem_map, (struct spdk_rdma_mem_map **map));

/* used to mock out having to split an SGL over a memory region */
size_t g_mr_size;
uint64_t g_mr_next_size;
struct ibv_mr g_rdma_mr = {
	.addr = (void *)0xC0FFEE,
	.lkey = RDMA_UT_LKEY,
	.rkey = RDMA_UT_RKEY
};

DEFINE_RETURN_MOCK(spdk_rdma_get_translation, int);
int
spdk_rdma_get_translation(struct spdk_rdma_mem_map *map, void *address,
			  size_t length, struct spdk_rdma_memory_translation *translation)
{
	translation->mr_or_key.mr = &g_rdma_mr;
	translation->translation_type = SPDK_RDMA_TRANSLATION_MR;
	HANDLE_RETURN_MOCK(spdk_rdma_get_translation);

	if (g_mr_size && length > g_mr_size) {
		if (g_mr_next_size) {
			g_mr_size = g_mr_next_size;
		}
		return -ERANGE;
	}

	return 0;
}

DEFINE_RETURN_MOCK(spdk_rdma_get_pd, struct ibv_pd *);
struct ibv_pd *
spdk_rdma_get_pd(struct ibv_context *context)
{
	HANDLE_RETURN_MOCK(spdk_rdma_get_pd);
	return NULL;
}

DEFINE_STUB_V(spdk_rdma_put_pd, (struct ibv_pd *pd));
