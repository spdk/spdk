/*-
 *	 BSD LICENSE
 *
 *	 Copyright © 2022 NVIDIA CORPORATION & AFFILIATES. ALL RIGHTS RESERVED.
 *
 *	 Redistribution and use in source and binary forms, with or without
 *	 modification, are permitted provided that the following conditions
 *	 are met:
 *
 *	   * Redistributions of source code must retain the above copyright
 *		 notice, this list of conditions and the following disclaimer.
 *	   * Redistributions in binary form must reproduce the above copyright
 *		 notice, this list of conditions and the following disclaimer in
 *		 the documentation and/or other materials provided with the
 *		 distribution.
 *	   * Neither the name of Intel Corporation nor the names of its
 *		 contributors may be used to endorse or promote products derived
 *		 from this software without specific prior written permission.
 *
 *	 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *	 "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *	 LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *	 A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *	 OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *	 SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *	 LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *	 DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *	 THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *	 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *	 OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <infiniband/verbs.h>
#include "spdk/config.h"
#include "spdk/log.h"
#include "spdk/vrdma.h"
#include "spdk/vrdma_controller.h"
#include "spdk/vrdma_snap.h"
#include "spdk/vrdma_controller.h"
#include "spdk/vrdma_emu_mgr.h"
#include "spdk/vrdma_snap_pci_mgr.h"
#include "spdk/vrdma_qp.h"
#include "spdk/vrdma_rpc.h"
#include "spdk/vrdma_admq.h"

static uint32_t g_vdev_cnt;

void spdk_vrdma_ctx_stop(void (*fini_cb)(void))
{
	spdk_vrdma_snap_stop(fini_cb);
	vrdma_del_bk_qp_list();
	vrdma_dev_mac_list_del();
	vrdma_del_indirect_mkey_list();
}

int spdk_vrdma_ctx_start(struct spdk_vrdma_ctx *vrdma_ctx)
{
	struct spdk_vrdma_dev *vdev;
	struct ibv_device **list;
	struct snap_context *sctx;
	int dev_count;
	struct spdk_emu_ctx *ctx;
	struct vrdma_ctrl *ctrl;

	g_vdev_cnt = 0;
	memset(&g_vrdma_rpc, 0, sizeof(struct spdk_vrdma_rpc));
	g_vrdma_rpc.srv.rpc_lock_fd = -1;
	if (vrdma_ctx->dpa_enabled) {
		/*Load provider just for DPA*/
	}

	if (spdk_vrdma_snap_start()) {
		SPDK_ERRLOG("Failed to start vrdma snap\n");
		goto err;
	}

	list = ibv_get_device_list(&dev_count);
	if (!list || !dev_count) {
		SPDK_ERRLOG("failed to open IB device list\n");
		goto err;
	}
	strncpy(vrdma_ctx->emu_manager, ibv_get_device_name(list[0]),
			 MAX_VRDMA_DEV_LEN - 1);
    sctx = spdk_vrdma_snap_get_snap_context(ibv_get_device_name(list[0]));
    if (!sctx) {
		SPDK_ERRLOG("failed to find snap context for %s\n",
			vrdma_ctx->emu_manager);
		ibv_free_device_list(list);
		goto err;
	}
	ibv_free_device_list(list);
	/*Create static PF device*/
	for (dev_count = 0; (uint32_t)dev_count < sctx->vrdma_pfs.num_emulated_pfs;
		dev_count++) {
		vdev = calloc(1, sizeof(*vdev));
		if (!vdev)
			goto err;
		vdev->emu_mgr = spdk_vrdma_snap_get_ibv_device(vrdma_ctx->emu_manager);
		vdev->devid = dev_count;
		LIST_INIT(&vdev->vpd_list);
		LIST_INIT(&vdev->vmr_list);
		LIST_INIT(&vdev->vqp_list);
		LIST_INIT(&vdev->vcq_list);
		LIST_INIT(&vdev->veq_list);
		g_vdev_cnt++;
		if (spdk_vrdma_init_all_id_pool(vdev))
			goto free_vdev;
		if (spdk_emu_controller_vrdma_create(vdev))
			goto free_vdev;
		/* init sf name */
		strcpy(vdev->vrdma_sf.sf_name, "dummy");
		ctx = spdk_emu_ctx_find_by_pci_id(vrdma_ctx->emu_manager, vdev->devid);
		ctrl = ctx->ctrl;
		ctrl->emu_ctx  = spdk_vrdma_snap_get_ibv_context(vrdma_ctx->emu_manager);
		//vrdma_ctx->dpa_enabled = 1;
		ctrl->dpa_enabled = 1;
	}
	if (vrdma_ctx->dpa_enabled) {
		/*Prove init DPA*/
	}
	return 0;
free_vdev:
	free(vdev);
err:
	return -1;
}
