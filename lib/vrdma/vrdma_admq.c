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
#include "snap.h"
#include "snap_vrdma_ctrl.h"
#include "snap_vrdma_virtq.h"

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/bit_array.h"
#include "spdk/barrier.h"
#include "spdk/conf.h"
#include "spdk/log.h"
#include "spdk/likely.h"

#include "spdk/vrdma_admq.h"
#include "spdk/vrdma_srv.h"
#include "spdk/vrdma_controller.h"
#include "spdk/vrdma_emu_mgr.h"
#include "spdk/vrdma_io_mgr.h"

static uint32_t g_vpd_cnt;
static uint32_t g_vmr_cnt;
static uint32_t g_vah_cnt;
static uint32_t g_vqp_cnt;
static uint32_t g_vcq_cnt;
static uint32_t g_veq_cnt;

struct spdk_bit_array *free_vpd_ids;
struct spdk_bit_array *free_vmr_ids;
struct spdk_bit_array *free_vah_ids;
struct spdk_bit_array *free_vqp_ids;
struct spdk_bit_array *free_vcq_ids;
struct spdk_bit_array *free_veq_ids;

static struct spdk_bit_array *
spdk_vrdma_create_id_pool(uint32_t max_num)
{
	struct spdk_bit_array *free_ids;
	uint32_t i;

	free_ids = spdk_bit_array_create(max_num + 1);
	if (!free_ids)
		return NULL;
	for (i = 0; i <= max_num; i++)
		spdk_bit_array_clear(free_ids, i);
	return free_ids;
}

static int spdk_vrdma_init_all_id_pool(void)
{
	free_vpd_ids = spdk_vrdma_create_id_pool(VRDMA_MAX_PD_NUM);
	if (!free_vpd_ids)
		return -1;
	free_vmr_ids = spdk_vrdma_create_id_pool(VRDMA_MAX_MR_NUM);
	if (!free_vmr_ids)
		return -1;
	free_vah_ids = spdk_vrdma_create_id_pool(VRDMA_MAX_AH_NUM);
	if (!free_vah_ids)
		return -1;
	free_vqp_ids = spdk_vrdma_create_id_pool(VRDMA_MAX_QP_NUM);
	if (!free_vqp_ids)
		return -1;
	free_vcq_ids = spdk_vrdma_create_id_pool(VRDMA_MAX_CQ_NUM);
	if (!free_vcq_ids)
		return -1;
	free_veq_ids = spdk_vrdma_create_id_pool(VRDMA_MAX_EQ_NUM);
	if (!free_veq_ids)
		return -1;
	return 0;
}

int spdk_vrdma_adminq_resource_init(void)
{
	g_vpd_cnt = 0;
	g_vqp_cnt = 0;
	g_vcq_cnt = 0;
	g_veq_cnt = 0;
	g_vmr_cnt = 0;
	g_vah_cnt = 0;
	if (spdk_vrdma_init_all_id_pool())
		return -1;
	return 0;
}

void spdk_vrdma_adminq_resource_destory(void)
{
	spdk_bit_array_free(&free_vpd_ids);
	spdk_bit_array_free(&free_vmr_ids);
	spdk_bit_array_free(&free_vah_ids);
	spdk_bit_array_free(&free_vqp_ids);
	spdk_bit_array_free(&free_vcq_ids);
	spdk_bit_array_free(&free_veq_ids);
}

struct spdk_vrdma_pd *
find_spdk_vrdma_pd_by_idx(struct vrdma_ctrl *ctrl, uint32_t pd_idx)
{
	struct spdk_vrdma_pd *vpd = NULL;

	LIST_FOREACH(vpd, &ctrl->vdev->vpd_list, entry)
        if (vpd->pd_idx == pd_idx)
            break;
	return vpd;
}

struct spdk_vrdma_mr *
find_spdk_vrdma_mr_by_idx(struct vrdma_ctrl *ctrl, uint32_t mr_idx)
{
	struct spdk_vrdma_mr *vmr = NULL;

	LIST_FOREACH(vmr, &ctrl->vdev->vmr_list, entry)
        if (vmr->mr_idx == mr_idx)
            break;
	return vmr;
}

struct spdk_vrdma_mr *
find_spdk_vrdma_mr_by_key(struct vrdma_ctrl *ctrl, uint32_t key)
{
	struct spdk_vrdma_mr *vmr = NULL;

	LIST_FOREACH(vmr, &ctrl->vdev->vmr_list, entry)
        if (vmr->mr_log.mkey == key)
            break;
	return vmr;
}

struct spdk_vrdma_ah *
find_spdk_vrdma_ah_by_idx(struct vrdma_ctrl *ctrl, uint32_t ah_idx)
{
	struct spdk_vrdma_ah *vah = NULL;

	LIST_FOREACH(vah, &ctrl->vdev->vah_list, entry)
        if (vah->ah_idx == ah_idx)
            break;
	return vah;
}

struct spdk_vrdma_qp *
find_spdk_vrdma_qp_by_idx(struct vrdma_ctrl *ctrl, uint32_t qp_idx)
{
	struct spdk_vrdma_qp *vqp = NULL;

	LIST_FOREACH(vqp, &ctrl->vdev->vqp_list, entry)
        if (vqp->qp_idx == qp_idx)
            break;
	return vqp;
}

struct spdk_vrdma_cq *
find_spdk_vrdma_cq_by_idx(struct vrdma_ctrl *ctrl, uint32_t cq_idx)
{
	struct spdk_vrdma_cq *vcq = NULL;

	LIST_FOREACH(vcq, &ctrl->vdev->vcq_list, entry)
        if (vcq->cq_idx == cq_idx)
            break;
	return vcq;
}

struct spdk_vrdma_eq *
find_spdk_vrdma_eq_by_idx(struct vrdma_ctrl *ctrl, uint32_t eq_idx)
{
	struct spdk_vrdma_eq *veq = NULL;

	LIST_FOREACH(veq, &ctrl->vdev->veq_list, entry)
        if (veq->eq_idx == eq_idx)
            break;
	return veq;
}

static inline int aqe_sanity_check(struct vrdma_admin_cmd_entry *aqe)
{
	if (!aqe) {
		SPDK_ERRLOG("check input aqe NULL\n");
		return -1;
	}
	
	if (aqe->hdr.magic != VRDMA_AQ_HDR_MEGIC_NUM) { 
		SPDK_ERRLOG("check input aqe wrong megic num\n");
		return -1;
	}
	if (aqe->hdr.is_inline_out || !aqe->hdr.is_inline_in) {
		/* It only supports inline message */
		SPDK_ERRLOG("check input aqe wrong inline flag\n");
		return -1;
	}
	//TODO: add other sanity check later

	return 0;

}

static void vrdma_ctrl_dev_init(struct vrdma_ctrl *ctrl)
{
	struct snap_device *sdev = ctrl->sctrl->sdev;
	struct snap_vrdma_device_attr vattr = {};

	if (ctrl->dev_inited)
		return;
	if (snap_vrdma_query_device(sdev, &vattr))
		return;
    memcpy(ctrl->dev.gid, &vattr.mac, sizeof(uint64_t));
	memcpy(ctrl->dev.mac, &vattr.mac, sizeof(uint64_t));
	ctrl->dev.state = vattr.status;
	ctrl->dev_inited = 1;
}

static void vrdma_aq_open_dev(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe)
{
	SPDK_NOTICELOG("\nlizh vrdma_aq_open_dev...start\n");
	if (ctrl->srv_ops->vrdma_device_notify(&ctrl->dev)) {
		aqe->resp.open_device_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_SERVICE_FAIL;
		return;
	}
	aqe->resp.open_device_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_SUCCESS;
	SPDK_NOTICELOG("\nlizh vrdma_aq_open_dev...done\n");
}

static void vrdma_aq_query_dev(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe)
{

	struct snap_device *sdev = ctrl->sctrl->sdev;
	const char fw_ver[] = "Unkown";

	SPDK_NOTICELOG("\nlizh vrdma_aq_query_dev...start\n");
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
	aqe->resp.query_device_resp.max_ah = VRDMA_DEV_MAX_AH;
	aqe->resp.query_device_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_SUCCESS;
}

static void vrdma_aq_query_port(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe)
{
	SPDK_NOTICELOG("\nlizh vrdma_aq_query_port...start\n");
	aqe->resp.query_port_resp.state = IBV_PORT_ACTIVE; /* hardcode just for POC*/
	aqe->resp.query_port_resp.max_mtu = ctrl->sctrl->bar_curr->mtu;
	aqe->resp.query_port_resp.active_mtu = ctrl->sctrl->bar_curr->mtu;
	aqe->resp.query_port_resp.gid_tbl_len = 1;/* hardcode just for POC*/
	aqe->resp.query_port_resp.max_msg_sz = 1 << ctrl->sctx->vrdma_caps.log_max_msg;
	aqe->resp.query_port_resp.sm_lid = 0xFFFF; /*IB_LID_PERMISSIVE*/
	aqe->resp.query_port_resp.lid = 0xFFFF;
	aqe->resp.query_port_resp.pkey_tbl_len = 1;/* hardcode just for POC*/
	aqe->resp.query_port_resp.active_speed = 16; /* FDR hardcode just for POC*/
	aqe->resp.query_port_resp.phys_state = VRDMA_PORT_PHYS_STATE_LINK_UP;
	aqe->resp.query_port_resp.link_layer = IBV_LINK_LAYER_INFINIBAND;
	aqe->resp.query_port_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_SUCCESS;
}

static void vrdma_aq_query_gid(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe)
{
	SPDK_NOTICELOG("\nlizh vrdma_aq_query_gid...start\n");
	if (ctrl->srv_ops->vrdma_device_query_gid(&ctrl->dev, aqe)) {
		aqe->resp.query_gid_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_SERVICE_FAIL;
		return;
	}
	memcpy(aqe->resp.query_gid_resp.gid, ctrl->dev.gid, VRDMA_DEV_GID_LEN);
	aqe->resp.query_gid_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_SUCCESS;
	for (int i = 0; i < VRDMA_DEV_GID_LEN; i += 4)
		SPDK_NOTICELOG("\nlizh vrdma_aq_query_gid...gid=0x%x%x%x%x\n",
			aqe->resp.query_gid_resp.gid[i], aqe->resp.query_gid_resp.gid[i+1],
			aqe->resp.query_gid_resp.gid[i+2], aqe->resp.query_gid_resp.gid[i+3]);
}

static void vrdma_aq_modify_gid(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe)
{
	struct vrdma_cmd_param param;
	
	SPDK_NOTICELOG("\nlizh vrdma_aq_modify_gid...start\n");
	memcpy(param.param.modify_gid_param.gid, aqe->req.modify_gid_req.gid,
			VRDMA_DEV_GID_LEN);
	if (ctrl->srv_ops->vrdma_device_modify_gid(&ctrl->dev, aqe, &param)) {
		aqe->resp.modify_gid_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_SERVICE_FAIL;
		return;
	}
	memcpy(ctrl->dev.gid, aqe->req.modify_gid_req.gid, VRDMA_DEV_GID_LEN);
	aqe->resp.modify_gid_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_SUCCESS;
	for (int i = 0; i < VRDMA_DEV_GID_LEN; i += 4)
		SPDK_NOTICELOG("\nlizh vrdma_aq_modify_gid...gid=0x%x%x%x%x\n",
			aqe->resp.query_gid_resp.gid[i], aqe->resp.query_gid_resp.gid[i+1],
			aqe->resp.query_gid_resp.gid[i+2], aqe->resp.query_gid_resp.gid[i+3]);
}

static void vrdma_aq_create_pd(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe)
{
	struct vrdma_cmd_param param;
	struct spdk_vrdma_pd *vpd;
	uint32_t pd_idx;

	SPDK_NOTICELOG("\nlizh vrdma_aq_create_pd...start\n");
	if (g_vpd_cnt > VRDMA_MAX_PD_NUM ||
		!ctrl->vdev ||
		ctrl->vdev->vpd_cnt > VRDMA_DEV_MAX_PD) {
		aqe->resp.create_pd_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_EXCEED_MAX;
		SPDK_ERRLOG("Failed to create PD, err(%d)",
				  aqe->resp.create_pd_resp.err_code);
		return;
	}
	pd_idx = spdk_bit_array_find_first_clear(free_vpd_ids, 0);
	if (pd_idx == UINT32_MAX) {
		aqe->resp.create_pd_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_NO_MEM;
		SPDK_ERRLOG("Failed to allocate PD index, err(%d)",
				  aqe->resp.create_pd_resp.err_code);
		return;
	}
    vpd = calloc(1, sizeof(*vpd));
    if (!vpd) {
		aqe->resp.create_pd_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_NO_MEM;
		SPDK_ERRLOG("Failed to allocate PD memory, err(%d)",
				  aqe->resp.create_pd_resp.err_code);
		return;
	}
	
	vpd->ibpd = ibv_alloc_pd(ctrl->sctx->context);
	if (!vpd->ibpd) {
		aqe->resp.create_pd_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_NO_MEM;
		SPDK_ERRLOG("Failed to allocate PD, err(%d)",
				  aqe->resp.create_pd_resp.err_code);
		goto free_vpd;
	}

	/* Create mlnx-qp pool with mlnx-cq/eq and hardcode remote-qpn */

	param.param.create_pd_param.pd_handle = pd_idx;
	if (ctrl->srv_ops->vrdma_device_create_pd(&ctrl->dev, aqe, &param)) {
		aqe->resp.create_pd_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_SERVICE_FAIL;
		SPDK_ERRLOG("Failed to notify PD in service, err(%d)",
				  aqe->resp.create_pd_resp.err_code);
		goto free_ibpd;
	}
	g_vpd_cnt++;
	ctrl->vdev->vpd_cnt++;
	vpd->pd_idx = pd_idx;
	spdk_bit_array_set(free_vpd_ids, pd_idx);
	LIST_INSERT_HEAD(&ctrl->vdev->vpd_list, vpd, entry);
	aqe->resp.create_pd_resp.pd_handle = pd_idx;
	aqe->resp.create_pd_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_SUCCESS;
	SPDK_NOTICELOG("\nlizh vrdma_aq_create_pd...pd_idx %d done\n", pd_idx);
	return;

free_ibpd:
	ibv_dealloc_pd(vpd->ibpd);
free_vpd:
	free(vpd);
}

static void vrdma_aq_destroy_pd(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe)
{
	struct spdk_vrdma_pd *vpd = NULL;

	SPDK_NOTICELOG("\nlizh vrdma_aq_destroy_pd...pd_handle %d start\n",
	aqe->req.destroy_pd_req.pd_handle);
	if (!g_vpd_cnt || !ctrl->vdev ||
		!ctrl->vdev->vpd_cnt) {
		aqe->resp.destroy_pd_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to destroy PD, err(%d)",
				  aqe->resp.destroy_pd_resp.err_code);
		return;
	}
	vpd = find_spdk_vrdma_pd_by_idx(ctrl, aqe->req.destroy_pd_req.pd_handle);
	if (!vpd) {
		aqe->resp.destroy_pd_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to find PD handle %d, err(%d)",
				  aqe->req.destroy_pd_req.pd_handle,
				  aqe->resp.destroy_pd_resp.err_code);
		return;
	}
	if (vpd->ref_cnt) {
		aqe->resp.destroy_pd_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_REF_CNT_INVALID;
		SPDK_ERRLOG("PD handle %d is used now, err(%d)",
				  aqe->req.destroy_pd_req.pd_handle,
				  aqe->resp.destroy_pd_resp.err_code);
		return;
	}
	if (ctrl->srv_ops->vrdma_device_destroy_pd(&ctrl->dev, aqe)) {
		aqe->resp.destroy_pd_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_SERVICE_FAIL;
		SPDK_ERRLOG("Failed to notify destroy PD handle %d in service, err(%d)",
				  aqe->req.destroy_pd_req.pd_handle,
				  aqe->resp.destroy_pd_resp.err_code);
		return;
	}
	LIST_REMOVE(vpd, entry);
    ibv_dealloc_pd(vpd->ibpd);
	spdk_bit_array_clear(free_vpd_ids, vpd->pd_idx);

	/* Free mlnx-qp pool */

    free(vpd);
	g_vpd_cnt--;
	ctrl->vdev->vpd_cnt--;
	aqe->resp.destroy_pd_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_SUCCESS;
	SPDK_NOTICELOG("\nlizh vrdma_aq_destroy_pd...done\n");
}

static inline unsigned int log2above(unsigned int v)
{
	unsigned int l;
	unsigned int r;

	for (l = 0, r = 0; (v >> 1); ++l, v >>= 1)
		r |= (v & 1);
	return l + r;
}

static void vrdma_destroy_crossing_mkey(struct snap_device *dev,
					 struct spdk_vrdma_mr_log *lattr)
{
	int ret = 0;

	if (lattr->crossing_mkey) {
		ret = snap_destroy_cross_mkey(lattr->crossing_mkey);
		if (ret)
			SPDK_ERRLOG("dev(%s): Failed to destroy cross mkey, err(%d)",
				  dev->pci->pci_number, ret);
		lattr->crossing_mkey = NULL;
	}
}

static void vrdma_indirect_mkey_attr_init(struct snap_device *dev,
					 struct spdk_vrdma_mr_log *log,
					 struct snap_cross_mkey *crossing_mkey,
					 struct mlx5_devx_mkey_attr* attr,
					 uint32_t *total_len)
{
	uint32_t sge_size = log->sge[0].size;
	struct mlx5_klm *klm_array = attr->klm_array;
	uint64_t size = 0;
	uint32_t i;

	attr->log_entity_size = log2above(sge_size);
	if (((uint32_t)(1 << attr->log_entity_size) != sge_size) ||
	    (attr->log_entity_size < LOG_4K_PAGE_SIZE))
		attr->log_entity_size = 0;
	for (i = 0; i < log->num_sge; ++i) {
		size += log->sge[i].size;

		if (log->sge[i].size != sge_size)
			attr->log_entity_size = 0;
	}
	for (i = 0; i < log->num_sge; ++i) {
		if (!attr->log_entity_size)
			klm_array[i].byte_count = log->sge[i].size;

		klm_array[i].mkey = crossing_mkey->mkey;
		klm_array[i].address = log->sge[i].paddr;
	}

	attr->addr = log->start_vaddr;
	attr->size = size; /* total size */
	attr->klm_num = log->num_sge;
	*total_len = size;

	SPDK_NOTICELOG("\ndev(%s): start_addr:0x%lx, total_size:0x%lx, "
		  "crossing key:0x%x, log_entity_size:0x%x klm_num:0x%x\n",
		  dev->pci->pci_number, attr->addr, attr->size,
		  crossing_mkey->mkey, attr->log_entity_size, attr->klm_num);
}

static int vrdma_destroy_indirect_mkey(struct snap_device *dev,
					struct spdk_vrdma_mr_log *lattr)
{
	int ret = 0;

	if (lattr->indirect_mkey) {
		ret = snap_destroy_indirect_mkey(lattr->indirect_mkey);
		if (ret)
			SPDK_ERRLOG("\ndev(%s): Failed to destroy indirect mkey, err(%d)",
				  dev->pci->pci_number, ret);
		lattr->indirect_mkey = NULL;
		free(lattr->klm_array);
		lattr->klm_array = NULL;
	}
	return ret;
}

static struct snap_indirect_mkey*
vrdma_create_indirect_mkey(struct snap_device *dev,
					struct spdk_vrdma_mr *vmr,
				    struct spdk_vrdma_mr_log *log,
				    uint32_t *total_len)
{
	struct snap_cross_mkey *crossing_mkey = log->crossing_mkey;
	struct snap_indirect_mkey *indirect_mkey;
	struct mlx5_devx_mkey_attr attr = {0};

	attr.klm_array = calloc(log->num_sge, sizeof(struct mlx5_klm));
	if (!attr.klm_array)
		return NULL;

	vrdma_indirect_mkey_attr_init(dev, log,
						 crossing_mkey,
						 &attr, total_len);

	indirect_mkey = snap_create_indirect_mkey(vmr->vpd->ibpd, &attr);
	if (indirect_mkey == NULL) {
		SPDK_ERRLOG("\ndev(%s): Failed to create indirect mkey",
			  dev->pci->pci_number);
		goto free_klm_array;
	}
	log->klm_array = attr.klm_array;
	return indirect_mkey;
free_klm_array:
	free(attr.klm_array);
	return NULL;
}

static int vrdma_create_remote_mkey(struct vrdma_ctrl *ctrl,
					struct spdk_vrdma_mr *vmr)
{
	struct spdk_vrdma_mr_log *lattr;
	uint32_t total_len = 0;

	lattr = &vmr->mr_log;
	lattr->crossing_mkey = snap_create_cross_mkey(vmr->vpd->ibpd,
								ctrl->sctrl->sdev);
	if (!lattr->crossing_mkey) {
		SPDK_ERRLOG("\ndev(%s): Failed to create cross mkey", ctrl->name);
		return -1;
	}

	if (lattr->num_sge == 1) {
			lattr->mkey = lattr->crossing_mkey->mkey;
			lattr->log_base = lattr->sge[0].paddr;
			lattr->log_size = lattr->sge[0].size;
	} else {
		lattr->indirect_mkey = vrdma_create_indirect_mkey(ctrl->sctrl->sdev,
									vmr, lattr, &total_len);
		if (!lattr->indirect_mkey) {
			SPDK_ERRLOG("\ndev(%s): Failed to create indirect mkey",
					ctrl->name);
			goto destroy_crossing;
		}
		lattr->mkey = lattr->indirect_mkey->mkey;
		lattr->log_size = total_len;
		lattr->log_base = 0;
	}

	SPDK_NOTICELOG("\ndev(%s): Created remote mkey=0x%x, "
	"start_vaddr=0x%lx, base=0x%lx, size=0x%x\n",
		  ctrl->name, lattr->mkey, lattr->start_vaddr, lattr->log_base, lattr->log_size);
	return 0;

destroy_crossing:
	vrdma_destroy_crossing_mkey(ctrl->sctrl->sdev, lattr);
	return -1;
}

void vrdma_destroy_remote_mkey(struct vrdma_ctrl *ctrl,
					struct spdk_vrdma_mr *vmr)
{
	struct spdk_vrdma_mr_log *lattr = &vmr->mr_log;

	if (!lattr->mkey) {
		SPDK_ERRLOG("\ndev(%s): remote mkey is not created", ctrl->name);
		return;
	}
	vrdma_destroy_indirect_mkey(ctrl->sctrl->sdev, lattr);
	vrdma_destroy_crossing_mkey(ctrl->sctrl->sdev, lattr);
	return;
}

static void vrdma_reg_mr_create_attr(struct vrdma_create_mr_req *mr_req,
				struct spdk_vrdma_mr *vmr)
{
	struct spdk_vrdma_mr_log *lattr = &vmr->mr_log;
	uint32_t i;
 
	lattr->start_vaddr = mr_req->vaddr;
	lattr->num_sge = mr_req->sge_count;
	for (i = 0; i < lattr->num_sge; i++) {
		lattr->sge[i].paddr = mr_req->sge_list[i].pa;
		lattr->sge[i].size = mr_req->sge_list[i].length;
	}
	/*TODO use mr_type and access_flag in future. Not support in POC.*/
}

static void vrdma_aq_reg_mr(struct vrdma_ctrl *ctrl,
	        	struct vrdma_admin_cmd_entry *aqe)
{
	struct spdk_vrdma_mr *vmr;
	struct vrdma_cmd_param param;
	struct spdk_vrdma_pd *vpd = NULL;
	uint32_t dev_max_mr = spdk_min(VRDMA_DEV_MAX_MR,
			(1 << ctrl->sctx->vrdma_caps.log_max_mkey));
	uint32_t i, mr_idx, total_len = 0;

	SPDK_NOTICELOG("\nlizh vrdma_aq_reg_mr...start\n");
	if (g_vmr_cnt > VRDMA_MAX_MR_NUM ||
		!ctrl->vdev ||
		ctrl->vdev->vmr_cnt > dev_max_mr) {
		aqe->resp.create_mr_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_EXCEED_MAX;
		SPDK_ERRLOG("Failed to register MR, err(%d)",
				  aqe->resp.create_mr_resp.err_code);
		return;
	}
	if (!aqe->req.create_mr_req.sge_count || !aqe->req.create_mr_req.length) {
		aqe->resp.create_mr_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to register MR, err(%d)",
				  aqe->resp.create_mr_resp.err_code);
		return;
	}
	vpd = find_spdk_vrdma_pd_by_idx(ctrl, aqe->req.create_mr_req.pd_handle);
	if (!vpd) {
		aqe->resp.create_mr_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to find PD %d when creation MR, err(%d)",
					aqe->req.create_mr_req.pd_handle,
					aqe->resp.create_mr_resp.err_code);
		return;
	}
	if (aqe->req.create_mr_req.sge_count > MAX_VRDMA_MR_SGE_NUM) {
		aqe->resp.create_mr_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_EXCEED_MAX;
		SPDK_ERRLOG("Failed to register MR for sge_count more than 8, err(%d)",
				  aqe->resp.create_mr_resp.err_code);
		return;
	}
	for (i = 0; i < aqe->req.create_mr_req.sge_count; i++) {
		total_len += aqe->req.create_mr_req.sge_list[i].length;
	}
	if (total_len < aqe->req.create_mr_req.length) {
		aqe->resp.create_mr_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to register MR for sge length %d more than %ld, err(%d)",
				total_len, aqe->req.create_mr_req.length,
				aqe->resp.create_mr_resp.err_code);
		return;
	}
	mr_idx = spdk_bit_array_find_first_clear(free_vmr_ids, 0);
	if (mr_idx == UINT32_MAX) {
		aqe->resp.create_mr_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_NO_MEM;
		SPDK_ERRLOG("Failed to allocate mr_idx, err(%d)",
				  aqe->resp.create_mr_resp.err_code);
		return;
	}
    vmr = calloc(1, sizeof(*vmr));
    if (!vmr) {
		aqe->resp.create_mr_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_NO_MEM;
		SPDK_ERRLOG("Failed to allocate MR memory, err(%d)",
				  aqe->resp.create_mr_resp.err_code);
		return;
	}
	vrdma_reg_mr_create_attr(&aqe->req.create_mr_req, vmr);
	vmr->vpd = vpd;
	if (vrdma_create_remote_mkey(ctrl, vmr)) {
		aqe->resp.create_mr_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_UNKNOWN;
		SPDK_ERRLOG("Failed to register MR remote mkey, err(%d)",
				  aqe->resp.create_mr_resp.err_code);
		goto free_vmr;
	}
	param.param.create_mr_param.mr_handle = mr_idx;
	param.param.create_mr_param.lkey = vmr->mr_log.mkey;
	param.param.create_mr_param.rkey = vmr->mr_log.mkey;
	if (ctrl->srv_ops->vrdma_device_create_mr(&ctrl->dev, aqe, &param)) {
		aqe->resp.create_mr_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_SERVICE_FAIL;
		SPDK_ERRLOG("Failed to register MR in service, err(%d)",
				  aqe->resp.create_mr_resp.err_code);
		goto free_mkey;
	}
	g_vmr_cnt++;
	ctrl->vdev->vmr_cnt++;
	vmr->mr_idx = mr_idx;
	vpd->ref_cnt++;
	spdk_bit_array_set(free_vmr_ids, mr_idx);
	LIST_INSERT_HEAD(&ctrl->vdev->vmr_list, vmr, entry);
	aqe->resp.create_mr_resp.rkey = vmr->mr_log.mkey;
	aqe->resp.create_mr_resp.lkey = aqe->resp.create_mr_resp.rkey;
	aqe->resp.create_mr_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_SUCCESS;
	SPDK_NOTICELOG("\nlizh vrdma_aq_reg_mr...mr_idx %d done\n", mr_idx);
	return;

free_mkey:
	vrdma_destroy_remote_mkey(ctrl, vmr);
free_vmr:
	free(vmr);
	return;
}

static void vrdma_aq_dereg_mr(struct vrdma_ctrl *ctrl,
			struct vrdma_admin_cmd_entry *aqe)
{
	struct spdk_vrdma_mr *vmr = NULL;
	struct vrdma_cmd_param param;

	SPDK_NOTICELOG("\nlizh vrdma_aq_dereg_mr..lkey=0x%x.start\n",
	aqe->req.destroy_mr_req.lkey);
	if (!g_vmr_cnt || !ctrl->vdev ||
		!ctrl->vdev->vmr_cnt ||
		!aqe->req.destroy_mr_req.lkey) {
		aqe->resp.destroy_mr_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to dereg MR, err(%d)",
				  aqe->resp.destroy_mr_resp.err_code);
		return;
	}
	vmr = find_spdk_vrdma_mr_by_key(ctrl, aqe->req.destroy_mr_req.lkey);
	if (!vmr) {
		aqe->resp.destroy_mr_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to find MR %d dereg MR, err(%d)",
					aqe->req.destroy_mr_req.lkey,
					aqe->resp.destroy_mr_resp.err_code);
		return;
	}
	if (vmr->ref_cnt) {
		aqe->resp.destroy_mr_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_REF_CNT_INVALID;
		SPDK_ERRLOG("MR %d is used now, err(%d)",
					aqe->req.destroy_mr_req.lkey,
					aqe->resp.destroy_mr_resp.err_code);
		return;
	}
	param.param.destroy_mr_param.mr_handle = vmr->mr_idx;
	if (ctrl->srv_ops->vrdma_device_destroy_mr(&ctrl->dev, aqe, &param)) {
		aqe->resp.destroy_mr_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_SERVICE_FAIL;
		SPDK_ERRLOG("Failed to notify MR %d dereg MR in service, err(%d)",
					aqe->req.destroy_mr_req.lkey,
					aqe->resp.destroy_mr_resp.err_code);
		return;
	}
	LIST_REMOVE(vmr, entry);
	vrdma_destroy_remote_mkey(ctrl, vmr);
	spdk_bit_array_clear(free_vmr_ids, vmr->mr_idx);

	g_vmr_cnt--;
	ctrl->vdev->vmr_cnt--;
	vmr->vpd->ref_cnt--;
	free(vmr);
	aqe->resp.destroy_mr_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_SUCCESS;
	SPDK_NOTICELOG("\nlizh vrdma_aq_dereg_mr...done\n");
}

/* vqp -> mlnx_qp -> mlnx_cq-> vcq -> veq -> vector_idx -> mlnx_cqn for msix */
static void vrdma_aq_create_cq(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe)
{
	struct spdk_vrdma_cq *vcq;
	struct vrdma_cmd_param param;
	struct spdk_vrdma_eq *veq = NULL;
	uint32_t cq_idx;

	SPDK_NOTICELOG("\nlizh vrdma_aq_create_cq...start\n");
	if (g_vcq_cnt > VRDMA_MAX_CQ_NUM ||
		!ctrl->vdev ||
		ctrl->vdev->vcq_cnt > VRDMA_DEV_MAX_CQ) {
		aqe->resp.create_cq_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_EXCEED_MAX;
		SPDK_ERRLOG("Failed to create CQ, err(%d)",
				  aqe->resp.create_cq_resp.err_code);
		return;
	}
	veq = find_spdk_vrdma_eq_by_idx(ctrl, aqe->req.create_cq_req.ceq_handle);
	if (!veq) {
		aqe->resp.create_cq_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to find CEQ %d when creation CQ, err(%d)",
					aqe->req.create_cq_req.ceq_handle,
					aqe->resp.create_cq_resp.err_code);
		return;
	}
	cq_idx = spdk_bit_array_find_first_clear(free_vcq_ids, 0);
	if (cq_idx == UINT32_MAX) {
		aqe->resp.create_cq_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_NO_MEM;
		SPDK_ERRLOG("Failed to allocate cq_idx, err(%d)",
				  aqe->resp.create_cq_resp.err_code);
		return;
	}
    vcq = calloc(1, sizeof(*vcq));
    if (!vcq) {
		aqe->resp.create_cq_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_NO_MEM;
		SPDK_ERRLOG("Failed to allocate CQ memory, err(%d)",
				  aqe->resp.create_cq_resp.err_code);
		return;
	}
	param.param.create_cq_param.cq_handle = cq_idx;
	if (ctrl->srv_ops->vrdma_device_create_cq(&ctrl->dev, aqe, &param)) {
		aqe->resp.create_cq_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_SERVICE_FAIL;
		SPDK_ERRLOG("Failed to create CQ in service, err(%d)",
				  aqe->resp.create_cq_resp.err_code);
		goto free_vcq;
	}
	g_vcq_cnt++;
	ctrl->vdev->vcq_cnt++;
	vcq->veq = veq;
	vcq->cq_idx = cq_idx;
	vcq->log_cqe_entry_num = aqe->req.create_cq_req.log_cqe_entry_num;
	vcq->log_cqe_size = aqe->req.create_cq_req.log_cqe_size;
	vcq->log_pagesize = aqe->req.create_cq_req.log_pagesize;
	vcq->interrupt_mode = aqe->req.create_cq_req.interrupt_mode;
	vcq->host_pa = aqe->req.create_cq_req.l0_pa;
	veq->ref_cnt++;
	spdk_bit_array_set(free_vcq_ids, cq_idx);
	LIST_INSERT_HEAD(&ctrl->vdev->vcq_list, vcq, entry);
	aqe->resp.create_cq_resp.cq_handle = cq_idx;
	aqe->resp.create_cq_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_SUCCESS;
	SPDK_NOTICELOG("\nlizh vrdma_aq_create_cq...cq_idx %d done\n", cq_idx);
	return;

free_vcq:
	free(vcq);
	return;
}

static void vrdma_aq_destroy_cq(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe)
{
	struct spdk_vrdma_cq *vcq = NULL;

	SPDK_NOTICELOG("\nlizh vrdma_aq_destroy_cq..cq_handle=0x%x.start\n",
	aqe->req.destroy_cq_req.cq_handle);
	if (!g_vcq_cnt || !ctrl->vdev ||
		!ctrl->vdev->vcq_cnt) {
		aqe->resp.destroy_cq_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to destroy CQ, err(%d)",
				  aqe->resp.destroy_cq_resp.err_code);
		return;
	}
	LIST_FOREACH(vcq, &ctrl->vdev->vcq_list, entry)
        if (vcq->cq_idx == aqe->req.destroy_cq_req.cq_handle)
            break;
	if (!vcq) {
		aqe->resp.destroy_cq_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to find destroy CQ %d , err(%d)",
					aqe->req.destroy_cq_req.cq_handle,
					aqe->resp.destroy_cq_resp.err_code);
		return;
	}
	if (vcq->ref_cnt) {
		aqe->resp.destroy_cq_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_REF_CNT_INVALID;
		SPDK_ERRLOG("CQ %d is used now, err(%d)",
					aqe->req.destroy_cq_req.cq_handle,
					aqe->resp.destroy_cq_resp.err_code);
		return;
	}
	if (ctrl->srv_ops->vrdma_device_destroy_cq(&ctrl->dev, aqe)) {
		aqe->resp.destroy_cq_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_SERVICE_FAIL;
		SPDK_ERRLOG("Failed to notify destroy CQ %d in service, err(%d)",
					aqe->req.destroy_cq_req.cq_handle,
					aqe->resp.destroy_cq_resp.err_code);
		return;
	}
	LIST_REMOVE(vcq, entry);
	spdk_bit_array_clear(free_vcq_ids, vcq->cq_idx);

	g_vcq_cnt--;
	ctrl->vdev->vcq_cnt--;
	vcq->veq->ref_cnt--;
	free(vcq);
	aqe->resp.destroy_cq_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_SUCCESS;
	SPDK_NOTICELOG("\nlizh vrdma_aq_destroy_cq...done\n");
	return;
}

static int vrdma_create_backend_qp(struct vrdma_ctrl *ctrl,
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
	//qp->bk_qp.qp_attr.sq_size = vqp->sq.comm.wqebb_cnt;
	qp->bk_qp.qp_attr.sq_size = ctrl->sctrl->bar_curr->mtu;
	qp->bk_qp.qp_attr.sq_max_sge = 1;
	qp->bk_qp.qp_attr.sq_max_inline_size = (64 * (vqp->sq.comm.wqebb_size + 1));
	qp->bk_qp.qp_attr.rq_size = vqp->rq.comm.wqebb_cnt;
	qp->bk_qp.qp_attr.rq_max_sge = 1;
	if (snap_vrdma_create_qp_helper(qp->pd, &qp->bk_qp)) {
		SPDK_ERRLOG("Failed to create backend QP ");
		free(qp);
		return -1;
	}
	vqp->bk_qp[0] = qp;
	LIST_INSERT_HEAD(&ctrl->bk_qp_list, qp, entry);
	SPDK_NOTICELOG("\nlizh vrdma_create_backend_qp...done\n");
	return 0;
}

static void vrdma_destroy_backend_qp(struct spdk_vrdma_qp *vqp)
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

static int vrdma_create_vq(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe,
				struct spdk_vrdma_qp *vqp,
				struct spdk_vrdma_cq *rq_vcq,
				struct spdk_vrdma_cq *sq_vcq)
{
	struct snap_vrdma_vq_create_attr q_attr;
	uint32_t wqe_buff_size, q_buff_size;

	SPDK_NOTICELOG("\nlizh vrdma_create_vq...start\n");
	q_attr.bdev = NULL;
	q_attr.pd = vqp->vpd->ibpd;
	q_attr.sq_size = VRDMA_MAX_DMA_SQ_SIZE_PER_VQP;
	q_attr.rq_size = VRDMA_MAX_DMA_RQ_SIZE_PER_VQP;
	q_attr.tx_elem_size = VRDMA_DMA_ELEM_SIZE;
	q_attr.rx_elem_size = VRDMA_DMA_ELEM_SIZE;
	q_attr.vqpn = vqp->qp_idx;
	vqp->snap_queue = ctrl->sctrl->q_ops->create(ctrl->sctrl, &q_attr);
	if (!vqp->snap_queue) {
		SPDK_ERRLOG("Failed to create rq dma queue");
		return -1;
	}
	vqp->q_comp.func = vrdma_sq_sm_dma_cb;
	vqp->q_comp.count = 1;

	vqp->rq.comm.wqebb_size = aqe->req.create_qp_req.rq_wqebb_size;
	vqp->rq.comm.wqebb_cnt = 1 << aqe->req.create_qp_req.log_rq_wqebb_cnt;
	wqe_buff_size = (VRDMA_QP_WQEBB_BASE_SIZE * (vqp->rq.comm.wqebb_size + 1)) * vqp->rq.comm.wqebb_cnt;
    q_buff_size  = wqe_buff_size + ((1 << rq_vcq->log_cqe_size) * (1 << rq_vcq->log_cqe_entry_num));
	vqp->rq.rq_buff = spdk_malloc(q_buff_size, 0x10, NULL, SPDK_ENV_LCORE_ID_ANY,
                             SPDK_MALLOC_DMA);
    if (!vqp->rq.rq_buff) {
		SPDK_ERRLOG("Failed to allocate rq buff");
        goto destroy_dma;
    }
	vqp->rq.cqe_buff = (uint8_t *)vqp->rq.rq_buff + wqe_buff_size;
    vqp->rq.comm.mr = ibv_reg_mr(vqp->vpd->ibpd, vqp->rq.rq_buff, q_buff_size,
                    IBV_ACCESS_REMOTE_READ |
                    IBV_ACCESS_REMOTE_WRITE |
                    IBV_ACCESS_LOCAL_WRITE);
    if (!vqp->rq.comm.mr) {
		SPDK_ERRLOG("Failed to register rq mr");
        goto free_rq_buff;
    }
	vqp->sq.comm.wqebb_size = aqe->req.create_qp_req.sq_wqebb_size;
	vqp->sq.comm.wqebb_cnt = 1 << aqe->req.create_qp_req.log_sq_wqebb_cnt;
	wqe_buff_size = (VRDMA_QP_WQEBB_BASE_SIZE * (vqp->sq.comm.wqebb_size + 1)) * vqp->sq.comm.wqebb_cnt;
    q_buff_size  = wqe_buff_size + ((1 << sq_vcq->log_cqe_size) * (1 << sq_vcq->log_cqe_entry_num));
	vqp->sq.sq_buff = spdk_malloc(q_buff_size, 0x10, NULL, SPDK_ENV_LCORE_ID_ANY,
                             SPDK_MALLOC_DMA);
    if (!vqp->sq.sq_buff) {
		SPDK_ERRLOG("Failed to allocate sq buff");
        goto free_rq_mr;
    }
	vqp->sq.cqe_buff = (uint8_t *)vqp->sq.sq_buff + wqe_buff_size;
    vqp->sq.comm.mr = ibv_reg_mr(vqp->vpd->ibpd, vqp->sq.sq_buff, q_buff_size,
                    IBV_ACCESS_REMOTE_READ |
                    IBV_ACCESS_REMOTE_WRITE |
                    IBV_ACCESS_LOCAL_WRITE);
    if (!vqp->sq.comm.mr) {
		SPDK_ERRLOG("Failed to register sq mr");
        goto free_sq_buff;
    }
	vqp->rq.comm.wqe_buff_pa = aqe->req.create_qp_req.rq_l0_paddr;
	vqp->rq.comm.doorbell_pa = aqe->req.create_qp_req.rq_pi_paddr;
	vqp->rq.comm.log_pagesize = aqe->req.create_qp_req.log_rq_pagesize;
	vqp->rq.comm.hop = aqe->req.create_qp_req.rq_hop;
	vqp->rq.sm_state = VRDMA_RQ_STATE_IDLE;
	vqp->sq.comm.wqe_buff_pa = aqe->req.create_qp_req.sq_l0_paddr;
	vqp->sq.comm.doorbell_pa = aqe->req.create_qp_req.sq_pi_paddr;
	vqp->sq.comm.log_pagesize = aqe->req.create_qp_req.log_sq_pagesize;
	vqp->sq.comm.hop = aqe->req.create_qp_req.sq_hop;
	vqp->sq.sm_state = VRDMA_SQ_STATE_IDLE;
	SPDK_NOTICELOG("\nlizh vrdma_create_vq...done\n");
	return 0;

free_sq_buff:
	spdk_free(vqp->sq.sq_buff);
free_rq_mr:
	ibv_dereg_mr(vqp->rq.comm.mr);
free_rq_buff:
	spdk_free(vqp->rq.rq_buff);
destroy_dma:
	ctrl->sctrl->q_ops->destroy(ctrl->sctrl, vqp->snap_queue);
	return -1;
}

static void vrdma_destroy_vq(struct vrdma_ctrl *ctrl,
				struct spdk_vrdma_qp *vqp)
{
	bool is_flushing = false;

	/* set queue flushing and waiting it suspended*/
	if (ctrl->sctrl->q_ops->is_suspended(vqp->snap_queue)) {
		ctrl->sctrl->q_ops->destroy(ctrl->sctrl, vqp->snap_queue);
	} else {
		ctrl->sctrl->q_ops->suspend(vqp->snap_queue);
		is_flushing = true;
	}
	if (is_flushing) {
		SPDK_NOTICELOG("\nlizh vrdma_destroy_vq...vq\n");
	}
}

static void vrdma_aq_create_qp(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe)
{
	struct spdk_vrdma_qp *vqp;
	struct vrdma_cmd_param param;
	struct spdk_vrdma_pd *vpd;
	struct spdk_vrdma_cq *sq_vcq;
	struct spdk_vrdma_cq *rq_vcq;
	uint32_t qp_idx;

	SPDK_NOTICELOG("\nlizh vrdma_aq_create_qp...start\n");
	if (g_vqp_cnt > VRDMA_MAX_QP_NUM ||
		!ctrl->vdev ||
		ctrl->vdev->vqp_cnt > VRDMA_DEV_MAX_QP) {
		aqe->resp.create_qp_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_EXCEED_MAX;
		SPDK_ERRLOG("Failed to create QP, err(%d)",
				  aqe->resp.create_qp_resp.err_code);
		return;
	}
	vpd = find_spdk_vrdma_pd_by_idx(ctrl, aqe->req.create_qp_req.pd_handle);
	if (!vpd) {
		aqe->resp.create_qp_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to find PD %d when creation QP, err(%d)",
					aqe->req.create_qp_req.pd_handle,
					aqe->resp.create_qp_resp.err_code);
		return;
	}
	sq_vcq = find_spdk_vrdma_cq_by_idx(ctrl, aqe->req.create_qp_req.sq_cqn);
	if (!sq_vcq) {
		aqe->resp.create_qp_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to find SQ CQ %d when creation QP, err(%d)",
					aqe->req.create_qp_req.sq_cqn,
					aqe->resp.create_qp_resp.err_code);
		return;
	}
	rq_vcq = find_spdk_vrdma_cq_by_idx(ctrl, aqe->req.create_qp_req.rq_cqn);
	if (!rq_vcq) {
		aqe->resp.create_qp_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to find RQ CQ %d when creation QP, err(%d)",
					aqe->req.create_qp_req.rq_cqn,
					aqe->resp.create_qp_resp.err_code);
		return;
	}
	qp_idx = spdk_bit_array_find_first_clear(free_vqp_ids, 0);
	if (qp_idx == UINT32_MAX) {
		aqe->resp.create_qp_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_NO_MEM;
		SPDK_ERRLOG("Failed to allocate qp_idx, err(%d)",
				  aqe->resp.create_qp_resp.err_code);
		return;
	}
    vqp = calloc(1, sizeof(*vqp));
    if (!vqp) {
		aqe->resp.create_qp_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_NO_MEM;
		SPDK_ERRLOG("Failed to allocate QP memory, err(%d)",
				  aqe->resp.create_qp_resp.err_code);
		return;
	}
	vqp->vpd = vpd;
	vqp->sq_vcq = sq_vcq;
	vqp->rq_vcq = rq_vcq;
	vqp->qp_idx = qp_idx;
	if (vrdma_create_vq(ctrl, aqe, vqp, rq_vcq, sq_vcq)) {
		aqe->resp.create_qp_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_UNKNOWN;
		goto free_vqp;
	}
	if (vrdma_create_backend_qp(ctrl, vqp)) {
		aqe->resp.create_qp_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_UNKNOWN;
		goto destroy_vq;
	}

	param.param.create_qp_param.qp_handle = qp_idx;
	if (ctrl->srv_ops->vrdma_device_create_qp(&ctrl->dev, aqe, &param)) {
		aqe->resp.create_qp_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_SERVICE_FAIL;
		SPDK_ERRLOG("Failed to create QP in service, err(%d)",
				  aqe->resp.create_qp_resp.err_code);
		goto destroy_bk_qp;
	}
	g_vqp_cnt++;
	ctrl->vdev->vqp_cnt++;
	sq_vcq->ref_cnt++;
	rq_vcq->ref_cnt++;
	vpd->ref_cnt++;
	spdk_bit_array_set(free_vqp_ids, qp_idx);
	LIST_INSERT_HEAD(&ctrl->vdev->vqp_list, vqp, entry);
	aqe->resp.create_qp_resp.qp_handle = qp_idx;
	aqe->resp.create_qp_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_SUCCESS;
	SPDK_NOTICELOG("\nlizh vrdma_aq_create_cq...qp_idx %d done\n", qp_idx);
	return;

destroy_bk_qp:
	vrdma_destroy_backend_qp(vqp);
destroy_vq:
	vrdma_destroy_vq(ctrl, vqp);
free_vqp:
	free(vqp);
	return;
}

static void vrdma_aq_destroy_qp(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe)
{
	struct spdk_vrdma_qp *vqp = NULL;

	SPDK_NOTICELOG("\nlizh vrdma_aq_destroy_qp..qp_handle=0x%x.start\n",
	aqe->req.destroy_qp_req.qp_handle);
	if (!g_vqp_cnt || !ctrl->vdev ||
		!ctrl->vdev->vqp_cnt) {
		aqe->resp.destroy_qp_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to destroy QP, err(%d)",
				  aqe->resp.destroy_qp_resp.err_code);
		return;
	}
	vqp = find_spdk_vrdma_qp_by_idx(ctrl,
				aqe->req.destroy_qp_req.qp_handle);
	if (!vqp) {
		aqe->resp.destroy_qp_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to find destroy QP %d , err(%d)",
					aqe->req.destroy_qp_req.qp_handle,
					aqe->resp.destroy_qp_resp.err_code);
		return;
	}
	if (vqp->ref_cnt) {
		aqe->resp.destroy_qp_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_REF_CNT_INVALID;
		SPDK_ERRLOG("QP %d is used now, err(%d)",
					aqe->req.destroy_qp_req.qp_handle,
					aqe->resp.destroy_qp_resp.err_code);
		return;
	}
	vrdma_destroy_backend_qp(vqp);
	vrdma_destroy_vq(ctrl, vqp);
	if (ctrl->srv_ops->vrdma_device_destroy_qp(&ctrl->dev, aqe)) {
		aqe->resp.destroy_qp_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_SERVICE_FAIL;
		SPDK_ERRLOG("Failed to notify destroy QP %d in service, err(%d)",
					aqe->req.destroy_qp_req.qp_handle,
					aqe->resp.destroy_qp_resp.err_code);
		return;
	}
	LIST_REMOVE(vqp, entry);
	spdk_bit_array_clear(free_vqp_ids, vqp->qp_idx);

	g_vqp_cnt--;
	ctrl->vdev->vqp_cnt--;
	vqp->sq_vcq->ref_cnt--;
	vqp->rq_vcq->ref_cnt--;
	vqp->vpd->ref_cnt--;
	free(vqp);
	aqe->resp.destroy_qp_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_SUCCESS;
	SPDK_NOTICELOG("\nlizh vrdma_aq_destroy_qp...done\n");
	return;
}

static void vrdma_aq_query_qp(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe)
{
	struct spdk_vrdma_qp *vqp = NULL;

	SPDK_NOTICELOG("\nlizh vrdma_aq_query_qp..qp_handle=0x%x.start\n",
	aqe->req.query_qp_req.qp_handle);
	if (!g_vqp_cnt || !ctrl->vdev ||
		!ctrl->vdev->vqp_cnt) {
		aqe->resp.query_qp_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to query QP, err(%d)",
				  aqe->resp.query_qp_resp.err_code);
		return;
	}
	vqp = find_spdk_vrdma_qp_by_idx(ctrl,
				aqe->req.query_qp_req.qp_handle);
	if (!vqp) {
		aqe->resp.query_qp_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to find query QP %d , err(%d)",
					aqe->req.query_qp_req.qp_handle,
					aqe->resp.query_qp_resp.err_code);
		return;
	}
	aqe->resp.query_qp_resp.qp_state = vqp->qp_state;
	aqe->resp.query_qp_resp.rq_psn = vqp->rq_psn;
	aqe->resp.query_qp_resp.sq_psn = vqp->sq_psn;
	aqe->resp.query_qp_resp.dest_qp_num = vqp->dest_qp_num;
	aqe->resp.query_qp_resp.sq_draining = vqp->sq_draining;
	aqe->resp.query_qp_resp.qkey = vqp->qkey;
	aqe->resp.query_qp_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_SUCCESS;
	
	if (ctrl->srv_ops->vrdma_device_query_qp(&ctrl->dev, aqe)) {
		aqe->resp.query_qp_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_SERVICE_FAIL;
		SPDK_ERRLOG("Failed to notify query QP %d in service, err(%d)",
					aqe->req.query_qp_req.qp_handle,
					aqe->resp.query_qp_resp.err_code);
		return;
	}
	SPDK_NOTICELOG("\nlizh vrdma_aq_query_qp...done\n");
	return;
}

static void vrdma_aq_modify_qp(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe)
{
	struct snap_vrdma_bk_qp_rdy_attr rdy_attr = {0};
	struct spdk_vrdma_qp *vqp = NULL;
	struct ibv_qp_attr qp_attr;
	struct snap_qp *sqp;
	int attr_mask;

	SPDK_NOTICELOG("\nlizh vrdma_aq_modify_qp..qp_handle=0x%x.start\n",
				aqe->req.modify_qp_req.qp_handle);
	if (!g_vqp_cnt || !ctrl->vdev ||
		!ctrl->vdev->vqp_cnt) {
		aqe->resp.modify_qp_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to modify QP, err(%d)",
				  aqe->resp.modify_qp_resp.err_code);
		return;
	}
	if (aqe->req.modify_qp_req.qp_attr_mask & ~vrdma_supported_qp_attr_mask) {
		aqe->resp.modify_qp_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to modify QP for qp_attr_mask "
				"some bits unsupportted, err(%d)",
				aqe->resp.modify_qp_resp.err_code);
		return;
	}
	vqp = find_spdk_vrdma_qp_by_idx(ctrl,
				aqe->req.query_qp_req.qp_handle);
	if (!vqp) {
		aqe->resp.modify_qp_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to find QP %d in modify progress , err(%d)",
					aqe->req.modify_qp_req.qp_handle,
					aqe->resp.modify_qp_resp.err_code);
		return;
	}
	if (aqe->req.modify_qp_req.qp_attr_mask & IBV_QP_RQ_PSN){
		vqp->rq_psn = aqe->req.modify_qp_req.rq_psn;
	}
	if (aqe->req.modify_qp_req.qp_attr_mask & IBV_QP_SQ_PSN){
		vqp->sq_psn = aqe->req.modify_qp_req.sq_psn;
	}
	if (aqe->req.modify_qp_req.qp_attr_mask & IBV_QP_DEST_QPN){
		vqp->dest_qp_num = aqe->req.modify_qp_req.dest_qp_num;
	}
	/* lizh : Waiting for tencent add define qp_attr_mask*/
	if (aqe->req.modify_qp_req.qp_attr_mask & IBV_QP_AV){
		vqp->sip = aqe->req.modify_qp_req.sip;
		vqp->dip = aqe->req.modify_qp_req.dip;
	}
	if (aqe->req.modify_qp_req.qp_attr_mask & IBV_QP_QKEY){
		vqp->qkey = aqe->req.modify_qp_req.qkey;
	}
	if (aqe->req.modify_qp_req.qp_attr_mask & IBV_QP_TIMEOUT){
		vqp->timeout = aqe->req.modify_qp_req.timeout;
	}
	if (aqe->req.modify_qp_req.qp_attr_mask & IBV_QP_MIN_RNR_TIMER){
		vqp->min_rnr_timer = aqe->req.modify_qp_req.min_rnr_timer;
	}
	if (aqe->req.modify_qp_req.qp_attr_mask & IBV_QP_RETRY_CNT){
		vqp->timeout_retry_cnt = aqe->req.modify_qp_req.timeout_retry_cnt;
	}
	if (aqe->req.modify_qp_req.qp_attr_mask & IBV_QP_RNR_RETRY){
		vqp->rnr_retry_cnt = aqe->req.modify_qp_req.rnr_retry_cnt;
	}
	if (aqe->req.modify_qp_req.qp_attr_mask & IBV_QP_STATE){
		if (vqp->qp_state == IBV_QPS_RESET &&
			aqe->req.modify_qp_req.qp_state >= IBV_QPS_INIT) {
			/* Modify bankend QP to ready (rst2init + init2rtr + rtr2rts)*/
			sqp = vqp->bk_qp[0]->bk_qp.sqp;
			qp_attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ |
						 IBV_ACCESS_REMOTE_ATOMIC | IBV_ACCESS_LOCAL_WRITE;
			attr_mask = IBV_QP_ACCESS_FLAGS;
			if (snap_vrdma_modify_bankend_qp_rst2init(sqp, &qp_attr, attr_mask)) {
				aqe->resp.modify_qp_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_UNKNOWN;
				SPDK_ERRLOG("Failed to modify bankend QP %d to init, err(%d)",
					aqe->req.modify_qp_req.qp_handle,
					aqe->resp.modify_qp_resp.err_code);
				return;
			}

			attr_mask = IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | 
						IBV_QP_RQ_PSN | IBV_QP_MIN_RNR_TIMER;
			qp_attr.path_mtu = ctrl->sctrl->bar_curr->mtu > 1024 ?
						IBV_MTU_2048 : IBV_MTU_1024;
			qp_attr.dest_qp_num = vqp->bk_qp[0]->remote_qpn;
			if (qp_attr.dest_qp_num == VRDMA_INVALID_QPN) {
				aqe->resp.modify_qp_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_UNKNOWN;
				SPDK_ERRLOG("Failed to modify bankend QP %d for invalid remote qpn, err(%d)",
					aqe->req.modify_qp_req.qp_handle,
					aqe->resp.modify_qp_resp.err_code);
				return;
			}
			qp_attr.rq_psn = 0;
			qp_attr.min_rnr_timer = 12;
			rdy_attr.dest_mac = vqp->bk_qp[0]->dest_mac;
			rdy_attr.rgid_rip = vqp->bk_qp[0]->rgid_rip;
			rdy_attr.src_addr_index = 0;
			if (snap_vrdma_modify_bankend_qp_init2rtr(sqp, &qp_attr, attr_mask, &rdy_attr)) {
				aqe->resp.modify_qp_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_UNKNOWN;
				SPDK_ERRLOG("Failed to modify bankend QP %d to RTR, err(%d)",
					aqe->req.modify_qp_req.qp_handle,
					aqe->resp.modify_qp_resp.err_code);
				return;
			}

			attr_mask = IBV_QP_SQ_PSN | IBV_QP_RETRY_CNT | 
						IBV_QP_RNR_RETRY | IBV_QP_TIMEOUT;
			qp_attr.sq_psn = 0;
			qp_attr.retry_cnt = 8;
			qp_attr.rnr_retry = 8;
			qp_attr.timeout = 32;
			if (snap_vrdma_modify_bankend_qp_rtr2rts(sqp, &qp_attr, attr_mask)) {
				aqe->resp.modify_qp_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_UNKNOWN;
				SPDK_ERRLOG("Failed to modify bankend QP %d to RTS, err(%d)",
					aqe->req.modify_qp_req.qp_handle,
					aqe->resp.modify_qp_resp.err_code);
				return;
			}
		}
		//init2rtr qp join poller: lei API
		if (vqp->qp_state == IBV_QPS_INIT &&
			aqe->req.modify_qp_req.qp_state == IBV_QPS_RTR) {
		}
		vqp->qp_state = aqe->req.modify_qp_req.qp_state;
	}
	if (ctrl->srv_ops->vrdma_device_modify_qp(&ctrl->dev, aqe)) {
		aqe->resp.modify_qp_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_SERVICE_FAIL;
		SPDK_ERRLOG("Failed to notify modify QP %d in service, err(%d)",
					aqe->req.modify_qp_req.qp_handle,
					aqe->resp.modify_qp_resp.err_code);
		return;
	}
	SPDK_NOTICELOG("\nlizh vrdma_aq_modify_qp...done\n");
	return;
}

static void vrdma_aq_create_ceq(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe)
{
	struct spdk_vrdma_eq *veq;
	struct vrdma_cmd_param param;
	uint32_t eq_idx;

	SPDK_NOTICELOG("\nlizh vrdma_aq_create_ceq...start\n");
	if (g_veq_cnt > VRDMA_MAX_EQ_NUM ||
		!ctrl->vdev || !ctrl->sctrl ||
		ctrl->vdev->veq_cnt > VRDMA_DEV_MAX_EQ ||
		aqe->req.create_ceq_req.vector_idx >=
			ctrl->sctrl->bar_curr->num_msix) {
		aqe->resp.create_ceq_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_EXCEED_MAX;
		SPDK_ERRLOG("Failed to create ceq, err(%d)",
				  aqe->resp.create_ceq_resp.err_code);
		return;
	}
	eq_idx = spdk_bit_array_find_first_clear(free_veq_ids, 0);
	if (eq_idx == UINT32_MAX) {
		aqe->resp.create_ceq_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_NO_MEM;
		SPDK_ERRLOG("Failed to allocate eq_idx, err(%d)",
				  aqe->resp.create_ceq_resp.err_code);
		return;
	}
    veq = calloc(1, sizeof(*veq));
    if (!veq) {
		aqe->resp.create_ceq_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_NO_MEM;
		SPDK_ERRLOG("Failed to allocate CEQ memory, err(%d)",
				  aqe->resp.create_ceq_resp.err_code);
		return;
	}

	param.param.create_eq_param.eq_handle = eq_idx;
	if (ctrl->srv_ops->vrdma_device_create_eq(&ctrl->dev, aqe, &param)) {
		aqe->resp.create_ceq_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_SERVICE_FAIL;
		SPDK_ERRLOG("Failed to create CEQ in service, err(%d)",
				  aqe->resp.create_ceq_resp.err_code);
		goto free_veq;
	}

	g_veq_cnt++;
	ctrl->vdev->veq_cnt++;
	veq->eq_idx = eq_idx;
	veq->log_depth = aqe->req.create_ceq_req.log_depth;
	veq->queue_addr = aqe->req.create_ceq_req.queue_addr;
	veq->vector_idx = aqe->req.create_ceq_req.vector_idx;
	spdk_bit_array_set(free_veq_ids, eq_idx);
	LIST_INSERT_HEAD(&ctrl->vdev->veq_list, veq, entry);
	aqe->resp.create_ceq_resp.ceq_handle = eq_idx;
	aqe->resp.create_ceq_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_SUCCESS;
	SPDK_NOTICELOG("\nlizh vrdma_aq_create_ceq...eq_idx %d vector_idx 0x%x done\n", eq_idx, veq->vector_idx);
	return;

free_veq:
	free(veq);
	return;

	return;
}

static void vrdma_aq_modify_ceq(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe)
{
	aqe->resp.modify_ceq_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_UNKNOWN;
	SPDK_ERRLOG("\nIt does not support modify ceq, err(%d) \n",
				  aqe->resp.modify_ceq_resp.err_code);
	return;
}

static void vrdma_aq_destroy_ceq(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe)
{
	struct spdk_vrdma_eq *veq = NULL;

	SPDK_NOTICELOG("\nlizh vrdma_aq_destroy_ceq..ceq_handle=0x%x.start\n",
	aqe->req.destroy_ceq_req.ceq_handle);
	if (!g_veq_cnt || !ctrl->vdev ||
		!ctrl->vdev->veq_cnt) {
		aqe->resp.destroy_ceq_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to destroy CEQ, err(%d)",
				  aqe->resp.destroy_ceq_resp.err_code);
		return;
	}
	veq = find_spdk_vrdma_eq_by_idx(ctrl,
				aqe->req.destroy_ceq_req.ceq_handle);
	if (!veq) {
		aqe->resp.destroy_ceq_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to find destroy CEQ %d , err(%d)",
					aqe->req.destroy_ceq_req.ceq_handle,
					aqe->resp.destroy_ceq_resp.err_code);
		return;
	}
	if (veq->ref_cnt) {
		aqe->resp.destroy_ceq_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_REF_CNT_INVALID;
		SPDK_ERRLOG("CEQ %d is used now, err(%d)",
					aqe->req.destroy_ceq_req.ceq_handle,
					aqe->resp.destroy_ceq_resp.err_code);
		return;
	}
	if (ctrl->srv_ops->vrdma_device_destroy_eq(&ctrl->dev, aqe)) {
		aqe->resp.destroy_ceq_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_SERVICE_FAIL;
		SPDK_ERRLOG("Failed to notify destroy CEQ %d in service, err(%d)",
					aqe->req.destroy_ceq_req.ceq_handle,
					aqe->resp.destroy_ceq_resp.err_code);
		return;
	}
	LIST_REMOVE(veq, entry);
	spdk_bit_array_clear(free_veq_ids, veq->eq_idx);

	g_veq_cnt--;
	ctrl->vdev->veq_cnt--;
	free(veq);
	aqe->resp.destroy_ceq_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_SUCCESS;
	SPDK_NOTICELOG("\nlizh vrdma_aq_destroy_ceq...done\n");
	return;
}

static void vrdma_aq_create_ah(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe)
{
	struct spdk_vrdma_ah *vah;
	struct vrdma_cmd_param param;
	struct spdk_vrdma_pd *vpd = NULL;
	uint32_t ah_idx;

	SPDK_NOTICELOG("\nlizh vrdma_aq_create_ah...start\n");
	if (g_vah_cnt > VRDMA_MAX_AH_NUM ||
		!ctrl->vdev ||
		ctrl->vdev->vah_cnt > VRDMA_DEV_MAX_AH) {
		aqe->resp.create_ah_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_EXCEED_MAX;
		SPDK_ERRLOG("Failed to create ah, err(%d)",
				  aqe->resp.create_ah_resp.err_code);
		return;
	}
	vpd = find_spdk_vrdma_pd_by_idx(ctrl, aqe->req.create_ah_req.pd_handle);
	if (!vpd) {
		aqe->resp.create_ah_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to find PD %d when creation AH, err(%d)",
					aqe->req.create_ah_req.pd_handle,
					aqe->resp.create_ah_resp.err_code);
		return;
	}
	ah_idx = spdk_bit_array_find_first_clear(free_vah_ids, 0);
	if (ah_idx == UINT32_MAX) {
		aqe->resp.create_ah_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_NO_MEM;
		SPDK_ERRLOG("Failed to allocate ah_idx, err(%d)",
				  aqe->resp.create_ah_resp.err_code);
		return;
	}
    vah = calloc(1, sizeof(*vah));
    if (!vah) {
		aqe->resp.create_ah_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_NO_MEM;
		SPDK_ERRLOG("Failed to allocate AH memory, err(%d)",
				  aqe->resp.create_ah_resp.err_code);
		return;
	}
	param.param.create_ah_param.ah_handle = ah_idx;
	if (ctrl->srv_ops->vrdma_device_create_ah(&ctrl->dev, aqe, &param)) {
		aqe->resp.create_ah_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_SERVICE_FAIL;
		SPDK_ERRLOG("Failed to create AH in service, err(%d)",
				  aqe->resp.create_ah_resp.err_code);
		goto free_vah;
	}
	g_vah_cnt++;
	ctrl->vdev->vah_cnt++;
	vah->vpd = vpd;
	vah->ah_idx = ah_idx;
	vah->dip = aqe->req.create_ah_req.dip;
	vpd->ref_cnt++;
	spdk_bit_array_set(free_vah_ids, ah_idx);
	LIST_INSERT_HEAD(&ctrl->vdev->vah_list, vah, entry);
	aqe->resp.create_ah_resp.ah_handle = ah_idx;
	aqe->resp.create_ah_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_SUCCESS;
	SPDK_NOTICELOG("\nlizh vrdma_aq_create_ah...ah_idx %d dip 0x%x done\n", ah_idx, vah->dip);
	return;

free_vah:
	free(vah);
	return;
}

static void vrdma_aq_destroy_ah(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe)
{
	struct spdk_vrdma_ah *vah = NULL;

	SPDK_NOTICELOG("\nlizh vrdma_aq_destroy_ah..ah_handle=0x%x.start\n",
	aqe->req.destroy_ah_req.ah_handle);
	if (!g_vah_cnt || !ctrl->vdev ||
		!ctrl->vdev->vah_cnt) {
		aqe->resp.destroy_ah_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to destroy AH, err(%d)",
				  aqe->resp.destroy_ah_resp.err_code);
		return;
	}
	vah = find_spdk_vrdma_ah_by_idx(ctrl, aqe->req.destroy_ah_req.ah_handle);
	if (!vah) {
		aqe->resp.destroy_ah_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to find destroy AH %d , err(%d)",
					aqe->req.destroy_ah_req.ah_handle,
					aqe->resp.destroy_ah_resp.err_code);
		return;
	}
	if (vah->ref_cnt) {
		aqe->resp.destroy_ah_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_REF_CNT_INVALID;
		SPDK_ERRLOG("AH %d is used now, err(%d)",
					aqe->req.destroy_ah_req.ah_handle,
					aqe->resp.destroy_ah_resp.err_code);
		return;
	}
	if (ctrl->srv_ops->vrdma_device_destroy_ah(&ctrl->dev, aqe)) {
		aqe->resp.destroy_ah_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_SERVICE_FAIL;
		SPDK_ERRLOG("Failed to notify destroy AH %d in service, err(%d)",
					aqe->req.destroy_ah_req.ah_handle,
					aqe->resp.destroy_ah_resp.err_code);
		return;
	}
	LIST_REMOVE(vah, entry);
	spdk_bit_array_clear(free_vah_ids, vah->ah_idx);

	g_vah_cnt--;
	ctrl->vdev->vah_cnt--;
	vah->vpd->ref_cnt--;
	free(vah);
	aqe->resp.destroy_ah_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_SUCCESS;
	SPDK_NOTICELOG("\nlizh vrdma_aq_destroy_ah...done\n");
}

int vrdma_parse_admq_entry(struct vrdma_ctrl *ctrl,
			struct vrdma_admin_cmd_entry *aqe)
{
	if (!ctrl || aqe_sanity_check(aqe)) {
		SPDK_ERRLOG("\nlizh vrdma_parse_admq_entry check input fail\n");
		return -1;
	}

	SPDK_NOTICELOG("\nlizh vrdma_parse_admq_entry aqe->hdr.opcode %d\n",
			aqe->hdr.opcode);
	switch (aqe->hdr.opcode) {
			case VRDMA_ADMIN_OPEN_DEVICE:
				vrdma_aq_open_dev(ctrl, aqe);
				break;
			case VRDMA_ADMIN_QUERY_DEVICE:
				vrdma_aq_query_dev(ctrl, aqe);
				break;
			case VRDMA_ADMIN_QUERY_PORT:
				vrdma_aq_query_port(ctrl, aqe);
				break;
			case VRDMA_ADMIN_QUERY_GID:
				vrdma_aq_query_gid(ctrl, aqe);
				break;
			case VRDMA_ADMIN_MODIFY_GID:
				vrdma_aq_modify_gid(ctrl, aqe);
				break;
			case VRDMA_ADMIN_CREATE_PD:
				vrdma_aq_create_pd(ctrl, aqe);
				break;
			case VRDMA_ADMIN_DESTROY_PD:
				vrdma_aq_destroy_pd(ctrl, aqe);
				break;
			case VRDMA_ADMIN_REG_MR:
				vrdma_aq_reg_mr(ctrl, aqe);
				break;
			case VRDMA_ADMIN_DEREG_MR:
				vrdma_aq_dereg_mr(ctrl, aqe);
				break;
			case VRDMA_ADMIN_CREATE_CQ:
				vrdma_aq_create_cq(ctrl, aqe);
				break;
			case VRDMA_ADMIN_DESTROY_CQ:
				vrdma_aq_destroy_cq(ctrl, aqe);
				break;
			case VRDMA_ADMIN_CREATE_QP:
				vrdma_aq_create_qp(ctrl, aqe);
				break;
			case VRDMA_ADMIN_DESTROY_QP:
				vrdma_aq_destroy_qp(ctrl, aqe);
				break;
			case VRDMA_ADMIN_QUERY_QP:
				vrdma_aq_query_qp(ctrl, aqe);
				break;
			case VRDMA_ADMIN_MODIFY_QP:
				vrdma_aq_modify_qp(ctrl, aqe);
				break;
			case VRDMA_ADMIN_CREATE_CEQ:
				vrdma_aq_create_ceq(ctrl, aqe);
				break;
			case VRDMA_ADMIN_MODIFY_CEQ:
				vrdma_aq_modify_ceq(ctrl, aqe);
				break;
			case VRDMA_ADMIN_DESTROY_CEQ:
				vrdma_aq_destroy_ceq(ctrl, aqe);
				break;
			case VRDMA_ADMIN_CREATE_AH:
				vrdma_aq_create_ah(ctrl, aqe);
				break;
			case VRDMA_ADMIN_DESTROY_AH:
				vrdma_aq_destroy_ah(ctrl, aqe);
				break;
			default:
				return -1;		
	}

#if 0
	/* lizh:Just for test*/
	if (aqe->hdr.opcode > VRDMA_ADMIN_DEREG_MR && 
		aqe->hdr.opcode != VRDMA_ADMIN_CREATE_AH &&
		aqe->hdr.opcode != VRDMA_ADMIN_DESTROY_AH)
		return 0;
	int i;
	SPDK_NOTICELOG("\nhdr:seq=0x%x magic=0x%x version=%d is_inline_in=%d is_inline_out=%d opcode=%d\n",
		aqe->hdr.seq, aqe->hdr.magic, aqe->hdr.version,
		aqe->hdr.is_inline_in, aqe->hdr.is_inline_out, aqe->hdr.opcode);
	for (i = 0; i < 256;) {
		SPDK_NOTICELOG("\nreq idx=%d:0x%x%x%x%x 0x%x%x%x%x ",
		i, aqe->req.buf[i+3], aqe->req.buf[i+2], aqe->req.buf[i+1], aqe->req.buf[i],
		aqe->req.buf[i+7], aqe->req.buf[i+6], aqe->req.buf[i+5], aqe->req.buf[i+4]);
		i += 8;
	}
	for (i = 0; i < 256;) {
		SPDK_NOTICELOG("\n\nresp idx=%d:0x%x%x%x%x 0x%x%x%x%x ",
		i, aqe->resp.buf[i+3], aqe->resp.buf[i+2], aqe->resp.buf[i+1], aqe->resp.buf[i],
		aqe->resp.buf[i+7], aqe->resp.buf[i+6], aqe->resp.buf[i+5], aqe->resp.buf[i+4]);
		i += 8;
	}
	/* End:Just for test*/
#endif
	return 0;
}

//need invoker to guarantee pi is bigger than ci 
static inline int vrdma_aq_rollback(struct vrdma_admin_sw_qp *aq, uint16_t pi,
                                           uint16_t q_size)
{
	return (pi % q_size < aq->admq->ci % q_size);
}

static bool vrdma_aq_sm_idle(struct vrdma_admin_sw_qp *aq,
                                    enum vrdma_aq_cmd_sm_op_status status)
{
	SPDK_ERRLOG("vrdma admq in invalid state %d\n",
					   VRDMA_CMD_STATE_IDLE);
	return false;
}

static bool vrdma_aq_sm_read_pi(struct vrdma_admin_sw_qp *aq,
                                   enum vrdma_aq_cmd_sm_op_status status)
{
	int ret;
	struct vrdma_ctrl *ctrl = container_of(aq, struct vrdma_ctrl, sw_qp);
	uint64_t pi_addr = ctrl->sctrl->bar_curr->adminq_base_addr + offsetof(struct vrdma_admin_queue, pi);

	if (status != VRDMA_CMD_SM_OP_OK) {
		SPDK_ERRLOG("failed to update admq CI, status %d\n", status);
		aq->state = VRDMA_CMD_STATE_FATAL_ERR;
		return true;
	}

	//SPDK_NOTICELOG("vrdam poll admin pi: admq pa 0x%lx\n", ctrl->sctrl->bar_curr->adminq_base_addr);

	aq->state = VRDMA_CMD_STATE_HANDLE_PI;
	aq->poll_comp.count = 1;

	ret = snap_dma_q_read(ctrl->sctrl->adminq_dma_q, &aq->admq->pi, sizeof(uint16_t),
				          ctrl->sctrl->adminq_mr->lkey, pi_addr,
				          ctrl->sctrl->xmkey->mkey, &aq->poll_comp);
	if (spdk_unlikely(ret)) {
		SPDK_ERRLOG("failed to read admin PI, ret %d\n", ret);
		aq->state = VRDMA_CMD_STATE_FATAL_ERR;
	}

	return false;
}

static bool vrdma_aq_sm_handle_pi(struct vrdma_admin_sw_qp *aq,
                                    enum vrdma_aq_cmd_sm_op_status status)
{

	if (status != VRDMA_CMD_SM_OP_OK) {
		SPDK_ERRLOG("failed to get admq PI, status %d\n", status);
		aq->state = VRDMA_CMD_STATE_FATAL_ERR;
		return false;
	}

	if (aq->admq->pi > aq->admq->ci) {
		aq->state = VRDMA_CMD_STATE_READ_CMD_ENTRY;
	} else {
		aq->state = VRDMA_CMD_STATE_POLL_PI;
	}

	return true;
}

static bool vrdma_aq_sm_read_cmd(struct vrdma_admin_sw_qp *aq,
                                    enum vrdma_aq_cmd_sm_op_status status)
{
	struct vrdma_ctrl *ctrl = container_of(aq, struct vrdma_ctrl, sw_qp);
	uint16_t pi = aq->admq->pi;
	uint32_t aq_poll_size = 0;
	uint64_t host_ring_addr;
	uint8_t *local_ring_addr;
	uint32_t offset = 0;
	uint16_t num = 0;
	uint16_t q_size = ctrl->sctrl->bar_curr->adminq_size;
	int ret;

	host_ring_addr = ctrl->sctrl->bar_curr->adminq_base_addr +
		             offsetof(struct vrdma_admin_queue, ring);

	aq->state = VRDMA_CMD_STATE_PARSE_CMD_ENTRY;
	aq->num_to_parse = pi - aq->admq->ci;

	SPDK_NOTICELOG("vrdam poll admin cmd: admq pa 0x%lx, pi %d, ci %d\n",
			ctrl->sctrl->bar_curr->adminq_base_addr, pi, aq->admq->ci);

	//fetch the delta PI number entry in one time
	if (!vrdma_aq_rollback(aq, pi, q_size)) {
		aq->poll_comp.count = 1;
		num = pi - aq->admq->ci;
		aq_poll_size = num * sizeof(struct vrdma_admin_cmd_entry);
		offset = (aq->admq->ci % q_size) * sizeof(struct vrdma_admin_cmd_entry);
	    	host_ring_addr = host_ring_addr + offset;
		ret = snap_dma_q_read(ctrl->sctrl->adminq_dma_q, aq->admq->ring, aq_poll_size,
				              ctrl->sctrl->adminq_mr->lkey, host_ring_addr,
				              ctrl->sctrl->xmkey->mkey, &aq->poll_comp);
		if (spdk_unlikely(ret)) {
			SPDK_ERRLOG("no roll back failed to read admin CMD entry, ret %d\n", ret);
		    aq->state = VRDMA_CMD_STATE_FATAL_ERR;
		    return true;
		}
	} else {
		/* aq roll back case, first part */
		aq->poll_comp.count = 1;
		num = q_size - (aq->admq->ci % q_size);
		aq_poll_size = num * sizeof(struct vrdma_admin_cmd_entry);
		offset = (aq->admq->ci % q_size) * sizeof(struct vrdma_admin_cmd_entry);
		host_ring_addr = host_ring_addr + offset;
		ret = snap_dma_q_read(ctrl->sctrl->adminq_dma_q, aq->admq->ring, aq_poll_size,
				              ctrl->sctrl->adminq_mr->lkey, host_ring_addr,
				              ctrl->sctrl->xmkey->mkey, &aq->poll_comp);
		if (spdk_unlikely(ret)) {
			SPDK_ERRLOG("roll back failed to first read admin CMD entry, ret %d\n", ret);
		    aq->state = VRDMA_CMD_STATE_FATAL_ERR;
			return true;
		}

		/* calculate second poll size */
		local_ring_addr = (uint8_t *)aq->admq->ring + num * sizeof(struct vrdma_admin_cmd_entry);
		aq->poll_comp.count++;
		num = pi % q_size;
		aq_poll_size = num * sizeof(struct vrdma_admin_cmd_entry);
		host_ring_addr = ctrl->sctrl->bar_curr->adminq_base_addr +
						offsetof(struct vrdma_admin_queue, ring);
		ret = snap_dma_q_read(ctrl->sctrl->adminq_dma_q, local_ring_addr, aq_poll_size,
				              ctrl->sctrl->adminq_mr->lkey, host_ring_addr,
				              ctrl->sctrl->xmkey->mkey, &aq->poll_comp);
		if (spdk_unlikely(ret)) {
			SPDK_ERRLOG("roll back failed to second read admin CMD entry, ret %d\n", ret);
		    aq->state = VRDMA_CMD_STATE_FATAL_ERR;
			return true;
		}
	}

	return false;
}

static bool vrdma_aq_sm_parse_cmd(struct vrdma_admin_sw_qp *aq,
                                           enum vrdma_aq_cmd_sm_op_status status)
{
	uint16_t i;
	int ret = 0;
	struct vrdma_ctrl *ctrl = container_of(aq, struct vrdma_ctrl, sw_qp);

	SPDK_ERRLOG("lizh vrdma_aq_sm_parse_cmd aq->num_to_parse %d\n", aq->num_to_parse);
	if (status != VRDMA_CMD_SM_OP_OK) {
		SPDK_ERRLOG("failed to get admq cmd entry, status %d\n", status);
		aq->state = VRDMA_CMD_STATE_FATAL_ERR;
		return true;
	}

	vrdma_ctrl_dev_init(ctrl);
    aq->state = VRDMA_CMD_STATE_WRITE_CMD_BACK;
	aq->num_parsed = aq->num_to_parse;
	for (i = 0; i < aq->num_to_parse; i++) {
		ret = vrdma_parse_admq_entry(ctrl, &(aq->admq->ring[i]));
		if (ret) {
			if (ret == VRDMA_CMD_STATE_WAITING) {
				aq->state = VRDMA_CMD_STATE_WAITING;
			}
			aq->num_parsed = i;
			break;
		}
	}
	return true;
}

static bool vrdma_aq_sm_waiting(struct vrdma_admin_sw_qp *aq,
                                           enum vrdma_aq_cmd_sm_op_status status)
{
	uint16_t wait_idx;
	struct vrdma_admin_cmd_entry *aqe;

	SPDK_ERRLOG("lizh vrdma_aq_sm_waiting num_to_parse %d, num_parsed %d\n", 
				aq->num_to_parse, aq->num_parsed);
	if (status != VRDMA_CMD_SM_OP_OK) {
		SPDK_ERRLOG("failed to get admq cmd entry, status %d\n", status);
		aq->state = VRDMA_CMD_STATE_FATAL_ERR;
		return true;
	}

	wait_idx = aq->num_parsed;
	aqe = &aq->admq->ring[wait_idx];
	if (aqe->hdr.opcode == VRDMA_ADMIN_DESTROY_QP) {
		uint8_t qp_state = vrdma_get_qp_status(aqe->req.destroy_qp_req.qp_handle);
		if (qp_state == SW_VIRTQ_SUSPENDED) {
			goto next_sm;
		} else {
			aq->state = VRDMA_CMD_STATE_WAITING;
			return false;
		}
	}

next_sm:
    aq->state = VRDMA_CMD_STATE_WRITE_CMD_BACK;
	return true;
}


static bool vrdma_aq_sm_write_cmd(struct vrdma_admin_sw_qp *aq,
                                           enum vrdma_aq_cmd_sm_op_status status)
{
	struct vrdma_ctrl *ctrl = container_of(aq, struct vrdma_ctrl, sw_qp);
	uint16_t num_to_write = aq->num_parsed;
	uint16_t ci = aq->admq->ci;
	uint32_t aq_poll_size = 0;
	uint64_t host_ring_addr;
	uint8_t *local_ring_addr;
	uint32_t offset = 0;
	uint16_t num = 0;
	uint16_t q_size = ctrl->sctrl->bar_curr->adminq_size;
	int ret;

	host_ring_addr = ctrl->sctrl->bar_curr->adminq_base_addr +
		             offsetof(struct vrdma_admin_queue, ring);
	SPDK_NOTICELOG("vrdam write admin cmd: admq pa 0x%lx, num_to_write %d, old ci %d, pi %d\n", 
			ctrl->sctrl->bar_curr->adminq_base_addr, num_to_write, ci, aq->admq->pi);

	if (!num_to_write) {
		aq->state = VRDMA_CMD_STATE_FATAL_ERR;
		return true;
	}
	aq->state = VRDMA_CMD_STATE_UPDATE_CI;

	//write back entries in one time
	if ((num_to_write + ci % q_size) < q_size ) {
		aq->poll_comp.count = 1;
		aq_poll_size = num_to_write * sizeof(struct vrdma_admin_cmd_entry);
		offset = (ci % q_size) * sizeof(struct vrdma_admin_cmd_entry);
	    host_ring_addr = host_ring_addr + offset;
		ret = snap_dma_q_write(ctrl->sctrl->adminq_dma_q, aq->admq->ring, aq_poll_size,
				              ctrl->sctrl->adminq_mr->lkey, host_ring_addr,
				              ctrl->sctrl->xmkey->mkey, &aq->poll_comp);
		if (spdk_unlikely(ret)) {
			SPDK_ERRLOG("no roll back failed to write back admin CMD entry, ret %d\n", ret);
		    aq->state = VRDMA_CMD_STATE_FATAL_ERR;
		    return true;
		}
	} else {
		/* aq roll back case, first part */
		aq->poll_comp.count = 1;
		num = q_size - (ci % q_size);
		aq_poll_size = num * sizeof(struct vrdma_admin_cmd_entry);
		offset = (ci % q_size) * sizeof(struct vrdma_admin_cmd_entry);
		host_ring_addr = host_ring_addr + offset;
		ret = snap_dma_q_write(ctrl->sctrl->adminq_dma_q, aq->admq->ring, aq_poll_size,
				              ctrl->sctrl->adminq_mr->lkey, host_ring_addr,
				              ctrl->sctrl->xmkey->mkey, &aq->poll_comp);
		if (spdk_unlikely(ret)) {
			SPDK_ERRLOG("roll back failed to first write admin CMD entry, ret %d\n", ret);
		    aq->state = VRDMA_CMD_STATE_FATAL_ERR;
			return true;
		}

		/* calculate second poll size */
		local_ring_addr = (uint8_t *)aq->admq->ring + num * sizeof(struct vrdma_admin_cmd_entry);
		aq->poll_comp.count++;
		num = num_to_write - num;
		aq_poll_size = num * sizeof(struct vrdma_admin_cmd_entry);
		host_ring_addr = ctrl->sctrl->bar_curr->adminq_base_addr +
						offsetof(struct vrdma_admin_queue, ring);
		ret = snap_dma_q_write(ctrl->sctrl->adminq_dma_q, local_ring_addr, aq_poll_size,
				              ctrl->sctrl->adminq_mr->lkey, host_ring_addr,
				              ctrl->sctrl->xmkey->mkey, &aq->poll_comp);
		if (spdk_unlikely(ret)) {
			SPDK_ERRLOG("roll back failed to second write admin CMD entry, ret %d\n", ret);
		    aq->state = VRDMA_CMD_STATE_FATAL_ERR;
			return true;
		}
	}

	aq->admq->ci += num_to_write;
	return false;
}

static bool vrdma_aq_sm_update_ci(struct vrdma_admin_sw_qp *aq,
                                           enum vrdma_aq_cmd_sm_op_status status)
{
	int ret;
	struct vrdma_ctrl *ctrl = container_of(aq, struct vrdma_ctrl, sw_qp);
	uint64_t ci_addr  = ctrl->sctrl->bar_curr->adminq_base_addr +
					offsetof(struct vrdma_admin_queue, ci);

	if (status != VRDMA_CMD_SM_OP_OK) {
		SPDK_ERRLOG("failed to write back admq, status %d\n", status);
		aq->state = VRDMA_CMD_STATE_FATAL_ERR;
		return true;
	}

	SPDK_NOTICELOG("vrdam update admq CI: admq pa 0x%lx, new ci %d\n",
					ctrl->sctrl->bar_curr->adminq_base_addr, aq->admq->ci);

	aq->state = VRDMA_CMD_STATE_POLL_PI;
	aq->poll_comp.count = 1;
	ret = snap_dma_q_write(ctrl->sctrl->adminq_dma_q, &aq->admq->ci, sizeof(uint16_t),
				          ctrl->sctrl->adminq_mr->lkey, ci_addr,
				          ctrl->sctrl->xmkey->mkey, &aq->poll_comp);
	if (spdk_unlikely(ret)) {
		SPDK_ERRLOG("failed to update admq CI, ret %d\n", ret);
		aq->state = VRDMA_CMD_STATE_FATAL_ERR;
	}

	return false;
}

static bool vrdma_aq_sm_fatal_error(struct vrdma_admin_sw_qp *aq,
                                       enum vrdma_aq_cmd_sm_op_status status)
{
	/*
	 * TODO: maybe need to add more handling
	 */

	return false;
}

//sm array states must be according to the order of vrdma_aq_cmd_sm_state
static struct vrdma_aq_sm_state vrdma_aq_sm_arr[] = {
/*VRDMA_CMD_STATE_IDLE					*/	{vrdma_aq_sm_idle},
/*VRDMA_CMD_STATE_INIT_CI		        */	{vrdma_aq_sm_idle},
/*VRDMA_CMD_STATE_POLL_PI			    */	{vrdma_aq_sm_read_pi},
/*VRDMA_CMD_STATE_HANDLE_PI			    */	{vrdma_aq_sm_handle_pi},
/*VRDMA_CMD_STATE_READ_CMD_ENTRY	    */	{vrdma_aq_sm_read_cmd},
/*VRDMA_CMD_STATE_PARSE_CMD_ENTRY	    */	{vrdma_aq_sm_parse_cmd},
/*VRDMA_CMD_STATE_WAITING       	    */	{vrdma_aq_sm_waiting},
/*VRDMA_CMD_STATE_WRITE_CMD_BACK	    */	{vrdma_aq_sm_write_cmd},
/*VRDMA_CMD_STATE_UPDATE_CI	            */	{vrdma_aq_sm_update_ci},
/*VIRTQ_CMD_STATE_FATAL_ERR				*/	{vrdma_aq_sm_fatal_error},
											};

struct vrdma_state_machine vrdma_sm  = { vrdma_aq_sm_arr, sizeof(vrdma_aq_sm_arr) / sizeof(struct vrdma_aq_sm_state) };

/**
 * vrdma_aq_cmd_progress() - admq commanda_adminq_init state machine progress handle
 * @aq:	admq to be processed
 * @status:	status of calling function (can be a callback)
 *
 * Return: 0 (Currently no option to fail)
 */
static int vrdma_aq_cmd_progress(struct vrdma_admin_sw_qp *aq,
		          enum vrdma_aq_cmd_sm_op_status status)
{
	struct vrdma_state_machine *sm;
	bool repeat = true;

	while (repeat) {
		repeat = false;
		//SPDK_NOTICELOG("vrdma admq cmd sm state: %d\n", aq->state);
		sm = aq->custom_sm;
		if (spdk_likely(aq->state < VRDMA_CMD_NUM_OF_STATES))
			repeat = sm->sm_array[aq->state].sm_handler(aq, status);
		else
			SPDK_ERRLOG("reached invalid state %d\n", aq->state);
	}

	return 0;
}

void vrdma_aq_sm_dma_cb(struct snap_dma_completion *self, int status)
{
	enum vrdma_aq_cmd_sm_op_status op_status = VRDMA_CMD_SM_OP_OK;
	struct vrdma_admin_sw_qp *aq = container_of(self, struct vrdma_admin_sw_qp,
						                        poll_comp);

	if (status != IBV_WC_SUCCESS) {
		SPDK_ERRLOG("error in dma for vrdma admq state %d\n", aq->state);
		op_status = VRDMA_CMD_SM_OP_ERR;
	}
	vrdma_aq_cmd_progress(aq, op_status);
}

int vrdma_ctrl_adminq_progress(void *ctrl)
{
	struct vrdma_ctrl *vdev_ctrl = ctrl;
	struct vrdma_admin_sw_qp *aq = &vdev_ctrl->sw_qp;
	enum vrdma_aq_cmd_sm_op_status status = VRDMA_CMD_SM_OP_OK;
	int n = 0;

	if (!vdev_ctrl->sctrl->adminq_dma_q) {
		return 0;
	}

	if (aq->state == VRDMA_CMD_STATE_WAITING) {
		SPDK_NOTICELOG("vrdma adminq is in waiting state, cmd idx %d\n",
						aq->num_parsed);
		vrdma_aq_cmd_progress(aq, status);
		return 0;
	}

	n = snap_dma_q_progress(vdev_ctrl->sctrl->adminq_dma_q);

	if (aq->pre_ci == VRDMA_INVALID_CI_PI ||
		aq->state < VRDMA_CMD_STATE_INIT_CI) {
		return 0;
	}

	if (aq->state == VRDMA_CMD_STATE_INIT_CI) {
		vrdma_aq_sm_read_pi(aq, status);
	}

	return n;
}
