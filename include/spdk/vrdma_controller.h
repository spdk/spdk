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

#ifndef _VRDMA_CTRL_H
#define _VRDMA_CTRL_H
#include <stdio.h>
#include <stdbool.h>
#include "spdk/stdinc.h"
#include "spdk/vrdma.h"
#include "spdk/vrdma_admq.h"
#include "spdk/vrdma_emu_mgr.h"
#include "spdk/vrdma_srv.h"
#include "spdk/vrdma_qp.h"
#include "spdk/vrdma_rpc.h"

#define VRDMA_EMU_NAME_PREFIX "VrdmaEmu"
#define VRDMA_EMU_NAME_MAXLEN 32
#define VRDMA_DMA_ELEM_SIZE 64

extern struct vrdma_state_machine vrdma_sm;

struct snap_vrdma_ctrl;
struct snap_context;

struct vrdma_ctrl {
    char name[VRDMA_EMU_NAME_MAXLEN];
    char emu_manager[SPDK_EMU_MANAGER_NAME_MAXLEN];
    size_t nthreads;
    int pf_id;
    uint32_t dev_inited:1;
    struct vrdma_dev dev;
    struct spdk_vrdma_dev *vdev;
    struct snap_context *sctx;
    struct ibv_pd *pd;
    struct ibv_mr *mr;
    struct vrdma_admin_sw_qp sw_qp;
    struct snap_vrdma_ctrl *sctrl;
    /** Service-specific callbacks. */
	const struct vRdmaServiceOps *srv_ops;
    void (*destroy_done_cb)(void *arg);
    void *destroy_done_cb_arg;
    LIST_HEAD(bk_qp_list, vrdma_backend_qp) bk_qp_list;
};

struct vrdma_ctrl_init_attr {
    const char *emu_manager_name;
    int pf_id;
    struct spdk_vrdma_dev *vdev;
    uint32_t nthreads;
    bool force_in_order;
    bool suspended;
};

int vrdma_ctrl_adminq_progress(void *ctrl);
void vrdma_ctrl_progress(void *ctrl);
int vrdma_ctrl_progress_all_io(void *ctrl);
int vrdma_ctrl_progress_io(void *arg, int thread_id);
void vrdma_ctrl_suspend(void *ctrl);
bool vrdma_ctrl_is_suspended(void *ctrl);

struct vrdma_ctrl *
vrdma_ctrl_init(const struct vrdma_ctrl_init_attr *attr);
struct vrdma_ctrl *
vrdma_find_ctrl_by_srv_dev(struct vrdma_dev *rdev);
void vrdma_ctrl_destroy(void *arg, void (*done_cb)(void *arg),
                             void *done_cb_arg);
int vrdma_dev_name_to_id(const char *rdma_dev_name);

uint8_t vrdma_get_qp_status(uint32_t qp_handle);

#endif
