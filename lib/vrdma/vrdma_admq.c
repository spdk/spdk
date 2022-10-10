/*-
 *   BSD LICENSE
 *
 *   Copyright © 2021 NVIDIA CORPORATION & AFFILIATES. ALL RIGHTS RESERVED.
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

#include "spdk/stdinc.h"

#include "spdk/env.h"
#include "spdk/barrier.h"
#include "spdk/vrdma_admq.h"
#include "spdk/vrdma_controller.h"
#include "snap_vrdma_ctrl.h"

static inline int aqe_sanity_check(struct vrdma_admin_cmd_entry *aqe)
{
	if (!aqe) {
		return -1;
	}
	
	if (aqe->hdr.magic != VRDMA_AQ_HDR_MEGIC_NUM) { 
		return -1;
	}
	//TODO: add other sanity check later

	return 0;

}

static int vrdma_aq_open_dev(struct vrdma_ctrl *ctrl,
								struct vrdma_admin_cmd_entry *aqe)
{
	/* No action and just return OK*/
	aqe->resp.open_device_resp.err_code = aq_msg_err_code_success;
	return 0;
}


static int vrdma_aq_query_dev(struct vrdma_ctrl *ctrl,
								struct vrdma_admin_cmd_entry *aqe)
{

	struct snap_device *sdev = ctrl->sctrl->sdev;
	const char fw_ver[] = "Unkown";

	memcpy(aqe->resp.query_device_resp.fw_ver, fw_ver, strlen(fw_ver));
	aqe->resp.query_device_resp.dev_cap_flags = VRDMA_DEVICE_RC_RNR_NAK_GEN;
	aqe->resp.query_device_resp.vendor_id = sdev->pci->pci_attr.vendor_id;
	aqe->resp.query_device_resp.hw_ver = sdev->pci->pci_attr.revision_id;
	aqe->resp.query_device_resp.max_pd = 1 << ctrl->sctx->vrdma_caps.log_max_pd;
	aqe->resp.query_device_resp.max_qp = VRDMA_DEV_MAX_QP;
	aqe->resp.query_device_resp.max_qp_wr = VRDMA_DEV_MAX_QP_SZ;
	aqe->resp.query_device_resp.max_cq = VRDMA_DEV_MAX_CQ;
	aqe->resp.query_device_resp.max_sq_depth = VRDMA_DEV_MAX_SQ_DP;
	aqe->resp.query_device_resp.max_rq_depth = VRDMA_DEV_MAX_RQ_DP;
	aqe->resp.query_device_resp.max_cq_depth = VRDMA_DEV_MAX_CQ_DP;
	aqe->resp.query_device_resp.max_mr = 1 << ctrl->sctx->vrdma_caps.log_max_mkey;
	aqe->resp.query_device_resp.err_code = aq_msg_err_code_success;
	return 0;
}

static int vrdma_aq_query_port(struct vrdma_ctrl *ctrl,
								struct vrdma_admin_cmd_entry *aqe)
{
	//TODO
	return 0;
}

static int vrdma_aq_query_gid(struct vrdma_ctrl *ctrl,
							struct vrdma_admin_cmd_entry *aqe)
{
	//TODO
	return 0;
}

static int vrdma_aq_modify_gid(struct vrdma_ctrl *ctrl,
							struct vrdma_admin_cmd_entry *aqe)
{
	//TODO
	return 0;
}

static int vrdma_aq_create_pd(struct vrdma_ctrl *ctrl,
							struct vrdma_admin_cmd_entry *aqe)
{
	//TODO
	return 0;
}

static int vrdma_aq_destroy_pd(struct vrdma_ctrl *ctrl,
							struct vrdma_admin_cmd_entry *aqe)
{
	//TODO
	return 0;
}

static int vrdma_aq_reg_mr(struct vrdma_ctrl *ctrl,
							struct vrdma_admin_cmd_entry *aqe)
{
	//TODO
	return 0;
}

static int vrdma_aq_dereg_mr(struct vrdma_ctrl *ctrl,
							struct vrdma_admin_cmd_entry *aqe)
{
	//TODO
	return 0;
}

static int vrdma_aq_create_cq(struct vrdma_ctrl *ctrl,
							struct vrdma_admin_cmd_entry *aqe)
{
	//TODO
	return 0;
}

static int vrdma_aq_destroy_cq(struct vrdma_ctrl *ctrl,
							struct vrdma_admin_cmd_entry *aqe)
{
	//TODO
	return 0;
}

static int vrdma_aq_create_qp(struct vrdma_ctrl *ctrl,
							struct vrdma_admin_cmd_entry *aqe)
{
	//TODO
	return 0;
}

static int vrdma_aq_destroy_qp(struct vrdma_ctrl *ctrl,
							struct vrdma_admin_cmd_entry *aqe)
{
	//TODO
	return 0;
}

static int vrdma_aq_query_qp(struct vrdma_ctrl *ctrl,
							struct vrdma_admin_cmd_entry *aqe)
{
	//TODO
	return 0;
}

static int vrdma_aq_modify_qp(struct vrdma_ctrl *ctrl,
							struct vrdma_admin_cmd_entry *aqe)
{
	//TODO
	return 0;
}

static int vrdma_aq_create_ceq(struct vrdma_ctrl *ctrl,
							struct vrdma_admin_cmd_entry *aqe)
{
	//TODO
	return 0;
}

static int vrdma_aq_modify_ceq(struct vrdma_ctrl *ctrl,
							struct vrdma_admin_cmd_entry *aqe)
{
	//TODO
	return 0;
}

static int vrdma_aq_destroy_ceq(struct vrdma_ctrl *ctrl,
							struct vrdma_admin_cmd_entry *aqe)
{
	//TODO
	return 0;
}

static int vrdma_aq_create_ah(struct vrdma_ctrl *ctrl,
							struct vrdma_admin_cmd_entry *aqe)
{
	//TODO
	return 0;
}

static int vrdma_aq_destroy_ah(struct vrdma_ctrl *ctrl,
							struct vrdma_admin_cmd_entry *aqe)
{
	//TODO
	return 0;
}

int vrdma_parse_admq_entry(struct vrdma_ctrl *ctrl,
			struct vrdma_admin_cmd_entry *aqe)
{
	int ret = 0;
	
	if (!ctrl || aqe_sanity_check(aqe)) {
		return -1;
	}

	switch (aqe->hdr.opcode) {
			case VRDMA_ADMIN_OPEN_DEVICE:
				ret = vrdma_aq_open_dev(ctrl, aqe);
				break;
			case VRDMA_ADMIN_QUERY_DEVICE:
				ret = vrdma_aq_query_dev(ctrl, aqe);
				break;
			case VRDMA_ADMIN_QUERY_PORT:
				ret = vrdma_aq_query_port(ctrl, aqe);
				break;
			case VRDMA_ADMIN_QUERY_GID:
				ret = vrdma_aq_query_gid(ctrl, aqe);
				break;
			case VRDMA_ADMIN_MODIFY_GID:
				ret = vrdma_aq_modify_gid(ctrl, aqe);
				break;
			case VRDMA_ADMIN_CREATE_PD:
				ret = vrdma_aq_create_pd(ctrl, aqe);
				break;
			case VRDMA_ADMIN_DESTROY_PD:
				ret = vrdma_aq_destroy_pd(ctrl, aqe);
				break;
			case VRDMA_ADMIN_REG_MR:
				ret = vrdma_aq_reg_mr(ctrl, aqe);
				break;
			case VRDMA_ADMIN_DEREG_MR:
				ret = vrdma_aq_dereg_mr(ctrl, aqe);
				break;
			case VRDMA_ADMIN_CREATE_CQ:
				ret = vrdma_aq_create_cq(ctrl, aqe);
				break;
			case VRDMA_ADMIN_DESTROY_CQ:
				ret = vrdma_aq_destroy_cq(ctrl, aqe);
				break;
			case VRDMA_ADMIN_CREATE_QP:
				ret = vrdma_aq_create_qp(ctrl, aqe);
				break;
			case VRDMA_ADMIN_DESTROY_QP:
				ret = vrdma_aq_destroy_qp(ctrl, aqe);
				break;
			case VRDMA_ADMIN_QUERY_QP:
				ret = vrdma_aq_query_qp(ctrl, aqe);
				break;
			case VRDMA_ADMIN_MODIFY_QP:
				ret = vrdma_aq_modify_qp(ctrl, aqe);
				break;
			case VRDMA_ADMIN_CREATE_CEQ:
				ret = vrdma_aq_create_ceq(ctrl, aqe);
				break;
			case VRDMA_ADMIN_MODIFY_CEQ:
				ret = vrdma_aq_modify_ceq(ctrl, aqe);
				break;
			case VRDMA_ADMIN_DESTROY_CEQ:
				ret = vrdma_aq_destroy_ceq(ctrl, aqe);
				break;
			case VRDMA_ADMIN_CREATE_AH:
				ret = vrdma_aq_create_ah(ctrl, aqe);
				break;
			case VRDMA_ADMIN_DESTROY_AH:
				ret = vrdma_aq_destroy_ah(ctrl, aqe);
				break;
			default:
				return -1;		
	}

	return ret;
}
