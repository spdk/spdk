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

#ifndef __VRDMA_ADMQ_H__
#define __VRDMA_ADMQ_H__

#include <stdint.h>
#include <infiniband/verbs.h>

#include "snap_dma.h"
#include "vrdma.h"

extern struct spdk_bit_array *free_vpd_ids;
extern struct spdk_bit_array *free_vmr_ids;
extern struct spdk_bit_array *free_vqp_ids;
extern struct spdk_bit_array *free_vcq_ids;

#define VRDMA_NUM_MSIX_VEC                  (64) 
#define VRDMA_ADMINQ_SIZE                   (1024)
#define VRDMA_ADMINQ_MSG_INLINE_LEN         (64) 
#define VRDMA_CEQ_SIZE                      (1024) /* need to be discussed */
#define VRDMA_ADMINQ_MSIX_VEC_IDX           (0)
#define VRDMA_CEQ_START_MSIX_VEC_IDX        (1)
#define VRDMA_AQ_HDR_MEGIC_NUM 				(0xAA88)

/* more states need mlnx to fill */
enum vrdma_dev_state {
	VRDMA_DEV_STATE_RESET = 0x0,
	VRDMA_DEV_STATE_ACKNOWLEDGE = 0x1,
	VRDMA_DEV_STATE_DRIVER = 0x2,
	VRDMA_DEV_STATE_DRIVER_OK = 0x4,
	VRDMA_DEV_STATE_FEATURES_OK = 0x8,
	VRDMA_DEV_STATE_NEED_RESET = 0x40,
	VRDMA_DEV_STATE_DRIVER_ERROR = 0x80, /*when driver encounter err to inform device*/
	VRDMA_DEV_STATE_MAX,
};

enum vrdma_aq_msg_err_code {
	VRDMA_AQ_MSG_ERR_CODE_SUCCESS = 0x0,
	VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM = 0x1,
	VRDMA_AQ_MSG_ERR_CODE_NO_MEM = 0x2,
	VRDMA_AQ_MSG_ERR_CODE_EXCEED_MAX = 0x3,
	VRDMA_AQ_MSG_ERR_CODE_REF_CNT_INVALID = 0x4,
	VRDMA_AQ_MSG_ERR_CODE_UNKNOWN = 0x5,
	VRDMA_AQ_MSG_ERR_CODE_SERVICE_FAIL = 0x6,
};

enum vrdma_port_phys_state {
	VRDMA_PORT_PHYS_STATE_SLEEP = 1,
	VRDMA_PORT_PHYS_STATE_POLLING = 2,
	VRDMA_PORT_PHYS_STATE_DISABLED = 3,
	VRDMA_PORT_PHYS_STATE_PORT_CONFIGURATION_TRAINING = 4,
	VRDMA_PORT_PHYS_STATE_LINK_UP = 5,
	VRDMA_PORT_PHYS_STATE_LINK_ERROR_RECOVERY = 6,
	VRDMA_PORT_PHYS_STATE_PHY_TEST = 7,
};

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

enum vrdma_device_cap_flags {
	VRDMA_DEVICE_RC_RNR_NAK_GEN		= (1 << 0),
};
#define VRDMA_MAX_PD_NUM     0x40000
#define VRDMA_DEV_MAX_PD     0x2000
#define VRDMA_MAX_MR_NUM     0x40000
#define VRDMA_DEV_MAX_MR     0x2000
#define VRDMA_MAX_QP_NUM     0x40000
#define VRDMA_DEV_MAX_QP     0x2000
#define VRDMA_DEV_MAX_QP_SZ  0x2000000
#define VRDMA_MAX_CQ_NUM     0x40000
#define VRDMA_DEV_MAX_CQ     0x2000
#define VRDMA_DEV_MAX_CQ_DP  0x400
#define VRDMA_DEV_MAX_SQ_DP  0x400
#define VRDMA_DEV_MAX_RQ_DP  0x400

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
	enum ibv_port_state state; /* Logical port state */
	enum ibv_mtu max_mtu;/* Max MTU supported by port */
	enum ibv_mtu active_mtu; /* Actual MTU */
	int gid_tbl_len;/* Length of source GID table */
	uint32_t port_cap_flags; /* Port capabilities */
	uint32_t max_msg_sz; /* Length of source GID table */
	uint32_t bad_pkey_cntr; /* Bad P_Key counter */
	uint32_t qkey_viol_cntr; /* Q_Key violation counter */
	uint32_t sm_lid; /* SM LID */
	uint32_t lid; /* Base port LID */
	uint16_t pkey_tbl_len; /* Length of partition table */
	uint8_t lmc; /* LMC of LID */
	uint8_t max_vl_num; /* Maximum number of VLs */
	uint8_t sm_sl; /* SM service level */
	uint8_t active_speed;
	uint8_t phys_state;/* Physical port state */
	uint8_t link_layer; /* link layer protocol of the port */
} __attribute__((packed));

struct vrdma_query_gid_req {
};

#define VRDMA_DEV_GID_LEN 16
struct vrdma_query_gid_resp {
	uint32_t err_code:8;
	uint32_t err_hint:24;
	uint8_t gid[VRDMA_DEV_GID_LEN];
} __attribute__((packed));

struct vrdma_modify_gid_req {
	uint8_t gid[VRDMA_DEV_GID_LEN];
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
		uint64_t pa;
		uint32_t length;
	} sge_list[MAX_VRDMA_MR_SGE_NUM];
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
	uint64_t sq_pi_paddr;
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
	char buf[256];  /* 256 Byte */
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
	char buf[256];  /* 256 Byte */
	struct {
    	uint16_t    len;
		uint16_t	reserved[3];
    	uint64_t 	pdata;/*host physic address*/
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

/**
 * enum vrdma_aq_cmd_sm_state - state of the sm handling a admq cmd
 * @VRDMA_CMD_STATE_IDLE:            SM initialization state
 * @VRDMA_CMD_STATE_INIT_CI:         set in snap lib when admq is created
 * @VRDMA_CMD_STATE_POLL_PI:         read admq PI from host memory
 * @VRDMA_CMD_STATE_HANDLE_PI:       process PI
 * @VRDMA_CMD_STATE_READ_CMD_ENTRY:  Read admq cmd entry from host memory
 * @VRDMA_CMD_STATE_PARSE_CMD_ENTRY: handle each cmd
 * @VRDMA_CMD_STATE_WRITE_CMD_BACK:  when all cmds are handled, write back to host memory
 * @VRDMA_CMD_STATE_UPDATE_CI:       update admq CI
 * @VRDMA_CMD_STATE_FATAL_ERR:       Fatal error, SM stuck here (until reset)
 * @VRDMA_CMD_NUM_OF_STATES:         should always be the last enum
 */
enum vrdma_aq_cmd_sm_state {
        VRDMA_CMD_STATE_IDLE,
        VRDMA_CMD_STATE_INIT_CI,
        VRDMA_CMD_STATE_POLL_PI,
        VRDMA_CMD_STATE_HANDLE_PI,
        VRDMA_CMD_STATE_READ_CMD_ENTRY,
        VRDMA_CMD_STATE_PARSE_CMD_ENTRY,
        VRDMA_CMD_STATE_WRITE_CMD_BACK,
        VRDMA_CMD_STATE_UPDATE_CI,
        VRDMA_CMD_STATE_FATAL_ERR,
        VRDMA_CMD_NUM_OF_STATES,
};

#define VRDMA_INVALID_CI_PI 0xFFFF

struct vrdma_admin_sw_qp {
	uint16_t pre_ci;
	uint16_t pre_pi;
	enum vrdma_aq_cmd_sm_state state;
	uint16_t num_to_parse;
	struct vrdma_admin_queue *admq;
	struct snap_dma_completion init_ci;
	struct snap_dma_completion poll_comp;
	struct vrdma_state_machine *custom_sm;
};

/**
 * enum vrdma_aq_cmd_sm_op_status - status of last operation
 * @VRDMA_CMD_SM_OP_OK:		Last operation finished without a problem
 * @VRDMA_CMD_SM_OP_ERR:	Last operation failed
 *
 * State machine operates asynchronously, usually by calling a function
 * and providing a callback. Once callback is called it calls the state machine
 * progress again and provides it with the status of the function called.
 * This enum describes the status of the function called.
 */
enum vrdma_aq_cmd_sm_op_status {
	VRDMA_CMD_SM_OP_OK,
	VRDMA_CMD_SM_OP_ERR,
};

struct vrdma_aq_sm_state {
	bool (*sm_handler)(struct vrdma_admin_sw_qp *aq,
				enum vrdma_aq_cmd_sm_op_status status);
};

struct vrdma_state_machine {
	struct vrdma_aq_sm_state *sm_array;
	uint16_t sme;
};

struct vrdma_ctrl;

int vrdma_parse_admq_entry(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe);
int spdk_vrdma_adminq_resource_init(void);
void spdk_vrdma_adminq_resource_destory(void);
void vrdma_aq_sm_dma_cb(struct snap_dma_completion *self, int status);
void vrdma_destroy_remote_mkey(struct vrdma_ctrl *ctrl,
					struct spdk_vrdma_mr *vmr);
#endif
