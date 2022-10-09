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

#ifndef _VRDMA_H
#define _VRDMA_H
#include <stdio.h>
#include <stdint.h>
#include <infiniband/verbs.h>
#include <sys/queue.h>

#define VRDMA_IB_NUM_PORTS 2
#define MAX_VRDMA_DEV_LEN 32

enum vrdma_size {
	VRDMA_VIRTQ_TYPE_SZ	= 2,
	VRDMA_EVENT_MODE_SZ	= 3,
	VRDMA_JSON_EMPTY_SZ	= 4,
	VRDMA_STATUS_ID_SZ	= 8,
	VRDMA_PCI_ADDR_STR_SZ	= 12,
	VRDMA_STR_SZ		= 64,
	VRDMA_FEATURE_SZ	= 64,
	VRDMA_VUID_SZ		= 128,
	VRDMA_PARAM_SZ	= 256,
	VRDMA_FILE_PATH_SZ	= 512,
};

struct spdk_vrdma_port_ctx {
	struct ibv_device *dev;
	char pci_addr_str[VRDMA_PCI_ADDR_STR_SZ + 1];
	uint8_t pci_function;
	uint16_t mtu;
};
struct spdk_vrdma_events_ctx {
	pthread_t tid;
	int	epfd;
};

struct spdk_vrdma_dev {
        uint32_t devid; /*PF_id*/
		char emu_name[MAX_VRDMA_DEV_LEN];
        uint32_t gid_index;
        struct ibv_device *emu_mgr;
        /*LIST_HEAD(pd, vpd) pd_list;
        LIST_HEAD(mr, vmr) mr_list;
        LIST_HEAD(cq, vcq) cq_list;
        LIST_HEAD(eq, veq) eq_list;
        LIST_HEAD(qp, vqp) qp_list;*/
};

struct spdk_vrdma_ctx {
    uint32_t dpa_enabled:1;
	char emu_manager[MAX_VRDMA_DEV_LEN];
    char emu_name[MAX_VRDMA_DEV_LEN];
	struct snap_context *sctx;
	struct spdk_vrdma_port_ctx *port_ctx[VRDMA_IB_NUM_PORTS];
	struct spdk_vrdma_events_ctx dev_ev_ctx;
};

int spdk_vrdma_ctx_start(struct spdk_vrdma_ctx *vrdma_ctx);
void spdk_vrdma_ctx_stop(void (*fini_cb)(void));
#endif
