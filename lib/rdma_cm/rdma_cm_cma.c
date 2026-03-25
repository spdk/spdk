/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Alibaba Cloud and its affiliates. All rights reserved.
 */

#include "spdk_internal/rdma_cm.h"

struct rdma_event_channel *
spdk_rdma_cm_create_event_channel(void)
{
	return rdma_create_event_channel();
}

void
spdk_rdma_cm_destroy_event_channel(struct rdma_event_channel *channel)
{
	rdma_destroy_event_channel(channel);
}

int
spdk_rdma_cm_create_id(struct rdma_event_channel *channel, struct rdma_cm_id **id,
		       void *context, enum rdma_port_space ps)
{
	return rdma_create_id(channel, id, context, ps);
}

int
spdk_rdma_cm_destroy_id(struct rdma_cm_id *id)
{
	return rdma_destroy_id(id);
}

int
spdk_rdma_cm_set_option(struct rdma_cm_id *id, int level, int optname,
			void *optval, size_t optlen)
{
	return rdma_set_option(id, level, optname, optval, optlen);
}

int
spdk_rdma_cm_bind_addr(struct rdma_cm_id *id, struct sockaddr *addr)
{
	return rdma_bind_addr(id, addr);
}

int
spdk_rdma_cm_resolve_addr(struct rdma_cm_id *id, struct sockaddr *src_addr,
			  struct sockaddr *dst_addr, int timeout_ms)
{
	return rdma_resolve_addr(id, src_addr, dst_addr, timeout_ms);
}

int
spdk_rdma_cm_resolve_route(struct rdma_cm_id *id, int timeout_ms)
{
	return rdma_resolve_route(id, timeout_ms);
}

int
spdk_rdma_cm_create_qp(struct rdma_cm_id *id, struct ibv_pd *pd,
		       struct ibv_qp_init_attr *qp_init_attr)
{
	return rdma_create_qp(id, pd, qp_init_attr);
}

int
spdk_rdma_cm_create_qp_ex(struct rdma_cm_id *id,
			  struct ibv_qp_init_attr_ex *qp_init_attr)
{
	return rdma_create_qp_ex(id, qp_init_attr);
}

void
spdk_rdma_cm_destroy_qp(struct rdma_cm_id *id)
{
	rdma_destroy_qp(id);
}

int
spdk_rdma_cm_init_qp_attr(struct rdma_cm_id *id, struct ibv_qp_attr *qp_attr,
			  int *qp_attr_mask)
{
#ifdef __FreeBSD__
	/**
	 * rdma_init_qp_attr is not supported on FreeBSD.
	 */
	errno = ENOTSUP;
	return -1;
#else
	return rdma_init_qp_attr(id, qp_attr, qp_attr_mask);
#endif
}

int
spdk_rdma_cm_connect(struct rdma_cm_id *id, struct rdma_conn_param *conn_param)
{
	return rdma_connect(id, conn_param);
}

int
spdk_rdma_cm_establish(struct rdma_cm_id *id)
{
#ifdef __FreeBSD__
	/**
	 * rdma_establish is not supported on FreeBSD.
	 */
	errno = ENOTSUP;
	return -1;
#else
	return rdma_establish(id);
#endif
}

int
spdk_rdma_cm_listen(struct rdma_cm_id *id, int backlog)
{
	return rdma_listen(id, backlog);
}

int
spdk_rdma_cm_accept(struct rdma_cm_id *id, struct rdma_conn_param *conn_param)
{
	return rdma_accept(id, conn_param);
}

int
spdk_rdma_cm_reject(struct rdma_cm_id *id, const void *private_data,
		    uint8_t private_data_len)
{
	return rdma_reject(id, private_data, private_data_len);
}

int
spdk_rdma_cm_disconnect(struct rdma_cm_id *id)
{
	return rdma_disconnect(id);
}

int
spdk_rdma_cm_get_cm_event(struct rdma_event_channel *channel,
			  struct rdma_cm_event **event)
{
	return rdma_get_cm_event(channel, event);
}

int
spdk_rdma_cm_ack_cm_event(struct rdma_cm_event *event)
{
	return rdma_ack_cm_event(event);
}

__be16
spdk_rdma_cm_get_src_port(struct rdma_cm_id *id)
{
	return rdma_get_src_port(id);
}

__be16
spdk_rdma_cm_get_dst_port(struct rdma_cm_id *id)
{
	return rdma_get_dst_port(id);
}

struct ibv_context **
spdk_rdma_cm_get_devices(int *num_devices)
{
	return rdma_get_devices(num_devices);
}

void
spdk_rdma_cm_free_devices(struct ibv_context **list)
{
	rdma_free_devices(list);
}
