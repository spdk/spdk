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
#include "snap_mr.h"

#define MAX_VRDMA_DEV_NUM 64
#define MAX_VRDMA_DEV_LEN 32
#define LOG_4K_PAGE_SIZE 12
#define MAX_VRDMA_MR_SGE_NUM 8

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

struct spdk_vrdma_pd {
    LIST_ENTRY(spdk_vrdma_pd) entry;
	uint32_t pd_idx;
	struct ibv_pd *ibpd;
	uint32_t ref_cnt;
};

struct spdk_vrdma_mem_sge {
      uint64_t	paddr;
      uint32_t	size;
};

struct spdk_vrdma_mr_log {
	uint64_t start_vaddr;
	uint64_t log_base;
	uint32_t log_size;
	uint32_t mkey;
	struct mlx5_klm *klm_array;
	struct snap_indirect_mkey *indirect_mkey;
	struct snap_cross_mkey *crossing_mkey;
	uint32_t num_sge;
	struct spdk_vrdma_mem_sge sge[MAX_VRDMA_MR_SGE_NUM];
};

struct spdk_vrdma_mr {
    LIST_ENTRY(spdk_vrdma_mr) entry;
	uint32_t mr_idx;
	struct spdk_vrdma_mr_log mr_log;
	struct spdk_vrdma_pd *vpd;
	uint32_t ref_cnt;
};

struct spdk_vrdma_ah {
    LIST_ENTRY(spdk_vrdma_ah) entry;
	uint32_t ah_idx;
	struct spdk_vrdma_pd *vpd;
	uint32_t dip;
	uint32_t ref_cnt;
};

struct spdk_vrdma_qp {
    LIST_ENTRY(spdk_vrdma_qp) entry;
	uint32_t qp_idx;
	uint32_t ref_cnt;
};

struct spdk_vrdma_cq {
    LIST_ENTRY(spdk_vrdma_cq) entry;
	uint32_t cq_idx;
	uint32_t ref_cnt;
};

struct spdk_vrdma_eq {
    LIST_ENTRY(spdk_vrdma_eq) entry;
	uint32_t eq_idx;
	uint32_t ref_cnt;
};

struct spdk_vrdma_dev {
    uint32_t devid; /*PF_id*/
	char emu_name[MAX_VRDMA_DEV_LEN];
    struct ibv_device *emu_mgr;
	uint32_t vpd_cnt;
	uint32_t vmr_cnt;
	uint32_t vah_cnt;
	uint32_t vqp_cnt;
	uint32_t vcq_cnt;
	uint32_t veq_cnt;
	LIST_HEAD(vpd_list, spdk_vrdma_pd) vpd_list;
	LIST_HEAD(vmr_list, spdk_vrdma_mr) vmr_list;
	LIST_HEAD(vah_list, spdk_vrdma_ah) vah_list;
	LIST_HEAD(vqp_list, spdk_vrdma_qp) vqp_list;
	LIST_HEAD(vcq_list, spdk_vrdma_cq) vcq_list;
	LIST_HEAD(veq_list, spdk_vrdma_eq) veq_list;
};

struct spdk_vrdma_ctx {
    uint32_t dpa_enabled:1;
	char emu_manager[MAX_VRDMA_DEV_LEN];
};

int spdk_vrdma_ctx_start(struct spdk_vrdma_ctx *vrdma_ctx);
void spdk_vrdma_ctx_stop(void (*fini_cb)(void));
#endif
