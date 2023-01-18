/*-
 *   BSD LICENSE
 *
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
#include "spdk/env.h"
#include "spdk/conf.h"
#include "spdk/log.h"
#include "spdk/vrdma_admq.h"
#include "spdk/vrdma_srv.h"
#include "spdk/vrdma_controller.h"
#include "spdk/vrdma_admq.h"

struct vrdma_srv_qp_list_head srv_qp_list =
                              LIST_HEAD_INITIALIZER(srv_qp_list);

static int vrdma_srv_device_notify(struct vrdma_dev *rdev)
{
	//TODO
	return 0;
}

static int vrdma_srv_device_open_device(struct vrdma_dev *rdev, 
					struct vrdma_admin_cmd_entry *cmd)
{
	//TODO
	return 0;
}

static int vrdma_srv_device_query_device(struct vrdma_dev *rdev, 
					struct vrdma_admin_cmd_entry *cmd)
{
	//TODO
	return 0;
}

static int vrdma_srv_device_query_port(struct vrdma_dev *rdev, 
					struct vrdma_admin_cmd_entry *cmd)
{
	//TODO
	return 0;
}

static int vrdma_srv_device_query_gid(struct vrdma_dev *rdev, 
					struct vrdma_admin_cmd_entry *cmd)
{
	//TODO
	return 0;
}

static int vrdma_srv_device_modify_gid(struct vrdma_dev *rdev, 
					struct vrdma_admin_cmd_entry *cmd,
					struct vrdma_cmd_param *param)
{
	//TODO
	return 0;
}

static int vrdma_srv_device_create_eq(struct vrdma_dev *rdev, 
					struct vrdma_admin_cmd_entry *cmd,
					struct vrdma_cmd_param *param)
{
	//TODO
	return 0;
}

static int vrdma_srv_device_modify_eq(struct vrdma_dev *rdev, 
					struct vrdma_admin_cmd_entry *cmd)
{
	//TODO
	return 0;
}

static int vrdma_srv_device_destroy_eq(struct vrdma_dev *rdev, 
					struct vrdma_admin_cmd_entry *cmd)
{
	//TODO
	return 0;
}

static int vrdma_srv_device_create_pd(struct vrdma_dev *rdev, 
					struct vrdma_admin_cmd_entry *cmd, 
					struct vrdma_cmd_param *param)
{
	//TODO
	return 0;
}

static int vrdma_srv_device_destroy_pd(struct vrdma_dev *rdev, 
					struct vrdma_admin_cmd_entry *cmd)
{
	//TODO
	return 0;
}

static int vrdma_srv_device_create_mr(struct vrdma_dev *rdev, 
					struct vrdma_admin_cmd_entry *cmd, 
					struct vrdma_cmd_param *param)
{
	//TODO
	return 0;
}

static int vrdma_srv_device_destroy_mr(struct vrdma_dev *rdev, 
					struct vrdma_admin_cmd_entry *cmd, 
					struct vrdma_cmd_param *param)
{
	//TODO
	return 0;
}

static int vrdma_srv_device_create_cq(struct vrdma_dev *rdev, 
					struct vrdma_admin_cmd_entry *cmd, 
					struct vrdma_cmd_param *param)
{
	//TODO
	return 0;
}

static int vrdma_srv_device_destroy_cq(struct vrdma_dev *rdev, 
					struct vrdma_admin_cmd_entry *cmd)
{
	//TODO
	return 0;
}

static struct vrdma_srv_qp *vrdma_srv_device_find_qp(uint32_t qp_idx)
{
	struct vrdma_srv_qp *vqp, *vqp_tmp;

	LIST_FOREACH_SAFE(vqp, &srv_qp_list, entry, vqp_tmp) {
    	if (vqp->qp_idx == qp_idx)
            return vqp;
	}
	return NULL;
}

static int vrdma_srv_device_create_qp(struct vrdma_dev *rdev, 
					struct vrdma_admin_cmd_entry *cmd, 
					struct vrdma_cmd_param *param)
{
	struct vrdma_srv_qp *vqp;

	vqp = calloc(1, sizeof(*vqp));
    if (!vqp) {
		SPDK_ERRLOG("Failed to allocate QP memory in service\n");
		return -1;
	}
	vqp->qp_idx = param->param.create_qp_param.qp_handle;
	vqp->pd = param->param.create_qp_param.ibpd;
	vqp->sq_size = 1 << cmd->req.create_qp_req.log_sq_wqebb_cnt;
	vqp->rq_size = 1 << cmd->req.create_qp_req.log_rq_wqebb_cnt;
	LIST_INSERT_HEAD(&srv_qp_list, vqp, entry);
	//TODO
	return 0;
}

static int vrdma_srv_device_destroy_qp(struct vrdma_dev *rdev, 
					struct vrdma_admin_cmd_entry *cmd)
{
	struct vrdma_srv_qp *vqp = NULL;
	uint32_t vqpn = cmd->req.destroy_qp_req.qp_handle;

	vqp = vrdma_srv_device_find_qp(vqpn);
	if (!vqp) {
		SPDK_ERRLOG("Failed to find QP for destroy in service\n");
		return -1;
	}
	if (vrdma_srv_unbind_channel(rdev, vqpn)) {
		SPDK_ERRLOG("Failed to unbind channel vqpn %d for destroy in service\n", vqpn);
		return -1;
	}
	LIST_REMOVE(vqp, entry);
	free(vqp);
	return 0;
}

static int vrdma_srv_device_query_qp(struct vrdma_dev *rdev, 
					struct vrdma_admin_cmd_entry *cmd)
{
	//TODO
	return 0;
}

int vrdma_srv_bind_channel(struct vrdma_dev *rdev, union ibv_gid *v_rgid, struct ibv_pd *pd,
							enum ibv_qp_state qp_state, uint32_t vqpn, uint32_t remote_vqpn)
{
	struct vrdma_srv_qp *vqp;
	struct vrdma_ctrl *ctrl;

	vqp = vrdma_srv_device_find_qp(vqpn);
	if (!vqp) {
		SPDK_ERRLOG("Failed to find qp for modify in service\n");
		return -1;
	}
	ctrl = vrdma_find_ctrl_by_srv_dev(rdev);
	if (!ctrl) {
		SPDK_ERRLOG("Failed to find controller for modify qp in service\n");
		return -1;
	}
	vqp->bk_qp[0] = vrdma_create_backend_qp(ctrl, vqpn, remote_vqpn);
	if (!vqp->bk_qp[0]) {
		SPDK_ERRLOG("Failed to create bankend qp %d\n",vqpn);
		return -1;
	}
	if (vrdma_qp_notify_remote_by_rpc(ctrl, vqpn, remote_vqpn, vqp->bk_qp[0])) {
        SPDK_ERRLOG("Fail to notify qp %d to send rpc\n", vqpn);
		goto destroy_bk_qp;
    }
	return 0;

destroy_bk_qp:
	vrdma_destroy_backend_qp(ctrl, vqpn);
	return -1;
}

int vrdma_srv_unbind_channel(struct vrdma_dev *rdev, uint32_t vqpn)
{
	struct vrdma_ctrl *ctrl;

	ctrl = vrdma_find_ctrl_by_srv_dev(rdev);
	if (!ctrl) {
		SPDK_ERRLOG("Failed to find controller for modify qp in service\n");
		return -1;
	}
	vrdma_destroy_backend_qp(ctrl, vqpn);
	return 0;
}

int vrdma_srv_map_backend_mqp(uint32_t vqpn, struct vrdma_backend_qp *bk_qp)
{
	//TODO
	return 0;	
}

static int vrdma_srv_device_modify_qp(struct vrdma_dev *rdev, 
					struct vrdma_admin_cmd_entry *cmd)
{
	struct vrdma_srv_qp *vqp;

	vqp = vrdma_srv_device_find_qp(cmd->req.modify_qp_req.qp_handle);
	if (!vqp) {
		SPDK_ERRLOG("Failed to find qp for modify in service\n");
		return -1;
	}
	SPDK_NOTICELOG("\nvrdma service device modify vqpn %d old qp_state %d new qp_state %d \n",
	cmd->req.modify_qp_req.qp_handle, vqp->qp_state, cmd->req.modify_qp_req.qp_state);
	if (vqp->qp_state == IBV_QPS_INIT &&
		cmd->req.modify_qp_req.qp_state == IBV_QPS_RTR) {
		/* For POC, it hardcode v_rgid to NULL, as only one remote node.*/
		vqp->remote_vqpn = cmd->req.modify_qp_req.dest_qp_num;
		if (vrdma_srv_bind_channel(rdev, NULL, vqp->pd,
					cmd->req.modify_qp_req.qp_state, vqp->qp_idx, vqp->remote_vqpn)) {
			SPDK_ERRLOG("Failed to bind channel for modify qp in service\n");
			return -1;
		}
	}
	vqp->qp_state = cmd->req.modify_qp_req.qp_state;
	return 0;
}

static int vrdma_srv_device_create_ah(struct vrdma_dev *rdev, 
					struct vrdma_admin_cmd_entry *cmd,
					struct vrdma_cmd_param *param)
{
	//TODO
	return 0;
}

static int vrdma_srv_device_destroy_ah(struct vrdma_dev *rdev, 
					struct vrdma_admin_cmd_entry *cmd)
{
	//TODO
	return 0;
}

static int vrdma_srv_map_backend_qp(uint32_t vqpn,
						struct vrdma_backend_qp *bk_qp)
{
	//TODO
	return 0;
}
static const struct vRdmaServiceOps vrdma_srv_ops = {
	.vrdma_device_notify = vrdma_srv_device_notify,
	.vrdma_device_open_device = vrdma_srv_device_open_device,
	.vrdma_device_query_device = vrdma_srv_device_query_device,
	.vrdma_device_query_port = vrdma_srv_device_query_port,
	.vrdma_device_query_gid = vrdma_srv_device_query_gid,
	.vrdma_device_modify_gid = vrdma_srv_device_modify_gid,
	.vrdma_device_create_eq = vrdma_srv_device_create_eq,
	.vrdma_device_modify_eq = vrdma_srv_device_modify_eq,
	.vrdma_device_destroy_eq = vrdma_srv_device_destroy_eq,
	.vrdma_device_create_pd = vrdma_srv_device_create_pd,
	.vrdma_device_destroy_pd = vrdma_srv_device_destroy_pd,
	.vrdma_device_create_mr = vrdma_srv_device_create_mr,
	.vrdma_device_destroy_mr = vrdma_srv_device_destroy_mr,
	.vrdma_device_create_cq = vrdma_srv_device_create_cq,
	.vrdma_device_destroy_cq = vrdma_srv_device_destroy_cq,
	.vrdma_device_create_qp = vrdma_srv_device_create_qp,
	.vrdma_device_destroy_qp = vrdma_srv_device_destroy_qp,
	.vrdma_device_query_qp = vrdma_srv_device_query_qp,
	.vrdma_device_modify_qp = vrdma_srv_device_modify_qp,
	.vrdma_device_create_ah = vrdma_srv_device_create_ah,
	.vrdma_device_destroy_ah = vrdma_srv_device_destroy_ah,
	.vrdma_device_map_backend_qp = vrdma_srv_map_backend_qp,
};

void vrdma_srv_device_init(struct vrdma_ctrl *ctrl)
{
	ctrl->srv_ops = &vrdma_srv_ops;
}
