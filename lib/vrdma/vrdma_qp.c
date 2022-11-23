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

#include <infiniband/verbs.h>

#include "spdk/stdinc.h"
#include "spdk/log.h"
#include "spdk/env.h"
#include "snap.h"
#include "snap_vrdma_ctrl.h"
#include "snap_vrdma_virtq.h"

#include "spdk/vrdma_admq.h"
#include "spdk/vrdma_io_mgr.h"
#include "spdk/vrdma_controller.h"
#include "spdk/vrdma_qp.h"

int vrdma_create_backend_qp(struct vrdma_ctrl *ctrl,
				struct spdk_vrdma_qp *vqp)
{
	struct vrdma_backend_qp *qp;

	SPDK_NOTICELOG("\nlizh vrdma_create_backend_qp...start\n");
	qp = calloc(1, sizeof(*qp));
    if (!qp) {
		SPDK_ERRLOG("Failed to allocate backend QP memory");
		return -1;
	}
	qp->pd = vqp->vpd->ibpd;
	qp->poller_core = spdk_env_get_current_core();
	qp->remote_qpn = VRDMA_INVALID_QPN;
	qp->bk_qp.qp_attr.qp_type = SNAP_OBJ_DEVX;
	qp->bk_qp.qp_attr.sq_size = vqp->sq.comm.wqebb_cnt;
	//qp->bk_qp.qp_attr.sq_size = ctrl->sctrl->bar_curr->mtu ? ctrl->sctrl->bar_curr->mtu : IBV_MTU_2048;
	qp->bk_qp.qp_attr.sq_max_sge = 1;
	qp->bk_qp.qp_attr.sq_max_inline_size = 256;
	qp->bk_qp.qp_attr.rq_size = vqp->rq.comm.wqebb_cnt;
	qp->bk_qp.qp_attr.rq_max_sge = 1;
	if (snap_vrdma_create_qp_helper(qp->pd, &qp->bk_qp)) {
		SPDK_ERRLOG("Failed to create backend QP ");
		free(qp);
		return -1;
	}
	vqp->bk_qp[0] = qp;
	LIST_INSERT_HEAD(&ctrl->bk_qp_list, qp, entry);
	SPDK_NOTICELOG("\nlizh vrdma_create_backend_qp..mqpn 0x%x.done\n", qp->bk_qp.qpnum);
	return 0;
}

int vrdma_modify_backend_qp_to_ready(struct vrdma_ctrl *ctrl,
				struct spdk_vrdma_qp *vqp)
{
	struct snap_vrdma_bk_qp_rdy_attr rdy_attr = {0};
	struct ibv_qp_attr qp_attr = {0};
	struct snap_qp *sqp;
	int attr_mask;

	SPDK_NOTICELOG("\nlizh vrdma_modify_backend_qp_to_ready...start\n");
	/* Modify bankend QP to ready (rst2init + init2rtr + rtr2rts)*/
	sqp = vqp->bk_qp[0]->bk_qp.sqp;
	qp_attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ |
				IBV_ACCESS_REMOTE_ATOMIC | IBV_ACCESS_LOCAL_WRITE;
	attr_mask = IBV_QP_ACCESS_FLAGS;
	if (snap_vrdma_modify_bankend_qp_rst2init(sqp, &qp_attr, attr_mask)) {
		SPDK_ERRLOG("Failed to modify bankend QP reset to init");
		return -1;
	}

	attr_mask = IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
				IBV_QP_RQ_PSN | IBV_QP_MIN_RNR_TIMER;
	qp_attr.path_mtu = ctrl->sctrl->bar_curr->mtu > 1024 ?
				IBV_MTU_2048 : IBV_MTU_1024;
	qp_attr.dest_qp_num = vqp->bk_qp[0]->remote_qpn;
	if (qp_attr.dest_qp_num == VRDMA_INVALID_QPN) {
		SPDK_ERRLOG("Failed to modify bankend QP for invalid remote qpn");
		return -1;
	}
	qp_attr.rq_psn = 0;
	qp_attr.min_rnr_timer = 12;
	rdy_attr.dest_mac = vqp->bk_qp[0]->dest_mac;
	rdy_attr.rgid_rip = vqp->bk_qp[0]->rgid_rip;
	rdy_attr.src_addr_index = vqp->bk_qp[0]->src_addr_idx;
	if (snap_vrdma_modify_bankend_qp_init2rtr(sqp, &qp_attr, attr_mask, &rdy_attr)) {
		SPDK_ERRLOG("Failed to modify bankend QP init to RTR");
		return -1;
	}

	attr_mask = IBV_QP_SQ_PSN | IBV_QP_RETRY_CNT |
				IBV_QP_RNR_RETRY | IBV_QP_TIMEOUT;
	qp_attr.sq_psn = 0;
	qp_attr.retry_cnt = 8;
	qp_attr.rnr_retry = 8;
	qp_attr.timeout = 32;
	if (snap_vrdma_modify_bankend_qp_rtr2rts(sqp, &qp_attr, attr_mask)) {
		SPDK_ERRLOG("Failed to modify bankend QP RTR to RTS");
		return -1;
	}
	SPDK_NOTICELOG("\nlizh vrdma_modify_backend_qp_to_ready...done\n");
	return 0;
}

void vrdma_destroy_backend_qp(struct spdk_vrdma_qp *vqp)
{
	struct vrdma_backend_qp *qp;
	uint32_t i;

	for (i = 0; i < VRDMA_MAX_BK_QP_PER_VQP; i++) {
		if (vqp->bk_qp[i]) {
			qp = vqp->bk_qp[i];
			snap_vrdma_destroy_qp_helper(&qp->bk_qp);
			vqp->bk_qp[i] = NULL;
			vqp->bk_qp[i] = NULL;
			LIST_REMOVE(qp, entry);
			free(qp);
		}
	}
}

int vrdma_create_vq(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe,
				struct spdk_vrdma_qp *vqp,
				struct spdk_vrdma_cq *rq_vcq,
				struct spdk_vrdma_cq *sq_vcq)
{
	struct snap_vrdma_vq_create_attr q_attr;
	uint32_t rq_buff_size, sq_buff_size, q_buff_size;

	SPDK_NOTICELOG("\nlizh vrdma_create_vq...start\n");
	q_attr.bdev = NULL;
	q_attr.pd = ctrl->pd;
	q_attr.sq_size = VRDMA_MAX_DMA_SQ_SIZE_PER_VQP;
	q_attr.rq_size = VRDMA_MAX_DMA_RQ_SIZE_PER_VQP;
	q_attr.tx_elem_size = VRDMA_DMA_ELEM_SIZE;
	q_attr.rx_elem_size = VRDMA_DMA_ELEM_SIZE;
	q_attr.vqpn = vqp->qp_idx;
	vqp->snap_queue = ctrl->sctrl->q_ops->create(ctrl->sctrl, &q_attr);
	if (!vqp->snap_queue) {
		SPDK_ERRLOG("Failed to create qp dma queue");
		return -1;
	}
	vrdma_qp_sm_init(vqp);
	vqp->rq.comm.wqebb_size =
		VRDMA_QP_WQEBB_BASE_SIZE * (aqe->req.create_qp_req.rq_wqebb_size + 1);
	vqp->rq.comm.wqebb_cnt = 1 << aqe->req.create_qp_req.log_rq_wqebb_cnt;
	rq_buff_size = vqp->rq.comm.wqebb_size * vqp->rq.comm.wqebb_cnt;
	q_buff_size = sizeof(*vqp->qp_pi) + rq_buff_size;
	vqp->sq.comm.wqebb_size =
		VRDMA_QP_WQEBB_BASE_SIZE * (aqe->req.create_qp_req.sq_wqebb_size + 1);
	vqp->sq.comm.wqebb_cnt = 1 << aqe->req.create_qp_req.log_sq_wqebb_cnt;
	sq_buff_size = vqp->sq.comm.wqebb_size * vqp->sq.comm.wqebb_cnt;
	q_buff_size += sq_buff_size;

	vqp->qp_pi = spdk_malloc(q_buff_size, 0x10, NULL, SPDK_ENV_LCORE_ID_ANY,
                             SPDK_MALLOC_DMA);
    if (!vqp->qp_pi) {
		SPDK_ERRLOG("Failed to allocate wqe buff");
        goto destroy_dma;
    }
	vqp->rq.rq_buff = (struct vrdma_recv_wqe *)((uint8_t *)vqp->qp_pi + sizeof(*vqp->qp_pi));
	vqp->sq.sq_buff = (struct vrdma_send_wqe *)((uint8_t *)vqp->rq.rq_buff + rq_buff_size);
    vqp->qp_mr = ibv_reg_mr(ctrl->pd, vqp->qp_pi, q_buff_size,
                    IBV_ACCESS_REMOTE_READ |
                    IBV_ACCESS_REMOTE_WRITE |
                    IBV_ACCESS_LOCAL_WRITE);
    if (!vqp->qp_mr) {
		SPDK_ERRLOG("Failed to register qp_mr");
        goto free_wqe_buff;
    }
	vqp->rq.comm.wqe_buff_pa = aqe->req.create_qp_req.rq_l0_paddr;
	vqp->rq.comm.doorbell_pa = aqe->req.create_qp_req.rq_pi_paddr;
	vqp->rq.comm.log_pagesize = aqe->req.create_qp_req.log_rq_pagesize;
	vqp->rq.comm.hop = aqe->req.create_qp_req.rq_hop;
	vqp->sq.comm.wqe_buff_pa = aqe->req.create_qp_req.sq_l0_paddr;
	vqp->sq.comm.doorbell_pa = aqe->req.create_qp_req.sq_pi_paddr;
	vqp->sq.comm.log_pagesize = aqe->req.create_qp_req.log_sq_pagesize;
	vqp->sq.comm.hop = aqe->req.create_qp_req.sq_hop;
	SPDK_NOTICELOG("\nlizh vrdma_create_vq...done\n");
	return 0;

free_wqe_buff:
	spdk_free(vqp->qp_pi);
destroy_dma:
	ctrl->sctrl->q_ops->destroy(ctrl->sctrl, vqp->snap_queue);
	return -1;
}

bool vrdma_set_vq_flush(struct vrdma_ctrl *ctrl,
				struct spdk_vrdma_qp *vqp)
{
	if (ctrl->sctrl->q_ops->is_suspended(vqp->snap_queue))
		return false;
	ctrl->sctrl->q_ops->suspend(vqp->snap_queue);
	return true;
}

void vrdma_destroy_vq(struct vrdma_ctrl *ctrl,
				struct spdk_vrdma_qp *vqp)
{
	ctrl->sctrl->q_ops->destroy(ctrl->sctrl, vqp->snap_queue);
	if (vqp->qp_mr) {
		ibv_dereg_mr(vqp->qp_mr);
		vqp->qp_mr = NULL;
	}
	if (vqp->qp_pi) {
		spdk_free(vqp->qp_pi);
		vqp->qp_pi = NULL;
		vqp->rq.rq_buff = NULL;
		vqp->sq.sq_buff = NULL;
	}
}

bool vrdma_qp_is_suspended(struct vrdma_ctrl *ctrl, uint32_t qp_handle)
{
	struct spdk_vrdma_qp *vqp;
	
	vqp = find_spdk_vrdma_qp_by_idx(ctrl, qp_handle);
	if (!vqp) {
		SPDK_ERRLOG("Failed to find QP %d in waiting qp suspended progress",
			qp_handle);
		return false;
	}
	return ctrl->sctrl->q_ops->is_suspended(vqp->snap_queue);
}
