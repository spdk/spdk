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

/* RTS state params */
#define VRDMA_BACKEND_QP_TIMEOUT 14
#define VRDMA_BACKEND_QP_RETRY_COUNT 7
#define VRDMA_BACKEND_QP_RNR_RETRY 7

struct snap_vrdma_backend_qp;

struct vrdma_backend_qp {
    LIST_ENTRY(vrdma_backend_qp) entry;
    struct ibv_pd *pd;
    union ibv_gid lgid_lip;
	union ibv_gid rgid_rip;
    uint32_t poller_core;
    struct snap_vrdma_backend_qp bk_qp;
    uint32_t remote_qpn;
	uint32_t remote_vqpn;
	uint32_t src_addr_idx;
    uint8_t dest_mac[6];
	uint8_t local_mac[6];
};

struct vrdma_local_bk_qp_attr {
	struct vrdma_bk_qp_connect comm;
	uint32_t core_id;
};
#define VRDMA_LOCAL_BK_QP_ATTR_SIZE sizeof(struct vrdma_local_bk_qp_attr)

struct vrdma_local_bk_qp {
	LIST_ENTRY(vrdma_local_bk_qp) entry;
	union {
		struct vrdma_local_bk_qp_attr attr;
		uint8_t data[VRDMA_LOCAL_BK_QP_ATTR_SIZE];
	};
	uint32_t bk_qpn;
	uint64_t remote_node_id;
	uint32_t remote_dev_id;
	uint32_t remote_qpn;
	uint64_t remote_gid_ip;
	struct vrdma_backend_qp *bk_qp;
};

struct vrdma_remote_bk_qp_attr {
	struct vrdma_bk_qp_connect comm;
};
#define VRDMA_REMOTE_BK_QP_ATTR_SIZE sizeof(struct vrdma_remote_bk_qp_attr)

struct vrdma_remote_bk_qp {
	LIST_ENTRY(vrdma_remote_bk_qp) entry;
	union {
		struct vrdma_remote_bk_qp_attr attr;
		uint8_t data[VRDMA_REMOTE_BK_QP_ATTR_SIZE];
	};
	uint32_t bk_qpn;
};

LIST_HEAD(vrdma_lbk_qp_list_head, vrdma_local_bk_qp);
LIST_HEAD(vrdma_rbk_qp_list_head, vrdma_remote_bk_qp);
extern struct vrdma_lbk_qp_list_head vrdma_lbk_qp_list;
extern struct vrdma_rbk_qp_list_head vrdma_rbk_qp_list;

struct spdk_vrdma_qp *
find_spdk_vrdma_qp_by_idx(struct vrdma_ctrl *ctrl, uint32_t qp_idx);
void vrdma_del_bk_qp_list(void);
int vrdma_add_rbk_qp_list(struct vrdma_ctrl *ctrl, uint64_t gid_ip,
		uint32_t vqp_idx, uint32_t remote_qpn,
		struct vrdma_remote_bk_qp_attr *qp_attr);
void vrdma_del_rbk_qp_from_list(struct vrdma_remote_bk_qp *rqp);
struct vrdma_remote_bk_qp *
vrdma_find_rbk_qp_by_vqp(uint64_t remote_gid_ip, uint32_t remote_vqpn);
struct vrdma_local_bk_qp *
vrdma_find_lbk_qp_by_vqp(uint64_t gid_ip, uint32_t vqp_idx);
int vrdma_qp_notify_remote_by_rpc(struct vrdma_ctrl *ctrl, uint32_t vqpn,
		uint32_t remote_vqpn, struct vrdma_backend_qp *bk_qp);
struct vrdma_backend_qp *
vrdma_create_backend_qp(struct vrdma_ctrl *ctrl,
				uint32_t vqp_idx, uint32_t remote_vqpn);
int vrdma_modify_backend_qp_to_ready(struct vrdma_ctrl *ctrl,
				struct vrdma_backend_qp *bk_qp);
void vrdma_destroy_backend_qp(struct vrdma_ctrl *ctrl, uint32_t vqp_idx);
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
#endif
