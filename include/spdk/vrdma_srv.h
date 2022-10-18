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

#ifndef __VRDMA_SRV_H__
#define __VRDMA_SRV_H__

#include <stdint.h>
#include "vrdma_admq.h"

struct vrdma_modify_gid_req_param {
	uint8_t gid[16];
};

struct vrdma_create_pd_req_param {
	uint32_t pd_handle;  /* pd handle need to be created in vrdev and passed to vservice */
};

struct vrdma_create_mr_req_param {
	uint32_t mr_handle; /* mr handle, lkey, rkey need to be created in vrdev and passed to vservice */
	uint32_t lkey;
	uint32_t rkey;
};

struct vrdma_destroy_mr_req_param {
	uint32_t mr_handle; /* mr handle need to be created in vrdev and passed to vservice */
};

struct vrdma_cmd_param {
	union {
		char buf[12];
		struct vrdma_modify_gid_req_param modify_gid_param;
		struct vrdma_create_pd_req_param create_pd_param;
		struct vrdma_create_mr_req_param create_mr_param;
		struct vrdma_destroy_mr_req_param destroy_mr_param;
	} param;
};

// following sqe/rqe/cqe/ceqe will be same with mlx structure

// based on mlx sqe
struct sqe {
} __attribute__((packed));

// based on mlx rqe
struct rqe {
	
} __attribute__((packed));

// based on mlx cqe
struct cqe {
	
} __attribute__((packed));

// based on mlx eqe
struct ceqe {
	
} __attribute__((packed));

/*
for following cb functions, device layer need care following return values
1, for create operation, return handle (int type) for gid/eq/cq/qp/pd/ah
2, for modify and destroy operation, return 0: success, -1: failed
*/
typedef int (*vrdma_device_notify_op)(struct vrdma_dev *rdev);
typedef int (*vrdma_admin_query_gid_op)(struct vrdma_dev *rdev, 
										struct vrdma_admin_cmd_entry *cmd);
typedef int (*vrdma_admin_modify_gid_op)(struct vrdma_dev *rdev, 
										struct vrdma_admin_cmd_entry *cmd,
										struct vrdma_cmd_param *param);
typedef int (*vrdma_admin_create_eq_op)(struct vrdma_dev *rdev, 
										struct vrdma_admin_cmd_entry *cmd);
typedef int (*vrdma_admin_modify_eq_op)(struct vrdma_dev *rdev, 
										struct vrdma_admin_cmd_entry *cmd);
typedef int (*vrdma_admin_destroy_eq_op)(struct vrdma_dev *rdev, 
										struct vrdma_admin_cmd_entry *cmd);
typedef int (*vrdma_admin_create_pd_op)(struct vrdma_dev *rdev, 
										struct vrdma_admin_cmd_entry *cmd, 
										struct vrdma_cmd_param *param);
typedef int (*vrdma_admin_destroy_pd_op)(struct vrdma_dev *rdev, 
										struct vrdma_admin_cmd_entry *cmd);
typedef int (*vrdma_admin_create_mr_op)(struct vrdma_dev *rdev, 
										struct vrdma_admin_cmd_entry *cmd, 
										struct vrdma_cmd_param *param);
typedef int (*vrdma_admin_destroy_mr_op)(struct vrdma_dev *rdev, 
										struct vrdma_admin_cmd_entry *cmd, 
										struct vrdma_cmd_param *param);
typedef int (*vrdma_admin_create_cq_op)(struct vrdma_dev *rdev, 
										struct vrdma_admin_cmd_entry *cmd);
typedef int (*vrdma_admin_destroy_cq_op)(struct vrdma_dev *rdev, 
										struct vrdma_admin_cmd_entry *cmd);
typedef int (*vrdma_admin_create_qp_op)(struct vrdma_dev *rdev, 
										struct vrdma_admin_cmd_entry *cmd);
typedef int (*vrdma_admin_destroy_qp_op)(struct vrdma_dev *rdev, 
										struct vrdma_admin_cmd_entry *cmd);
typedef int (*vrdma_admin_query_qp_op)(struct vrdma_dev *rdev, 
										struct vrdma_admin_cmd_entry *cmd, 
										struct vrdma_cmd_param *param);
typedef int (*vrdma_admin_modify_qp_op)(struct vrdma_dev *rdev, 
										struct vrdma_admin_cmd_entry *cmd);
typedef int (*vrdma_admin_create_ah_op)(struct vrdma_dev *rdev, 
										struct vrdma_admin_cmd_entry *cmd);
typedef int (*vrdma_admin_destroy_ah_op)(struct vrdma_dev *rdev, 
										struct vrdma_admin_cmd_entry *cmd);

/* vrdma ops call back exposed to vrdma device */
typedef struct vRdmaServiceOps {
    /* device notify state (probing) to vrdma service */
	vrdma_device_notify_op vrdma_device_notify;
    /* admin callback */
	vrdma_admin_query_gid_op vrdma_device_query_gid;
	vrdma_admin_modify_gid_op vrdma_device_modify_gid;
	vrdma_admin_create_eq_op vrdma_device_create_eq;
	vrdma_admin_modify_eq_op vrdma_device_modify_eq;
	vrdma_admin_destroy_eq_op vrdma_device_destroy_eq;
	vrdma_admin_create_pd_op vrdma_device_create_pd;
	vrdma_admin_destroy_pd_op vrdma_device_destroy_pd;
	vrdma_admin_create_mr_op vrdma_device_create_mr;
	vrdma_admin_destroy_mr_op vrdma_device_destroy_mr;
	vrdma_admin_create_cq_op vrdma_device_create_cq;
	vrdma_admin_destroy_cq_op vrdma_device_destroy_cq;
	vrdma_admin_create_qp_op vrdma_device_create_qp;
	vrdma_admin_destroy_qp_op vrdma_device_destroy_qp;
	vrdma_admin_query_qp_op vrdma_device_query_qp;
	vrdma_admin_modify_qp_op vrdma_device_modify_qp;
	vrdma_admin_create_ah_op vrdma_device_create_ah;
	vrdma_admin_destroy_ah_op vrdma_device_destroy_ah;
} vRdmaServiceOps;

// Assume vrdma service checks the pi,ci boundaries.

// @wqe_head : array to place the batch wqe fetched by dev
uint16_t vrdma_fetch_sq_wqes(struct vrdma_dev *dev, uint32_t qp_handle, uint32_t idx,
                             uint16_t num, void* wqe_head, uint32_t lkey);

//return the number of wqes vdev can provide, maybe less than num param
// @wqe_head: array to place the batch wqe fetched by dev
uint16_t vrdma_fetch_rq_wqes(struct vrdma_dev *dev, uint32_t qp_handle, uint32_t idx,
                             uint16_t num, void* wqe_head, uint32_t lkey);

//assume the cqes are continuous
//cqe_list: the place where eqes are stored
//return: the number of cqes vdev can provide, maybe less than num param
//        0 means failure.
uint16_t vrdma_gen_cqes(struct vrdma_dev *dev, uint32_t cq_handle, uint32_t idx,
                        uint16_t num, struct cqe * cqe_list);

//eqe_list: the place where eqes are stored
//return: the number of ceqes vdev can provide, maybe less than num param
//        0 means failure.
uint16_t vrdma_gen_ceqes(struct vrdma_dev *dev, uint32_t ceq_handle, uint32_t idx,
                         uint16_t num, struct ceqe * eqe_list);

// Generate Interrupt for CEQ:
bool vrdma_gen_ceq_msi(struct vrdma_dev *dev, uint32_t cqe_vector);

// Get SQ PI
//SQ PI should be an attribute cached in vdev.qp.sq, to avoid read from host mem dbr every time
uint16_t vrdma_get_sq_pi(struct vrdma_dev *dev, uint32_t qp_handle);

// Get RQ PI
//RQ PI should be an attribute cached in vdev.qp.rq, to avoid read from host mem dbr every time
uint16_t vrdma_get_rq_pi(struct vrdma_dev *dev, uint32_t qp_handle);

// Get CEQ CI
//EQ CI should be an attribute cached in vdev.cq, to avoid read from host mem dbr every time
uint16_t vrdma_get_cq_ci(struct vrdma_dev *dev, uint32_t cq_handle);

// Get CEQ CI
//EQ CI should be an attribute cached in vdev.eq, to avoid read from host mem dbr every time
uint16_t vrdma_get_eq_ci(struct vrdma_dev *dev, uint32_t eq_handle);

// Replicate data from HostMemory toSoCMemory
bool vrdma_mem_move_h2d(struct vrdma_dev *dev, void *src, uint32_t skey, void *dst, uint32_t dkey, size_t len);

// Replicate data from SoCMemory to HostMemory
bool vrdma_mem_move_d2h(struct vrdma_dev *dev, void *src, uint32_t skey, void *dst, uint32_t dkey, size_t len);

//register MR api for service
struct ibv_mr *vrdma_reg_mr(struct ibv_pd *pd, void *addr, size_t length);

void vrdma_srv_device_init(struct vrdma_ctrl *ctrl);
#endif
