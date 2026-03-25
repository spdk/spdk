/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Alibaba Cloud and its affiliates. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk_internal/rdma_cm.h"

struct rdma_event_channel *
spdk_rdma_cm_create_event_channel(void)
{
	errno = ENOTSUP;
	return NULL;
}

void
spdk_rdma_cm_destroy_event_channel(struct rdma_event_channel *channel)
{
}

int
spdk_rdma_cm_create_id(struct rdma_event_channel *channel, struct rdma_cm_id **id,
		       void *context, enum rdma_port_space ps)
{
	errno = ENOTSUP;
	return -1;
}

int
spdk_rdma_cm_destroy_id(struct rdma_cm_id *id)
{
	errno = ENOTSUP;
	return -1;
}

int
spdk_rdma_cm_set_option(struct rdma_cm_id *id, int level, int optname,
			void *optval, size_t optlen)
{
	errno = ENOTSUP;
	return -1;
}

int
spdk_rdma_cm_bind_addr(struct rdma_cm_id *id, struct sockaddr *addr)
{
	errno = ENOTSUP;
	return -1;
}

int
spdk_rdma_cm_resolve_addr(struct rdma_cm_id *id, struct sockaddr *src_addr,
			  struct sockaddr *dst_addr, int timeout_ms)
{
	errno = ENOTSUP;
	return -1;
}

int
spdk_rdma_cm_resolve_route(struct rdma_cm_id *id, int timeout_ms)
{
	errno = ENOTSUP;
	return -1;
}

int
spdk_rdma_cm_create_qp(struct rdma_cm_id *id, struct ibv_pd *pd,
		       struct ibv_qp_init_attr *qp_init_attr)
{
	errno = ENOTSUP;
	return -1;
}

int
spdk_rdma_cm_create_qp_ex(struct rdma_cm_id *id,
			  struct ibv_qp_init_attr_ex *qp_init_attr)
{
	errno = ENOTSUP;
	return -1;
}

void
spdk_rdma_cm_destroy_qp(struct rdma_cm_id *id)
{
}

int
spdk_rdma_cm_init_qp_attr(struct rdma_cm_id *id, struct ibv_qp_attr *qp_attr,
			  int *qp_attr_mask)
{
	errno = ENOTSUP;
	return -1;
}

int
spdk_rdma_cm_connect(struct rdma_cm_id *id, struct rdma_conn_param *conn_param)
{
	errno = ENOTSUP;
	return -1;
}

int
spdk_rdma_cm_establish(struct rdma_cm_id *id)
{
	errno = ENOTSUP;
	return -1;
}

int
spdk_rdma_cm_listen(struct rdma_cm_id *id, int backlog)
{
	errno = ENOTSUP;
	return -1;
}

int
spdk_rdma_cm_accept(struct rdma_cm_id *id, struct rdma_conn_param *conn_param)
{
	errno = ENOTSUP;
	return -1;
}

int
spdk_rdma_cm_reject(struct rdma_cm_id *id, const void *private_data,
		    uint8_t private_data_len)
{
	errno = ENOTSUP;
	return -1;
}

int
spdk_rdma_cm_disconnect(struct rdma_cm_id *id)
{
	errno = ENOTSUP;
	return -1;
}

int
spdk_rdma_cm_get_cm_event(struct rdma_event_channel *channel,
			  struct rdma_cm_event **event)
{
	errno = ENOTSUP;
	return -1;
}

int
spdk_rdma_cm_ack_cm_event(struct rdma_cm_event *event)
{
	errno = ENOTSUP;
	return -1;
}

__be16
spdk_rdma_cm_get_src_port(struct rdma_cm_id *id)
{
	return 0;
}

__be16
spdk_rdma_cm_get_dst_port(struct rdma_cm_id *id)
{
	return 0;
}

struct ibv_context **
spdk_rdma_cm_get_devices(int *num_devices)
{
	errno = ENOTSUP;
	return NULL;
}

void
spdk_rdma_cm_free_devices(struct ibv_context **list)
{
}
