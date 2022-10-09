/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
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

#ifndef __VRDMA_ADMQ_H__
#define __VRDMA_ADMQ_H__

#include <stdint.h>
#include <infiniband/verbs.h>

#define VRDMA_NUM_MSIX_VEC                  (64) 
#define VRDMA_ADMINQ_SIZE                   (1024)
#define VRDMA_ADMINQ_MSG_INLINE_LEN         (64) 
#define VRDMA_CEQ_SIZE                      (1024) /* need to be discussed */
#define VRDMA_ADMINQ_MSIX_VEC_IDX           (0)
#define VRDMA_CEQ_START_MSIX_VEC_IDX        (1)
#define VRDMA_AQ_HDR_MEGIC_NUM 				(0xAA88)


/* more states need mlnx to fill */
enum vrdma_dev_state {
	rdev_state_reset = 0x0,
	rdev_state_driver_ok = 0x4,
	rdev_state_need_reset = 0x40,
	rdev_state_driver_error = 0x80, /*when driver encounter err to inform device*/
	rdev_state_max,
};

/* more fields need mlnx to fill */
struct vrdma_dev {
	uint32_t rdev_idx;
	uint64_t rdev_ver;
	enum vrdma_dev_state state;

	char uuid[20];
	char mac[20];
	char veth[64];
	uint8_t gid[16];

	uint32_t input_pkt_num;
	uint32_t output_pkt_num;

} __attribute__((packed));

/*Per app backend resource creation*/
struct vrdma_open_device_req {
};

struct vrdma_open_device_resp {
	uint32_t err_code:8;
	uint32_t err_hint:24;
} __attribute__((packed));

struct vrdma_query_device_req {
};

struct vrdma_query_device_resp {
	uint32_t err_code:8;
	uint32_t err_hint:24;
	char fw_ver[64]; /* FW version */

	uint64_t page_size_cap;/* Supported memory shift sizes */
	uint64_t dev_cap_flags;/* HCA capabilities mask */
	uint32_t vendor_id; /* Vendor ID, per IEEE */
	uint32_t hw_ver; /* Hardware version */
	uint32_t max_pd;/* Maximum number of supported PDs */
	uint32_t max_qp;/* Maximum number of supported QPs */
	uint32_t max_qp_wr; /* Maximum number of outstanding WR on any work queue */
	uint32_t max_cq; /* Maximum number of supported CQs */
	uint32_t max_sq_depth; /* Maximum number of SQE capacity per SQ */
	uint32_t max_rq_depth; /* Maximum number of RQE capacity per RQ */
	uint32_t max_cq_depth; /* Maximum number of CQE capacity per CQ */
	uint32_t max_mr;/* Largest contiguous block that can be registered */
	uint32_t max_ah; /* Maximum number of supported address handles */

	uint16_t max_qp_rd_atom; /* Maximum number of RDMA Read & Atomic operations that can be outstanding per QP */
	uint16_t max_ee_rd_atom; /* Maximum number of RDMA Read & Atomic operations that can be outstanding per EEC */
	uint16_t max_res_rd_atom;/* Maximum number of resources used for RDMA Read & Atomic operations by this HCA as the Target */
	uint16_t max_qp_init_rd_atom;/* Maximum depth per QP for initiation of RDMA Read & Atomic operations */ 
	uint16_t max_ee_init_rd_atom;/* Maximum depth per EEC for initiation of RDMA Read & Atomic operations */
	uint16_t atomic_cap;/* Atomic operations support level */
	uint16_t masked_atomic_cap; /* Masked atomic operations support level */
	uint16_t sub_cqs_per_cq;
	uint16_t max_pkeys;  /* Maximum number of partitions */
} __attribute__((packed));

struct vrdma_query_port_req {
	uint32_t port_idx;
};

struct vrdma_query_port_resp {
	uint32_t err_code:8;
	uint32_t err_hint:24;
	enum ibv_port_state     state; /* Logical port state */
	enum ibv_mtu            max_mtu;/* Max MTU supported by port */
	enum ibv_mtu            active_mtu; /* Actual MTU */
	int                     gid_tbl_len;/* Length of source GID table */
	uint32_t                port_cap_flags; /* Port capabilities */
	uint32_t                max_msg_sz; /* Length of source GID table */
	uint32_t		bad_pkey_cntr; /* Bad P_Key counter */
	uint32_t		qkey_viol_cntr; /* Q_Key violation counter */
	uint32_t		sm_lid; /* SM LID */
	uint32_t		lid; /* Base port LID */
	uint16_t		pkey_tbl_len; /* Length of partition table */
	uint8_t			lmc; /* LMC of LID */
	uint8_t			max_vl_num; /* Maximum number of VLs */
	uint8_t			sm_sl; /* SM service level */
	uint8_t                 active_speed;
	uint8_t          	phys_state;/* Physical port state */
	uint8_t                 link_layer; /* link layer protocol of the port */

} __attribute__((packed));

struct vrdma_query_gid_req {
};

struct vrdma_query_gid_resp {
	uint32_t err_code:8;
	uint32_t err_hint:24;
	uint8_t gid[16];
} __attribute__((packed));

struct vrdma_modify_gid_req {
	uint8_t gid[16];
};

struct vrdma_modify_gid_resp {
	uint32_t err_code:8;
	uint32_t err_hint:24;
} __attribute__((packed));

struct vrdma_create_ceq_req {
	uint32_t log_depth; /* 2^n */
	uint64_t queue_addr;
	uint16_t vector_idx;
} __attribute__((packed));

struct vrdma_create_ceq_resp {
	uint32_t err_code:8;
	uint32_t err_hint:24;
	uint32_t ceq_handle;
} __attribute__((packed));

struct vrdma_modify_ceq_req {
};

struct vrdma_modify_ceq_resp {
	uint32_t err_code:8;
	uint32_t err_hint:24;
};

struct vrdma_destroy_ceq_req {
	uint32_t ceq_handle;
};

struct vrdma_destroy_ceq_resp {
	uint32_t err_code:8;
	uint32_t err_hint:24;
};

struct vrdma_create_pd_req {
};

struct vrdma_create_pd_resp {
	uint32_t err_code:8;
	uint32_t err_hint:24;
	uint32_t pd_handle;
} __attribute__((packed));

struct vrdma_destroy_pd_req {
	uint32_t pd_handle;
} __attribute__((packed));

struct vrdma_destroy_pd_resp { 
	uint32_t err_code:8;
	uint32_t err_hint:24;
} __attribute__((packed));

struct vrdma_create_mr_req {
	uint32_t pd_handle;
	uint32_t mr_type:3;
	uint32_t access_flags:8;
	uint32_t pagesize:5;
	uint32_t hop:2;
	uint32_t reserved:14;
	uint64_t length; 
	uint64_t vaddr;
	uint32_t sge_count;
	struct vrdma_sge {
		uint64_t va;
		uint64_t pa;
		uint32_t length;
	} sge_list[];
} __attribute__((packed));

struct vrdma_create_mr_resp {
	uint32_t err_code:8;
	uint32_t err_hint:24;
	uint32_t lkey;
	uint32_t rkey;
} __attribute__((packed));

struct vrdma_destroy_mr_req {
	uint32_t lkey;
} __attribute__((packed));

struct vrdma_destroy_mr_resp {
	uint32_t err_code:8;
	uint32_t err_hint:24;
} __attribute__((packed));

struct vrdma_create_cq_req {
	uint32_t log_cqe_entry_num:4; /* 2^n */
	uint32_t log_cqe_size:2; /* 2^n */
	uint32_t log_pagesize:3; /* 2^n */
	uint32_t hop:2;
	uint32_t interrupt_mode:1;
	uint32_t reserved:4;
	uint32_t ceq_handle;
	uint64_t l0_pa; 
} __attribute__((packed));

struct vrdma_create_cq_resp {
	uint32_t err_code:8;
	uint32_t err_hint:24;
	uint32_t cq_handle;
} __attribute__((packed));

struct vrdma_destroy_cq_req {
	uint32_t cq_handle;
} __attribute__((packed));

struct vrdma_destroy_cq_resp { 
	uint32_t err_code:8;
	uint32_t err_hint:24;
};

struct vrdma_create_qp_req {
	uint32_t pd_handle;

	uint32_t qp_type:3;
	uint32_t sq_sig_all:1;
	uint32_t sq_wqebb_size:2; /* based on 64 * (sq_wqebb_size + 1) */
	uint32_t log_sq_pagesize:3; /* 2^n */
	uint32_t sq_hop:2;
	uint32_t rq_wqebb_size:2; /* based on 64 * (rq_wqebb_size + 1) */
	uint32_t log_rq_pagesize:3; /* 2^n */
	uint32_t rq_hop:2;
	uint32_t reserved:5;

	uint32_t log_sq_wqebb_cnt:4; /* sqe entry cnt */
	uint32_t log_rq_wqebb_cnt:4; /* rqe entry cnt */

	uint32_t sq_cqn;
	uint32_t rq_cqn;

	//uint64_t qpc_l0_paddr; /* qpc buffer vm phy addr */
	uint64_t sq_l0_paddr;  /* sqe buffer vm phy addr */
	uint64_t rq_l0_paddr;  /* rqe buffer vm phy addr */
	uint64_t rq_pi_paddr;
} __attribute__((packed));

struct vrdma_create_qp_resp {
	uint32_t err_code:8;
	uint32_t err_hint:24;
	uint32_t qp_handle;
} __attribute__((packed));

struct vrdma_destroy_qp_req {
	uint32_t qp_handle;
} __attribute__((packed));

struct vrdma_destroy_qp_resp { 
	uint32_t err_code:8;
	uint32_t err_hint:24;
} __attribute__((packed));

struct vrdma_query_qp_req {
	uint32_t qp_attr_mask;
	uint32_t qp_handle;
} __attribute__((packed));

struct vrdma_query_qp_resp {
	uint32_t err_code:8;
	uint32_t err_hint:24;
	uint32_t qp_state;
	uint32_t rq_psn;
	uint32_t sq_psn;
	uint32_t dest_qp_num;
	uint32_t sq_draining;
	uint32_t qkey;
} __attribute__((packed));

struct vrdma_modify_qp_req {
	uint32_t qp_attr_mask;
	uint32_t qp_handle;
	uint32_t qp_state;
	uint32_t rq_psn;
	uint32_t sq_psn;
	uint32_t dest_qp_num;
	uint32_t sip;
	uint32_t dip;
	uint32_t qkey;
	uint32_t timeout;
	uint32_t min_rnr_timer;
	uint32_t timeout_retry_cnt;
	uint32_t rnr_retry_cnt;
} __attribute__((packed));

struct vrdma_modify_qp_resp { 
	uint32_t err_code:8;
	uint32_t err_hint:24;
} __attribute__((packed));

struct vrdma_create_ah_req {
	uint32_t pd_handle;
    	uint32_t dip;
} __attribute__((packed));

struct vrdma_create_ah_resp {
	uint32_t err_code:8;
	uint32_t err_hint:24;
	uint32_t ah_handle;
} __attribute__((packed));

struct vrdma_destroy_ah_req {
	uint32_t ah_handle;
} __attribute__((packed));

struct vrdma_destroy_ah_resp {
	uint32_t err_code:8;
	uint32_t err_hint:24;
} __attribute__((packed));

enum vrdma_admin_command_id {
    VRDMA_ADMIN_NONE = 100,
    VRDMA_ADMIN_OPEN_DEVICE,
    VRDMA_ADMIN_QUERY_DEVICE,
    VRDMA_ADMIN_QUERY_PORT,
	VRDMA_ADMIN_QUERY_GID,
	VRDMA_ADMIN_MODIFY_GID,
    VRDMA_ADMIN_CREATE_PD,
    VRDMA_ADMIN_DESTROY_PD,
    VRDMA_ADMIN_REG_MR,
    VRDMA_ADMIN_DEREG_MR,
    VRDMA_ADMIN_CREATE_CQ,
    VRDMA_ADMIN_DESTROY_CQ,
    VRDMA_ADMIN_CREATE_QP,
    VRDMA_ADMIN_DESTROY_QP,
    VRDMA_ADMIN_QUERY_QP,
    VRDMA_ADMIN_MODIFY_QP,
    VRDMA_ADMIN_CREATE_CEQ,
	VRDMA_ADMIN_MODIFY_CEQ,
	VRDMA_ADMIN_DESTROY_CEQ,
    VRDMA_ADMIN_CREATE_AH,
    VRDMA_ADMIN_DESTROY_AH,
    VRDMA_ADMIN_END,
};


/* align cmd format among driver, device and service */
/*
++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
+      HEADER      +                    REQUEST                    +                   RESPOND                   +        
++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
|      8 Bytes     |                    64 Bytes                   |                   64 Bytes                  |
Once the header is determined by driver, there is no need to change it.
@status removed from header and fixed in the first 4 Bytes of RESPONMD, can be modified by both device and service
*/
struct vrdma_admin_cmd_hdr {
	uint32_t seq;
	uint32_t magic: 16; /* 0xAA88 */
	uint32_t version: 6;
	uint32_t is_inline_in: 1; 
    uint32_t is_inline_out: 1;
	uint32_t opcode: 8;
} __attribute((packed));

/*
Currently only query_device_resp and query_port_resp exceed the resp length (64-8 = 56) bytes, 
but the corresponding two requests do not pass to service!
*/
union vrdma_admin_cmd_req {
	char buf[64];  /* 64 Byte */
	struct { 
    	uint16_t    len; 
		uint16_t	reserved[3]; 
    	uint64_t 	pdata; 
	}; 
	struct vrdma_open_device_req open_device_req;
	struct vrdma_query_device_req query_device_req;
	struct vrdma_query_port_req query_port_req;
	struct vrdma_query_gid_req query_gid_req;
	struct vrdma_modify_gid_req modify_gid_req;
	struct vrdma_create_ceq_req create_ceq_req;
	struct vrdma_modify_ceq_req modify_ceq_req;
	struct vrdma_destroy_ceq_req destroy_ceq_req;
	struct vrdma_create_pd_req create_pd_req;
	struct vrdma_destroy_pd_req destroy_pd_req;
	struct vrdma_create_mr_req create_mr_req;
	struct vrdma_destroy_mr_req destroy_mr_req;
	struct vrdma_create_cq_req create_cq_req;
	struct vrdma_destroy_cq_req destroy_cq_req;
	struct vrdma_create_qp_req create_qp_req;
	struct vrdma_destroy_qp_req destroy_qp_req;
	struct vrdma_query_qp_req query_qp_req;
	struct vrdma_modify_qp_req modify_qp_req;
	struct vrdma_create_ah_req create_ah_req;
	struct vrdma_destroy_ah_req destroy_ah_req;
} __attribute((packed));

union vrdma_admin_cmd_resp {
	char buf[64];  /* 64 Byte */
	struct { 
    	uint16_t    len; 
		uint16_t	reserved[3]; 
    	uint64_t 	pdata; 
	}; 
	struct vrdma_open_device_resp open_device_resp;
	struct vrdma_query_device_resp query_device_resp;
	struct vrdma_query_port_resp query_port_resp;
	struct vrdma_query_gid_resp query_gid_resp;
	struct vrdma_modify_gid_resp modify_gid_resp;
	struct vrdma_create_ceq_resp create_ceq_resp;
	struct vrdma_modify_ceq_resp modify_ceq_resp;
	struct vrdma_destroy_ceq_resp destroy_ceq_resp;
	struct vrdma_create_pd_resp create_pd_resp;
	struct vrdma_destroy_pd_resp destroy_pd_resp;
	struct vrdma_create_mr_resp create_mr_resp;
	struct vrdma_destroy_mr_resp destroy_mr_resp;
	struct vrdma_create_cq_resp create_cq_resp;
	struct vrdma_destroy_cq_resp destroy_cq_resp;
	struct vrdma_create_qp_resp create_qp_resp;
	struct vrdma_destroy_qp_resp destroy_qp_resp;
	struct vrdma_query_qp_resp query_qp_resp;
	struct vrdma_modify_qp_resp modify_qp_resp;
	struct vrdma_create_ah_resp create_ah_resp;
	struct vrdma_destroy_ah_resp destroy_ah_resp;
} __attribute((packed));

struct vrdma_admin_cmd_entry { 
    struct vrdma_admin_cmd_hdr hdr;
    union vrdma_admin_cmd_req req;
	union vrdma_admin_cmd_resp resp;
} __attribute__((packed));

struct vrdma_admin_queue {
    uint16_t ci;
    uint16_t pi;
    struct vrdma_admin_cmd_entry ring[VRDMA_ADMINQ_SIZE];
} __attribute__((packed));

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
	}param;
};

#define VRDMA_INVALID_CI_PI 0xFFFF
struct vrdma_snap_dma_completion;

/* Must be same as snap_dma_comp_cb_t in snap lib*/
typedef void (*vrdma_snap_dma_comp_cb_t)(struct vrdma_snap_dma_completion *comp, int status);

/* Must be same as struct snap_dma_completion in snap lib*/
struct vrdma_snap_dma_completion {
	/** @func: callback function. See &typedef snap_dma_comp_cb_t */
	vrdma_snap_dma_comp_cb_t func;
	/** @count: completion counter */
	int                count;
};

struct vrdma_admin_sw_qp {
	uint16_t pre_ci;// invalid -1
	uint16_t pre_pi;// invalid -1 == snap_last_ci
	struct vrdma_admin_queue *admq;
	struct vrdma_snap_dma_completion init_ci;
};

#endif