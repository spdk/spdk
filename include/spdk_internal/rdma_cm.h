/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Alibaba Cloud and its affiliates. All rights reserved.
 */

#ifndef SPDK_RDMA_CM_H
#define SPDK_RDMA_CM_H

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>

/**
 * Create RDMA event channel
 *
 * See rdma_create_event_channel for more details.
 *
 * \return Non-NULL event channel on success, NULL on failure and errno will be set.
 */
struct rdma_event_channel *spdk_rdma_cm_create_event_channel(void);

/**
 * Destroy RDMA event channel
 *
 * See rdma_destroy_event_channel for more details.
 *
 * \param channel The pointer to the event channel to destroy
 */
void spdk_rdma_cm_destroy_event_channel(struct rdma_event_channel *channel);

/**
 * Create RDMA CM id
 *
 * See rdma_create_id for more details.
 *
 * \param channel The event channel to use for the RDMA CM id
 * \param id The pointer to store the created RDMA CM id on success
 * \param context User context
 * \param ps RDMA port space
 *
 * \return 0 on success, or -1 on error. If an error occurs, errno will be set.
 */
int spdk_rdma_cm_create_id(struct rdma_event_channel *channel,
			   struct rdma_cm_id **id, void *context,
			   enum rdma_port_space ps);

/**
 * Destroy RDMA CM id
 *
 * See rdma_destroy_id for more details.
 *
 * \param id Pointer to RDMA CM id
 *
 * \return 0 on success, or -1 on error. If an error occurs, errno will be set.
 */
int spdk_rdma_cm_destroy_id(struct rdma_cm_id *id);

/**
 * Set RDMA CM option
 *
 * See rdma_set_option for more details.
 *
 * \param id Pointer to RDMA CM id
 * \param level Option level
 * \param optname Option name
 * \param optval Option value
 * \param optlen Option value length
 *
 * \return 0 on success, or -1 on error. If an error occurs, errno will be set.
 */
int spdk_rdma_cm_set_option(struct rdma_cm_id *id, int level, int optname,
			    void *optval, size_t optlen);

/**
 * Bind address to RDMA CM id
 *
 * See rdma_bind_addr for more details.
 *
 * \param id Pointer to RDMA CM id
 * \param addr Pointer to address
 *
 * \return 0 on success, or -1 on error. If an error occurs, errno will be set.
 */
int spdk_rdma_cm_bind_addr(struct rdma_cm_id *id, struct sockaddr *addr);

/**
 * Resolve address
 *
 * See rdma_resolve_addr for more details.
 *
 * \param id Pointer to RDMA CM id
 * \param src_addr Pointer to source address
 * \param dst_addr Pointer to destination address
 * \param timeout_ms Timeout in milliseconds
 *
 * \return 0 on success, or -1 on error. If an error occurs, errno will be set.
 */
int spdk_rdma_cm_resolve_addr(struct rdma_cm_id *id, struct sockaddr *src_addr,
			      struct sockaddr *dst_addr, int timeout_ms);

/**
 * Resolve route
 *
 * See rdma_resolve_route for more details.
 *
 * \param id Pointer to RDMA CM id
 * \param timeout_ms Timeout in milliseconds
 *
 * \return 0 on success, or -1 on error. If an error occurs, errno will be set.
 */
int spdk_rdma_cm_resolve_route(struct rdma_cm_id *id, int timeout_ms);

/**
 * Create qp for the RDMA CM id
 *
 * See rdma_create_qp for more details.
 *
 * \param id The CM id
 * \param pd The protection domain
 * \param qp_init_attr Pointer to qp init attr
 *
 * \return 0 on success, or -1 on error. If an error occurs, errno will be set.
 */
int spdk_rdma_cm_create_qp(struct rdma_cm_id *id, struct ibv_pd *pd,
			   struct ibv_qp_init_attr *qp_init_attr);

/**
 * Create qp for the RDMA CM id
 *
 * See rdma_create_qp_ex for more details.
 *
 * \param id The CM id
 * \param qp_init_attr Pointer to qp init attr
 *
 * \return 0 on success, or -1 on error. If an error occurs, errno will be set.
 */
int spdk_rdma_cm_create_qp_ex(struct rdma_cm_id *id,
			      struct ibv_qp_init_attr_ex *qp_init_attr);

/**
 * Destroy qp for the RDMA CM id
 *
 * See rdma_destroy_qp for more details.
 *
 * \param id The CM id
 */
void spdk_rdma_cm_destroy_qp(struct rdma_cm_id *id);

/**
 * Get qp attributes from the RDMA CM id
 *
 * See rdma_cm_init_qp_attr for more details.
 *
 * \param id The CM id
 * \param qp_attr The qp attr containing the information
 * \param qp_attr_mask Pointer to qp attr mask
 *
 * \return 0 on success, or -1 on error. If an error occurs, errno will be set.
 */
int spdk_rdma_cm_init_qp_attr(struct rdma_cm_id *id, struct ibv_qp_attr *qp_attr,
			      int *qp_attr_mask);

/**
 * Initiate connection request
 *
 * See rdma_connect for more details.
 *
 * \param id The CM id to connect
 * \param conn_param Connection parameters
 *
 * \return 0 on success, or -1 on error. If an error occurs, errno will be set.
 */
int spdk_rdma_cm_connect(struct rdma_cm_id *id, struct rdma_conn_param *conn_param);

/**
 * Complete an active connection request
 *
 * See rdma_establish for more details.
 *
 * \param id Pointer to RDMA CM id
 *
 * \return 0 on success, or -1 on error. If an error occurs, errno will be set.
 */
int spdk_rdma_cm_establish(struct rdma_cm_id *id);

/**
 * Start listening for incoming connection requests
 *
 * See rdma_listen for more details.
 *
 * \param id Pointer to RDMA CM id
 * \param backlog Maximum number of pending incoming connections
 *
 * \return 0 on success, or -1 on error. If an error occurs, errno will be set.
 */
int spdk_rdma_cm_listen(struct rdma_cm_id *id, int backlog);

/**
 * Accept a connection request
 *
 * See rdma_accept for more details.
 *
 * \param id Pointer to RDMA CM id
 * \param conn_param Connection parameters
 *
 * \return 0 on success, or -1 on error. If an error occurs, errno will be set.
 */
int spdk_rdma_cm_accept(struct rdma_cm_id *id, struct rdma_conn_param *conn_param);

/**
 * Reject a connection request
 *
 * See rdma_reject for more details.
 *
 * \param id Pointer to RDMA CM id
 * \param private_data Pointer to private data
 * \param private_data_len Length of private data
 *
 * \return 0 on success, or -1 on error. If an error occurs, errno will be set.
 */
int spdk_rdma_cm_reject(struct rdma_cm_id *id, const void *private_data,
			uint8_t private_data_len);

/**
 * Disconnect an established connection
 *
 * See rdma_disconnect for more details.
 *
 * \param id Pointer to RDMA CM id
 *
 * \return 0 on success, or -1 on error. If an error occurs, errno will be set.
 */
int spdk_rdma_cm_disconnect(struct rdma_cm_id *id);

/**
 * Get RDMA CM event
 *
 * See rdma_get_cm_event for more details.
 *
 * \param channel Pointer to event channel
 * \param event Pointer to event
 *
 * \return 0 on success, or -1 on error. If an error occurs, errno will be set.
 */
int spdk_rdma_cm_get_cm_event(struct rdma_event_channel *channel,
			      struct rdma_cm_event **event);

/**
 * Acknowledge RDMA CM event
 *
 * See rdma_ack_cm_event for more details.
 *
 * \param event Pointer to event
 *
 * \return 0 on success, or -1 on error. If an error occurs, errno will be set.
 */
int spdk_rdma_cm_ack_cm_event(struct rdma_cm_event *event);

/**
 * Get source port of the CM id
 *
 * See rdma_get_src_port for more details.
 *
 * \param id The CM id
 *
 * \return the source port of the CM id
 */
__be16 spdk_rdma_cm_get_src_port(struct rdma_cm_id *id);

/**
 * Get dest port of the CM id
 *
 * See rdma_get_dst_port for more details.
 *
 * \param id The CM id
 *
 * \return the dest port of the CM id
 */
__be16 spdk_rdma_cm_get_dst_port(struct rdma_cm_id *id);

/**
 * Get list of RDMA devices currently available.
 *
 * See rdma_get_devices for more details.
 *
 * \param num_devices Pointer to number of devices
 *
 * \return Pointer to array of ibv_context on success, or NULL on failure and errno will be set.
 */
struct ibv_context **spdk_rdma_cm_get_devices(int *num_devices);

/**
 * Free list of RDMA devices by spdk_rdma_cm_get_devices.
 *
 * See rdma_free_devices for more details.
 *
 * \param list Pointer to array of ibv_context
 */
void spdk_rdma_cm_free_devices(struct ibv_context **list);

#endif /* SPDK_RDMA_CM_H */
