/*
 *   Copyright © 2022 NVIDIA CORPORATION & AFFILIATES. ALL RIGHTS RESERVED.
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

#ifndef __VRDMA_QP_H__
#define __VRDMA_QP_H__
#include <stdio.h>
#include "spdk/stdinc.h"
#include "vrdma.h"
#include "vrdma_admq.h"
#include "vrdma_rpc.h"
#include "vrdma_controller.h"
#include "snap_vrdma_virtq.h"

#define VRDMA_INVALID_QPN 0xFFFFFFFF
#define VRDMA_INVALID_DEVID 0xFFFFFFFF

/* RTR state params */
#define VRDMA_MIN_RNR_TIMER 12
#define VRDMA_QP_MAX_DEST_RD_ATOMIC 16
#define VRDMA_MQP_SRC_ADDR_INDEX 1

/* RTS state params */
#define VRDMA_BACKEND_QP_TIMEOUT 14
#define VRDMA_BACKEND_QP_RETRY_COUNT 7
#define VRDMA_BACKEND_QP_RNR_RETRY 7
#define VRDMA_BACKEND_QP_SQ_SIZE 32*1024
#define VRDMA_BACKEND_QP_RQ_SIZE 32*1024
#define VRDMA_QP_MAX_RD_ATOMIC 16
//#define MPATH_DBG
struct snap_vrdma_backend_qp;

struct mqp_sq_meta {
    uint16_t req_id;
    struct spdk_vrdma_qp *vqp;
};

struct vrdma_backend_qp {
    struct ibv_pd *pd;
#define VRDMA_INVALID_POLLER_CORE 0xFFFFFFFF
    uint32_t poller_core;
    struct snap_vrdma_backend_qp bk_qp;
    LIST_HEAD(, vrdma_vqp) vqp_list;
    uint32_t remote_qpn;
    uint32_t qp_state;
    struct mqp_sq_meta *sq_meta_buf;
};

struct vrdma_vqp {
    LIST_ENTRY(vrdma_vqp) entry;
    uint32_t qpn;
    struct spdk_vrdma_qp *vqp;
};

LIST_HEAD(vrdma_tgid_list_head, vrdma_tgid_node);
extern struct vrdma_tgid_list_head vrdma_tgid_list;

struct spdk_vrdma_qp *
find_spdk_vrdma_qp_by_idx(struct vrdma_ctrl *ctrl, uint32_t qp_idx);
void vrdma_destroy_backend_qp(struct vrdma_backend_qp **local_mqp);
int vrdma_modify_backend_qp_to_init(struct vrdma_backend_qp *bk_qp);
int vrdma_modify_backend_qp_to_rtr(struct vrdma_backend_qp *bk_qp,
				struct ibv_qp_attr *qp_attr, int attr_mask,
			    struct snap_vrdma_bk_qp_rdy_attr *rdy_attr);
int vrdma_modify_backend_qp_to_rts(struct vrdma_backend_qp *bk_qp);
void set_spdk_vrdma_bk_qp_active(struct vrdma_backend_qp *bk_qp);
int vrdma_create_vq(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe,
				struct spdk_vrdma_qp *vqp,
				struct spdk_vrdma_cq *rq_vcq,
				struct spdk_vrdma_cq *sq_vcq);
bool vrdma_set_vq_flush(struct vrdma_ctrl *ctrl,
				struct spdk_vrdma_qp *vqp);
void vrdma_destroy_vq(struct vrdma_ctrl *ctrl,
				struct spdk_vrdma_qp *vqp);
bool vrdma_qp_is_suspended(struct vrdma_ctrl *ctrl, uint32_t qp_handle);
bool vrdma_qp_is_connected_ready(struct spdk_vrdma_qp *vqp);
struct vrdma_tgid_node *
vrdma_find_tgid_node(union ibv_gid *remote_tgid, union ibv_gid *local_tgid);
void vrdma_destroy_tgid_list(void);
struct vrdma_tgid_node *
vrdma_create_tgid_node(union ibv_gid *remote_tgid,
                       union ibv_gid *local_tgid,
                       struct spdk_vrdma_dev *local_vdev,
                       struct ibv_pd *local_pd,
                       uint16_t udp_sport_start,
                       uint32_t max_mqp_cnt);
struct vrdma_backend_qp *
vrdma_create_backend_qp(struct vrdma_tgid_node *tgid_node,
                        uint8_t mqp_idx);
struct spdk_vrdma_qp *
vrdma_mqp_find_vqp(struct vrdma_backend_qp *mqp,
                   uint32_t vqp_idx);
int vrdma_qp_notify_remote_by_rpc(struct vrdma_ctrl *ctrl,
                                  struct vrdma_tgid_node *tgid_node,
                                  uint8_t mqp_idx);
int vrdma_mqp_add_vqp_to_list(struct vrdma_backend_qp *mqp,
                              struct spdk_vrdma_qp *vqp,
                              uint32_t vqp_idx);
void
vrdma_mqp_del_vqp_from_list(struct vrdma_backend_qp *mqp,
                            uint32_t vqp_idx);
void vrdma_set_rpc_msg_with_mqp_info(struct vrdma_ctrl *ctrl,
                                     struct vrdma_tgid_node *tgid_node,
                                     uint8_t mqp_idx,
                                     struct spdk_vrdma_rpc_qp_msg *msg);

static inline int vrdma_vq_rollback(uint16_t pre_pi, uint16_t pi,
								   uint16_t q_size)
{
	if (pi % q_size == 0) {
		return 0;
	}
	return !(pi % q_size > pre_pi % q_size);
}
#endif
