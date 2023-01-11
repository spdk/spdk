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

#define VRDMA_MAX_BK_QP_PER_VQP 4

struct vrdma_srv_qp {
	LIST_ENTRY(vrdma_srv_qp) entry;
	uint32_t qp_idx;
	struct ibv_pd *pd;
	uint32_t remote_vqpn;
	uint32_t qp_state;
	uint32_t sq_size;
	uint32_t rq_size;
	struct vrdma_backend_qp *bk_qp[VRDMA_MAX_BK_QP_PER_VQP];
};

struct vrdma_srv_pd {
    LIST_ENTRY(vrdma_srv_pd) entry;
	uint32_t pd_idx;
	struct ibv_pd *ibpd;
};

LIST_HEAD(vrdma_srv_qp_list_head, vrdma_srv_qp);

extern struct vrdma_srv_qp_list_head srv_qp_list;

/* Admin-queue message API*/
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

struct vrdma_create_ah_req_param {
	uint32_t ah_handle;  /* ah handle need to be created in vrdev and passed to vservice */
};

struct vrdma_create_eq_req_param {
	uint32_t eq_handle;  /* eq handle need to be created in vrdev and passed to vservice */
};

struct vrdma_create_cq_req_param {
	uint32_t cq_handle;  /* cq handle need to be created in vrdev and passed to vservice */
};

struct vrdma_create_qp_req_param {
	uint32_t qp_handle;  /* qp handle need to be created in vrdev and passed to vservice */
	struct ibv_pd *ibpd;
};

struct vrdma_cmd_param {
	union {
		char buf[12];
		struct vrdma_modify_gid_req_param modify_gid_param;
		struct vrdma_create_pd_req_param create_pd_param;
		struct vrdma_create_mr_req_param create_mr_param;
		struct vrdma_destroy_mr_req_param destroy_mr_param;
		struct vrdma_create_ah_req_param create_ah_param;
		struct vrdma_create_eq_req_param create_eq_param;
		struct vrdma_create_cq_req_param create_cq_param;
		struct vrdma_create_qp_req_param create_qp_param;
	} param;
};

struct vrdma_tx_meta_desc {
    uint32_t reserved1: 4;
    uint32_t opcode: 4;
	uint32_t sge_num:8;
    uint32_t reserved2: 16;
    uint32_t send_flags: 16;
    uint32_t req_id: 16;
    uint32_t length;
    union {
        uint32_t imm_data;
        uint32_t invalid_key;
    }__attribute__((packed));
}__attribute__((packed));

struct vrdma_rdma_rw {
	uint64_t remote_addr;
	uint64_t rkey;
	uint64_t reserved;
}__attribute__((packed));

struct vrdma_rdma_atomic {
	uint64_t remote_addr;
	uint64_t compare_add;
	uint64_t swap;
	uint32_t rkey;
	uint32_t reserved;
}__attribute__((packed));

struct vrdma_rdma_ud{
	uint32_t remote_qpn;
	uint32_t remote_qkey;
	uint32_t ah_handle;
	uint32_t reserved2;
}__attribute__((packed));

/*
 * IO queue buffer descriptor, for any transport type. Preceded by metadata
 * descriptor.
 */
struct vrdma_buf_desc {
	/* Buffer address bits[31:0] */
	uint32_t buf_addr_lo;

	/* Buffer address bits[63:32] */
	uint32_t buf_addr_hi;

	/* length in bytes */
	uint32_t buf_length;

	/*
	 * 23:0 : lkey - local memory translation key
	 * 31:24 : reserved - MBZ
	 */
	uint32_t lkey;
};

/*
++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
+  vrdma send wqe field  |  mlx send wqe field  |                meaning               +
++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
          opcode              ctrl seg(opcode)               operation code
        send_flags           ctrl seg(sm,ce,se)    fence, signaled, solicited, inline
         req_id             ctrl seg(wqe_index)                  wqe index
         length                ctrl seg(DS)      total length of wqe, DS can be calculated
    imm_data/inval_key   ctrl seg(ctrl_general_id)            general identifier
         rdma_rw              remote addr seg              remote addr info
       rdma_atomic        remote addr & atomic seg       info for atomic operation
          ud                  ud addres vector                    UD info
    inline_data/sgl            data segments          memory pointer and inline data
*/
// 128 bytes
struct vrdma_send_wqe {
    /* TX meta */
    struct vrdma_tx_meta_desc meta;
    union {
        struct vrdma_rdma_rw rdma_rw;
        struct vrdma_rdma_atomic rdma_atomic;
        struct vrdma_rdma_ud ud;
    }__attribute__((packed));
    uint32_t reserved[4];
    union {
        struct vrdma_buf_desc sgl[4];
        uint8_t inline_data[64];
    }__attribute__((packed));
} __attribute__((packed));


// 64 bytes
struct vrdma_recv_wqe {
	uint32_t reserved[8];
    struct vrdma_buf_desc sgl[2];;
} __attribute__((packed));


/*
++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
+     mlx cqe field     |    vrdma cqe field    |                meaning               +
++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
        opcode                  opcode              
         NONE                   status               cqe status parsed by upper 
       byte_cnt                 length               byte count of data transferred
     wqe_counter                req_id                          wqe index
         qpn                  local_qpn           
         rqpn                 remote_qpn     
       immediate               imm_data            immediate field of received messages
         ??                       ts                  timestamp need to be disscussed?
*/ 
// 32 bytres
struct vrdma_cqe {
	uint32_t owner: 1;
	uint32_t reserved1:15;
    uint32_t opcode: 8;
    uint32_t status: 8;
    uint32_t length;
    uint32_t reserved2;
    uint32_t req_id: 16;
    uint32_t reserved3: 16;
    uint32_t local_qpn;
    uint32_t remote_qpn;
    uint32_t imm_data;
    uint32_t ts;
} __attribute__((packed));

// 8 bytes
struct vrdma_ceqe {
    uint32_t owner: 1;
	uint32_t reserved1: 7;
    uint32_t cqn: 24;
    uint32_t pi: 20;
    uint32_t reserved2: 12;
} __attribute__((packed));

/*
for following cb functions, device layer need care following return values
1, for create operation, return handle (int type) for gid/eq/cq/qp/pd/ah
2, for modify and destroy operation, return 0: success, -1: failed
*/
typedef int (*vrdma_device_notify_op)(struct vrdma_dev *rdev);
typedef int (*vrdma_admin_open_device_op)(struct vrdma_dev *rdev,
										struct vrdma_admin_cmd_entry *cmd);
typedef int (*vrdma_admin_query_device_op)(struct vrdma_dev *rdev,
										struct vrdma_admin_cmd_entry *cmd);
typedef int (*vrdma_admin_query_port_op)(struct vrdma_dev *rdev,
										struct vrdma_admin_cmd_entry *cmd);
typedef int (*vrdma_admin_query_gid_op)(struct vrdma_dev *rdev,
										struct vrdma_admin_cmd_entry *cmd);
typedef int (*vrdma_admin_modify_gid_op)(struct vrdma_dev *rdev,
										struct vrdma_admin_cmd_entry *cmd,
										struct vrdma_cmd_param *param);
typedef int (*vrdma_admin_create_eq_op)(struct vrdma_dev *rdev,
										struct vrdma_admin_cmd_entry *cmd,
										struct vrdma_cmd_param *param);
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
										struct vrdma_admin_cmd_entry *cmd,
										struct vrdma_cmd_param *param);
typedef int (*vrdma_admin_destroy_cq_op)(struct vrdma_dev *rdev,
										struct vrdma_admin_cmd_entry *cmd);
typedef int (*vrdma_admin_create_qp_op)(struct vrdma_dev *rdev,
										struct vrdma_admin_cmd_entry *cmd,
										struct vrdma_cmd_param *param);
typedef int (*vrdma_admin_destroy_qp_op)(struct vrdma_dev *rdev,
										struct vrdma_admin_cmd_entry *cmd);
typedef int (*vrdma_admin_query_qp_op)(struct vrdma_dev *rdev,
										struct vrdma_admin_cmd_entry *cmd);
typedef int (*vrdma_admin_modify_qp_op)(struct vrdma_dev *rdev,
										struct vrdma_admin_cmd_entry *cmd);
typedef int (*vrdma_admin_create_ah_op)(struct vrdma_dev *rdev,
										struct vrdma_admin_cmd_entry *cmd,
										struct vrdma_cmd_param *param);
typedef int (*vrdma_admin_destroy_ah_op)(struct vrdma_dev *rdev,
										struct vrdma_admin_cmd_entry *cmd);
typedef int (*vrdma_device_map_backend_qp_op)(uint32_t vqpn,
						struct vrdma_backend_qp *bk_qp);

/* vrdma ops call back exposed to vrdma device */
typedef struct vRdmaServiceOps {
    /* device notify state (probing) to vrdma service */
	vrdma_device_notify_op vrdma_device_notify;
    /* admin callback */
	vrdma_admin_open_device_op vrdma_device_open_device;
	vrdma_admin_query_device_op vrdma_device_query_device;
	vrdma_admin_query_port_op vrdma_device_query_port;
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
	/* map backend_qp */
	vrdma_device_map_backend_qp_op vrdma_device_map_backend_qp;
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
                        uint16_t num, struct vrdma_cqe * cqe_list);

//eqe_list: the place where eqes are stored
//return: the number of ceqes vdev can provide, maybe less than num param
//        0 means failure.
uint16_t vrdma_gen_ceqes(struct vrdma_dev *dev, uint32_t ceq_handle, uint32_t idx,
                         uint16_t num, struct vrdma_ceqe * eqe_list);

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

/* data path interfaces */
//@v_rgid   rgid passed from user
//@pd
//@core_num
//@qp_state  passed as parameter in modify_qp
//@vqpn      
//return 0 success, else failed
int vrdma_srv_bind_channel(struct vrdma_dev *rdev, union ibv_gid *v_rgid, struct ibv_pd *pd,
							enum ibv_qp_state qp_state, uint32_t vqpn, uint32_t remote_vqpn);
//@vqpn
//return 0 success, else failed
int vrdma_srv_unbind_channel(struct vrdma_dev *rdev, uint32_t vqpn);
//@vqpn
//bk_qp    the backend_qp returned by vrdma service
//return 0 success, else failed
int vrdma_srv_map_backend_mqp(uint32_t vqpn, struct vrdma_backend_qp *bk_qp);
//@sychrome    the synchrome passed to vrdma service
//@vqpn
//@bk_qp    if service has switched mqp for network error, returned bk_qp, else nothing change
//@return 0 success, else failed
int vrdma_srv_update_backend_channel(uint8_t synchrome, uint32_t vqpn, struct vrdma_backend_qp *bk_qp);
#endif
