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
#include "spdk/vrdma_mr.h"
#include "spdk/vrdma_admq.h"
#include "spdk/vrdma_srv.h"
#include "spdk/vrdma_controller.h"
#include "spdk/vrdma_emu_mgr.h"
#include "spdk/vrdma_io_mgr.h"
#include "spdk/vrdma_qp.h"

static bool g_indirect_mkey_map;

/* TODO: use a hash table or sorted list */
struct vrdma_indirect_mkey_list_head vrdma_indirect_mkey_list =
				LIST_HEAD_INITIALIZER(vrdma_indirect_mkey_list);

struct vrdma_r_vkey_list_head vrdma_r_vkey_list =
				LIST_HEAD_INITIALIZER(vrdma_r_vkey_list);

void spdk_vrdma_disable_indirect_mkey_map(void)
{
	g_indirect_mkey_map = false;
}
void spdk_vrdma_enable_indirect_mkey_map(void)
{
	g_indirect_mkey_map = true;
}

static struct vrdma_indirect_mkey *
vrdma_find_indirect_mkey_by_key(uint32_t mkey)
{
	struct vrdma_indirect_mkey *cmkey, *cmkey_tmp;

	if (!g_indirect_mkey_map)
		return NULL;
	LIST_FOREACH_SAFE(cmkey, &vrdma_indirect_mkey_list, entry, cmkey_tmp) {
		if (cmkey->indirect_mkey == mkey)
			return cmkey;
	}
	return NULL;
}

void
vrdma_get_va_crossing_mkey_by_key(uint32_t *mkey, uint64_t *va2pa)
{
	struct vrdma_indirect_mkey *cmkey, *cmkey_tmp;
	uint32_t i;
	uint64_t va = *va2pa;

	if (!g_indirect_mkey_map)
		return;
	LIST_FOREACH_SAFE(cmkey, &vrdma_indirect_mkey_list, entry, cmkey_tmp) {
		if (cmkey->indirect_mkey == *mkey) {
			for (i = 0; i < cmkey->num_sge; i++) {
				if (va >= cmkey->vapa[i].vaddr &&
					va < (cmkey->vapa[i].vaddr + cmkey->vapa[i].size)) {
						*va2pa = cmkey->vapa[i].paddr + (va - cmkey->vapa[i].vaddr);
						*mkey = cmkey->crossing_mkey;
						return;
				}
			}
			return;
		}
	}
}

static void vrdma_del_indirect_mkey_from_list(struct vrdma_indirect_mkey *cmkey)
{
	LIST_REMOVE(cmkey, entry);
	free(cmkey);
}

void vrdma_del_indirect_mkey_list(void)
{
	struct vrdma_indirect_mkey *cmkey, *cmkey_tmp;

	LIST_FOREACH_SAFE(cmkey, &vrdma_indirect_mkey_list, entry, cmkey_tmp) {
		vrdma_del_indirect_mkey_from_list(cmkey);
	}
}

static void vrdma_add_indirect_mkey_list(uint32_t crossing_mkey,
				uint32_t indirect_mkey, struct spdk_vrdma_mr_log *log)
{
	struct vrdma_indirect_mkey *cmkey;
	uint64_t total_size = 0;
	uint32_t i;

	if (!g_indirect_mkey_map)
		return;
	if (log->num_sge > MAX_VRDMA_MR_SGE_NUM) {
		SPDK_ERRLOG("Invalid sge number 0x%x", log->num_sge);
		return;
	}
    cmkey = calloc(1, sizeof(*cmkey));
    if (!cmkey) {
		SPDK_ERRLOG("Failed to allocate crossing_mkey memory for indirect_mkey 0x%x",
			indirect_mkey);
		return;
	}
	cmkey->crossing_mkey = crossing_mkey;
	cmkey->indirect_mkey = indirect_mkey;
	cmkey->num_sge = log->num_sge;
	for (i = 0; i < log->num_sge; ++i) {
		cmkey->vapa[i].vaddr = log->start_vaddr + total_size;
		cmkey->vapa[i].paddr = log->sge[i].paddr;
		cmkey->vapa[i].size = log->sge[i].size;
		total_size += log->sge[i].size;
	}
	LIST_INSERT_HEAD(&vrdma_indirect_mkey_list, cmkey, entry);
}

static inline unsigned int log2above(unsigned int v)
{
	unsigned int l;
	unsigned int r;

	for (l = 0, r = 0; (v >> 1); ++l, v >>= 1)
		r |= (v & 1);
	return l + r;
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

	SPDK_NOTICELOG("dev(%s): start_addr:0x%lx, total_size:0x%lx, "
		  "crossing key:0x%x, log_entity_size:0x%x klm_num:0x%x\n",
		  dev->pci->pci_number, attr->addr, attr->size,
		  crossing_mkey->mkey, attr->log_entity_size, attr->klm_num);
}

static int vrdma_destroy_indirect_mkey(struct spdk_vrdma_mr_log *lattr)
{
	int ret = 0;
	struct vrdma_indirect_mkey *cmkey;

	if (lattr->indirect_mkey) {
		cmkey = vrdma_find_indirect_mkey_by_key(lattr->indirect_mkey->mkey);
		if (cmkey)
			vrdma_del_indirect_mkey_from_list(cmkey);
		ret = snap_destroy_indirect_mkey(lattr->indirect_mkey);
		if (ret)
			SPDK_ERRLOG("\nFailed to destroy indirect mkey, err(%d)\n", ret);
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
		SPDK_ERRLOG("\ndev(%s): Failed to create indirect mkey\n",
			  dev->pci->pci_number);
		goto free_klm_array;
	}
	log->klm_array = attr.klm_array;
	vrdma_add_indirect_mkey_list(crossing_mkey->mkey,
				indirect_mkey->mkey, log);
	return indirect_mkey;
free_klm_array:
	free(attr.klm_array);
	return NULL;
}

int vrdma_create_remote_mkey(struct vrdma_ctrl *ctrl,
					struct spdk_vrdma_mr *vmr)
{
	struct spdk_vrdma_mr_log *lattr;
	uint32_t total_len = 0;

	lattr = &vmr->mr_log;
	lattr->crossing_mkey = ctrl->crossing_mkey;
	if (!lattr->crossing_mkey) {
		SPDK_ERRLOG("\ndev(%s): Failed to create cross mkey\n", ctrl->name);
		return -1;
	}
	if (lattr->num_sge == 1 && !lattr->start_vaddr) {
		lattr->mkey = lattr->crossing_mkey->mkey;
		lattr->log_base = lattr->sge[0].paddr;
		lattr->log_size = lattr->sge[0].size;
		vrdma_add_indirect_mkey_list(lattr->crossing_mkey->mkey,
				lattr->crossing_mkey->mkey, lattr);
	} else {
		/* 3 layers TPT traslation: indirect_mkey -> crossing_mkey -> crossed_mkey */
		lattr->indirect_mkey = vrdma_create_indirect_mkey(ctrl->sctrl->sdev,
									vmr, lattr, &total_len);
		if (!lattr->indirect_mkey) {
			SPDK_ERRLOG("\ndev(%s): Failed to create indirect mkey\n",
					ctrl->name);
			return -1;
		}
		lattr->mkey = lattr->indirect_mkey->mkey;
		lattr->log_size = total_len;
		lattr->log_base = 0;
	}
	SPDK_NOTICELOG("dev(%s): crossing_mkey=0x%x Created remote mkey=0x%x, "
			"start_vaddr=0x%lx, base=0x%lx, size=0x%x\n",
			ctrl->name, lattr->crossing_mkey->mkey, lattr->mkey,
			lattr->start_vaddr, lattr->log_base, lattr->log_size);
	return 0;
}

void vrdma_destroy_remote_mkey(struct vrdma_ctrl *ctrl,
					struct spdk_vrdma_mr *vmr)
{
	struct spdk_vrdma_mr_log *lattr = &vmr->mr_log;

	if (!lattr->mkey) {
		SPDK_ERRLOG("\ndev(%s): remote mkey is not created\n", ctrl->name);
		return;
	}
	vrdma_destroy_indirect_mkey(lattr);
}

void vrdma_reg_mr_create_attr(struct vrdma_create_mr_req *mr_req,
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

static void vrdma_del_r_vkey_tbl_from_list(struct vrdma_r_vkey *r_vkey)
{
	LIST_REMOVE(r_vkey, entry);
	free(r_vkey);
}

void vrdma_del_r_vkey_list(void)
{
	struct vrdma_r_vkey *r_vkey, *vkey_tmp;

	LIST_FOREACH_SAFE(r_vkey, &vrdma_r_vkey_list, entry, vkey_tmp) {
		vrdma_del_r_vkey_tbl_from_list(r_vkey);
	}
}

void vrdma_add_r_vkey_list(uint64_t gid_ip, uint32_t vkey_idx,
			struct vrdma_r_vkey_entry *vkey)
{
	struct vrdma_r_vkey *r_vkey, *vkey_tmp;

	if (vkey_idx >= VRDMA_DEV_MAX_MR)
		return;
	LIST_FOREACH_SAFE(r_vkey, &vrdma_r_vkey_list, entry, vkey_tmp) {
		if (r_vkey->vkey_tbl.gid_ip == gid_ip) {
			r_vkey->vkey_tbl.vkey[vkey_idx].mkey = vkey->mkey;
			r_vkey->vkey_tbl.vkey[vkey_idx].ts = vkey->ts;
			return;
		}
	}
    r_vkey = calloc(1, sizeof(*r_vkey));
    if (!r_vkey) {
		SPDK_ERRLOG("Failed to allocate remote vkey memory for vkey_idx 0x%x",
			vkey_idx);
		return;
	}
	r_vkey->vkey_tbl.gid_ip = gid_ip;
	r_vkey->vkey_tbl.vkey[vkey_idx].mkey = vkey->mkey;
	r_vkey->vkey_tbl.vkey[vkey_idx].ts = vkey->ts;
	LIST_INSERT_HEAD(&vrdma_r_vkey_list, r_vkey, entry);
}

static int vrdma_query_remote_mkey_by_rpc(uint64_t gid_ip,
		uint32_t remote_vqpn, uint32_t vkey_idx)
{
	struct spdk_vrdma_rpc_mkey_msg msg;

	msg.mkey_attr.gid_ip = gid_ip;
	msg.mkey_attr.vqpn = remote_vqpn;
	msg.mkey_attr.vkey = vkey_idx;
	msg.mkey_attr.mkey = 0;
	SPDK_NOTICELOG("remote_vqpn 0x%x gid_ip 0x%lx vkey 0x%x\n",
	msg.mkey_attr.vqpn, msg.mkey_attr.gid_ip, msg.mkey_attr.vkey);
    if (spdk_vrdma_rpc_send_mkey_msg(g_vrdma_rpc.node_rip, &msg)) {
        SPDK_ERRLOG("Fail to send vkey_idx %d to remote qp %d\n",
            vkey_idx, remote_vqpn);
		return -1;
    }
	return 0;
}

uint32_t
vrdma_find_r_mkey(uint64_t gid_ip, uint32_t vkey_idx, uint32_t rvqpn, bool *wait_mkey)
{
	struct vrdma_r_vkey *r_vkey, *vkey_tmp;

	if (vkey_idx >= VRDMA_DEV_MAX_MR)
		return 0;
	LIST_FOREACH_SAFE(r_vkey, &vrdma_r_vkey_list, entry, vkey_tmp) {
		if (r_vkey->vkey_tbl.gid_ip == gid_ip) {
			if (r_vkey->vkey_tbl.vkey[vkey_idx].mkey)
				return r_vkey->vkey_tbl.vkey[vkey_idx].mkey;
			break;
		}
	}
	/* Send rpc to get remote mkey */
	if (vrdma_query_remote_mkey_by_rpc(gid_ip, rvqpn, vkey_idx))
		return 0;
	/* Waiting for rpc resp */
	*wait_mkey = true;
	return 0;
}