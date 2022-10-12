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

#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <infiniband/verbs.h>

#include "snap.h"
#include "snap_env.h"
#include "snap_dma.h"
#include "snap_vrdma_ctrl.h"

#include "spdk/stdinc.h"
#include "spdk/conf.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/vrdma.h"
#include "spdk/vrdma_controller.h"
#include "spdk/vrdma_snap_pci_mgr.h"
#include "spdk/vrdma_admq.h"


int vrdma_dev_name_to_id(const char *rdma_dev_name)
{
    char vrdma_dev[MAX_VRDMA_DEV_LEN];
    char *str, *next;
    int ret = 0;

    snprintf(vrdma_dev, MAX_VRDMA_DEV_LEN, "%s", rdma_dev_name);
    next = vrdma_dev;

    do {
        str = next;
        while (str[0] != '\0' && !isdigit(str[0]))
            str++;

        if (str[0] == '\0')
            break;
        else
            ret = strtol(str, &next, 0);
    } while (true);

    return ret;
}


static struct snap_context *
vrdma_ctrl_find_snap_context(const char *emu_manager, int pf_id)
{
    struct snap_context *ctx, *found = NULL;
    struct ibv_device **ibv_list;
    struct snap_pci **pf_list;
    int ibv_list_sz, pf_list_sz;
    int i, j;

    ibv_list = ibv_get_device_list(&ibv_list_sz);
    if (!ibv_list)
        return NULL;

    for (i = 0; i < ibv_list_sz; i++) {
        if (strncmp(ibv_get_device_name(ibv_list[i]), emu_manager, 16))
            continue;

        ctx = spdk_vrdma_snap_get_snap_context(ibv_get_device_name(ibv_list[i]));
        if (!ctx)
            continue;

        if (!(ctx->emulation_caps & SNAP_VRDMA))
            continue;

        pf_list = calloc(ctx->vrdma_pfs.max_pfs, sizeof(*pf_list));
        if (!pf_list)
            continue;

        pf_list_sz = snap_get_pf_list(ctx, SNAP_VRDMA, pf_list);
        for (j = 0; j < pf_list_sz; j++) {
            if (pf_list[j]->plugged && pf_list[j]->id == pf_id) {
                found = ctx;
                break;
            }
        }
        free(pf_list);

        if (found)
            break;
    }

    ibv_free_device_list(ibv_list);
    return found;
}

void vrdma_ctrl_progress(void *arg)
{
    struct vrdma_ctrl *ctrl = arg;

    snap_vrdma_ctrl_progress(ctrl->sctrl);
}

#ifndef HAVE_SPDK_POLLER_BUSY
enum spdk_thread_poller_rc { SPDK_POLLER_IDLE , SPDK_POLLER_BUSY };
#endif

int vrdma_ctrl_progress_all_io(void *arg)
{
    struct vrdma_ctrl *ctrl = arg;

    return snap_vrdma_ctrl_io_progress(ctrl->sctrl) ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

int vrdma_ctrl_progress_io(void *arg, int thread_id)
{
    struct vrdma_ctrl *ctrl = arg;

    return snap_vrdma_ctrl_io_progress_thread(ctrl->sctrl, thread_id) ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

void vrdma_ctrl_suspend(void *arg)
{
    struct vrdma_ctrl *ctrl = arg;

    (void)snap_vrdma_ctrl_suspend(ctrl->sctrl);
}

bool vrdma_ctrl_is_suspended(void *arg)
{
    struct vrdma_ctrl *ctrl = arg;

    if (!ctrl->sctrl)
        return true;

    return snap_vrdma_ctrl_is_suspended(ctrl->sctrl) ||
           snap_vrdma_ctrl_is_stopped(ctrl->sctrl);
}

static int vrdma_ctrl_post_flr(void *arg)
{
    struct vrdma_ctrl *ctrl = arg;

    SPDK_NOTICELOG("ctrl %p name '%s' pf_id %d : PCI FLR detected",
            ctrl, ctrl->name, ctrl->pf_id);
    /*
     * Upon FLR, we must cleanup all created mkeys, which happens
     * during spdk_ext_io_context_clear() call. As there might still
     * be IOs inflight, we should do it asynchronously from the
     * IO threads context for an orderly cleanup.
     */
    //vrdma_ctrl_io_channels_clear(ctrl, NULL, NULL);

    return 0;
}

static void vrdma_adminq_dma_cb(struct snap_dma_completion *self, int status)
{
    struct vrdma_admin_queue *admq;
    struct vrdma_admin_sw_qp *sw_qp = container_of(self,
            struct vrdma_admin_sw_qp, init_ci);

    admq = sw_qp->admq;
    sw_qp->pre_ci = admq->ci;
    /* pre_pi should be init as last ci*/
    sw_qp->pre_pi = sw_qp->pre_ci;
	sw_qp->state = VRDMA_CMD_STATE_INIT_CI;
}

static int vrdma_adminq_init(struct vrdma_ctrl *ctrl)
{
    struct vrdma_admin_queue *admq;
    uint32_t aq_size = sizeof(*admq);
    
    admq = spdk_malloc(aq_size, 0x10, NULL, SPDK_ENV_LCORE_ID_ANY,
                             SPDK_MALLOC_DMA);
    if (!admq) {
        return -1;
    }
    ctrl->mr = ibv_reg_mr(ctrl->pd, admq, aq_size,
                    IBV_ACCESS_REMOTE_READ |
                    IBV_ACCESS_REMOTE_WRITE |
                    IBV_ACCESS_LOCAL_WRITE);
    if (!ctrl->mr) {
        spdk_free(admq);
        return -1;
    }
    ctrl->sw_qp.pre_ci = VRDMA_INVALID_CI_PI;
    ctrl->sw_qp.pre_pi = VRDMA_INVALID_CI_PI;
    ctrl->sw_qp.init_ci.func = vrdma_adminq_dma_cb;
    ctrl->sw_qp.init_ci.count = 1;
    ctrl->sw_qp.admq = admq;
	ctrl->sw_qp.state = VRDMA_CMD_STATE_IDLE;
	ctrl->sw_qp.custom_sm = &vrdma_sm;
    return 0;
}

static inline void eth_random_addr(uint8_t *addr)
{
	struct timeval t;
	uint64_t rand;

	gettimeofday(&t, NULL);
	srandom(t.tv_sec + t.tv_usec);
	rand = random();

	rand = rand << 32 | random();

	memcpy(addr, (uint8_t *)&rand, 6);
	addr[0] &= 0xfe;        /* clear multicast bit */
	addr[0] |= 0x02;        /* set local assignment bit (IEEE802) */
}

static int vrdma_device_mac_init(struct vrdma_ctrl *ctrl)
{
	struct snap_device *sdev = ctrl->sctrl->sdev;
	struct snap_vrdma_device_attr vattr = {};
	uint8_t *vmac;
	int ret;

	ret = snap_vrdma_query_device(sdev, &vattr);
	if (ret)
		return -1;
    if (!(vattr.modifiable_fields & SNAP_VRDMA_MOD_MAC))
        return -1;
	vmac = (uint8_t *)&vattr.mac;
	eth_random_addr(&vmac[2]);
	vattr.mac = be64toh(vattr.mac);

	ret = snap_vrdma_modify_device(sdev, SNAP_VRDMA_MOD_MAC, &vattr);
	if (ret)
		ret = -1;

    memcpy(ctrl->gid, &vattr.mac, sizeof(uint64_t));
	return ret;
}

struct vrdma_ctrl *
vrdma_ctrl_init(const struct vrdma_ctrl_init_attr *attr)
{
    struct vrdma_ctrl *ctrl;
    struct snap_vrdma_ctrl_attr sctrl_attr = {};
    struct snap_vrdma_ctrl_bar_cbs bar_cbs = {
        .post_flr = vrdma_ctrl_post_flr,
    };

    ctrl = calloc(1, sizeof(*ctrl));
    if (!ctrl)
        goto err;
    ctrl->nthreads = attr->nthreads;

    ctrl->sctx = vrdma_ctrl_find_snap_context(attr->emu_manager_name,
                            attr->pf_id);
    if (!ctrl->sctx)
        goto free_ctrl;

    ctrl->pd = ibv_alloc_pd(ctrl->sctx->context);
    if (!ctrl->pd)
        goto free_ctrl;
    if (vrdma_adminq_init(ctrl))
        goto dealloc_pd;

    sctrl_attr.bar_cbs = &bar_cbs;
    sctrl_attr.cb_ctx = ctrl;
    sctrl_attr.pf_id = attr->pf_id;
    sctrl_attr.pci_type = SNAP_VRDMA_PF;
    sctrl_attr.pd = ctrl->pd;
    sctrl_attr.npgs = attr->nthreads;
    sctrl_attr.force_in_order = attr->force_in_order;
    sctrl_attr.suspended = attr->suspended;
    sctrl_attr.adminq_size = sizeof(struct vrdma_admin_cmd_entry);
    sctrl_attr.adminq_buf = ctrl->sw_qp.admq;
    sctrl_attr.adminq_dma_comp = (struct snap_dma_completion *)&ctrl->sw_qp.init_ci;
    ctrl->sctrl = snap_vrdma_ctrl_open(ctrl->sctx, &sctrl_attr);
    if (!ctrl->sctrl) {
            SPDK_ERRLOG("Failed to open VRDMA controller %d [in order %d]"
                " over RDMA device %s, PF %d",
                attr->pf_id, attr->force_in_order, attr->emu_manager_name, attr->pf_id);
        goto dereg_mr;
    }
    if (vrdma_device_mac_init(ctrl)) {
            SPDK_ERRLOG("Failed to modify Mac for VRDMA controller %d [in order %d]"
                " over RDMA device %s, PF %d",
                attr->pf_id, attr->force_in_order, attr->emu_manager_name, attr->pf_id);
        goto ctrl_close;
    }

    ctrl->pf_id = attr->pf_id;
    ctrl->vdev = attr->vdev;
    SPDK_NOTICELOG("new VRDMA controller %d [in order %d]"
                  " was opened successfully over RDMA device %s ",
                  attr->pf_id, attr->force_in_order, attr->emu_manager_name);
    snprintf(ctrl->name, VRDMA_EMU_NAME_MAXLEN,
                "%s%dpf%d", VRDMA_EMU_NAME_PREFIX,
                vrdma_dev_name_to_id(attr->emu_manager_name), attr->pf_id);
    strncpy(ctrl->emu_manager, attr->emu_manager_name,
            SPDK_EMU_MANAGER_NAME_MAXLEN - 1);
    return ctrl;

ctrl_close:
    snap_vrdma_ctrl_close(ctrl->sctrl);
dereg_mr:
    ibv_dereg_mr(ctrl->mr);
    spdk_free(ctrl->sw_qp.admq);
dealloc_pd:
    ibv_dealloc_pd(ctrl->pd);
free_ctrl:
    free(ctrl);
err:
    return NULL;
}

static void vrdma_ctrl_free(struct vrdma_ctrl *ctrl)
{
    struct spdk_vrdma_pd *vpd;
    struct spdk_vrdma_mr *vmr;
    struct spdk_vrdma_qp *vqp;
    struct spdk_vrdma_cq *vcq;
    struct spdk_vrdma_eq *veq;

    if (ctrl->mr)
        ibv_dereg_mr(ctrl->mr);
    if (ctrl->sw_qp.admq)
        spdk_free(ctrl->sw_qp.admq);
    ibv_dealloc_pd(ctrl->pd);

    if (ctrl->destroy_done_cb)
        ctrl->destroy_done_cb(ctrl->destroy_done_cb_arg);
    if (ctrl->vdev) {
        LIST_FOREACH(vqp, &ctrl->vdev->vqp_list, entry) {
            LIST_REMOVE(vqp, entry);
            free(vqp);
        }
        LIST_FOREACH(vcq, &ctrl->vdev->vcq_list, entry) {
            LIST_REMOVE(vcq, entry);
            free(vcq);
        }
        LIST_FOREACH(veq, &ctrl->vdev->veq_list, entry) {
            LIST_REMOVE(veq, entry);
            free(veq);
        }
        LIST_FOREACH(vmr, &ctrl->vdev->vmr_list, entry) {
            LIST_REMOVE(vmr, entry);
            free(vmr);
        }
        LIST_FOREACH(vpd, &ctrl->vdev->vpd_list, entry) {
            LIST_REMOVE(vpd, entry);
            ibv_dealloc_pd(vpd->ibpd);
            free(vpd);
        }
        free(ctrl->vdev);
    }
    free(ctrl);
}

void vrdma_ctrl_destroy(void *arg, void (*done_cb)(void *arg),
                             void *done_cb_arg)
{
    struct vrdma_ctrl *ctrl = arg;

    snap_vrdma_ctrl_close(ctrl->sctrl);
    ctrl->sctrl = NULL;
    ctrl->destroy_done_cb = done_cb;
    ctrl->destroy_done_cb_arg = done_cb_arg;
    vrdma_ctrl_free(ctrl);
}
