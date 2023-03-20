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

#ifndef __VRDMA_MR_H__
#define __VRDMA_MR_H__

#include <stdint.h>
#include <infiniband/verbs.h>

#include "snap_dma.h"
#include "vrdma.h"
#include "vrdma_admq.h"

struct vrdma_vapa_map {
	uint64_t vaddr;
	uint64_t paddr;
	uint32_t size;
};
struct vrdma_indirect_mkey {
	LIST_ENTRY(vrdma_indirect_mkey) entry;
	uint32_t indirect_mkey;
	uint32_t crossing_mkey;
	uint32_t num_sge;
	struct vrdma_vapa_map vapa[MAX_VRDMA_MR_SGE_NUM];
};
LIST_HEAD(vrdma_indirect_mkey_list_head, vrdma_indirect_mkey);
extern struct vrdma_indirect_mkey_list_head vrdma_indirect_mkey_list;

struct vrdma_r_vkey_entry {
	uint32_t mkey;
	uint64_t ts;
};
struct vrdma_r_vkey_tbl {
	uint64_t gid_ip;
	struct vrdma_r_vkey_entry vkey[VRDMA_DEV_MAX_MR];
};
struct vrdma_r_vkey {
	LIST_ENTRY(vrdma_r_vkey) entry;
	struct vrdma_r_vkey_tbl vkey_tbl;
};
LIST_HEAD(vrdma_r_vkey_list_head, vrdma_r_vkey);
extern struct vrdma_r_vkey_list_head vrdma_r_vkey_list;

struct vrdma_ctrl;

void spdk_vrdma_disable_indirect_mkey_map(void);
void spdk_vrdma_enable_indirect_mkey_map(void);
void vrdma_del_indirect_mkey_list(void);
void vrdma_get_va_crossing_mkey_by_key(uint32_t *mkey, uint64_t *va2pa);
void vrdma_destroy_remote_mkey(struct vrdma_ctrl *ctrl,
					struct spdk_vrdma_mr *vmr);
int vrdma_create_remote_mkey(struct vrdma_ctrl *ctrl,
					struct spdk_vrdma_mr *vmr);
void vrdma_reg_mr_create_attr(struct vrdma_create_mr_req *mr_req,
				struct spdk_vrdma_mr *vmr);
void vrdma_del_r_vkey_list(void);
uint32_t vrdma_find_r_mkey(uint64_t gid_ip, uint32_t vkey_idx,
				uint32_t rvqpn, bool *wait_mkey);
void vrdma_add_r_vkey_list(uint64_t gid_ip, uint32_t vkey_idx,
				struct vrdma_r_vkey_entry *vkey);
#endif
