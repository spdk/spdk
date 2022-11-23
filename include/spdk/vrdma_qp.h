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
#include "vrdma_controller.h"
#include "snap_vrdma_virtq.h"

#define VRDMA_INVALID_QPN 0xFFFFFFFF

struct snap_vrdma_backend_qp;

struct vrdma_backend_qp {
    LIST_ENTRY(vrdma_backend_qp) entry;
    struct ibv_pd *pd;
    union ibv_gid rgid_rip;
    uint32_t poller_core;
    struct snap_vrdma_backend_qp bk_qp;
    uint32_t remote_qpn;
	uint32_t src_addr_idx;
    uint8_t dest_mac[6];
};

int vrdma_create_backend_qp(struct vrdma_ctrl *ctrl,
				struct spdk_vrdma_qp *vqp);
int vrdma_modify_backend_qp_to_ready(struct vrdma_ctrl *ctrl,
				struct spdk_vrdma_qp *vqp);
void vrdma_destroy_backend_qp(struct spdk_vrdma_qp *vqp);
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
#endif
