/*
 * Copyright (c) 2021, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *
 * This software product is a proprietary product of NVIDIA CORPORATION &
 * AFFILIATES (the "Company") and all right, title, and interest in and to the
 * software product, including all associated intellectual property rights, are
 * and shall remain exclusively with the Company.
 *
 * This software product is governed by the End User License Agreement
 * provided with the software product.
 */

#ifndef __VRDMA_DPA_COMMON_H__
#define __VRDMA_DPA_COMMON_H__

#include <string.h>
#include <stdint.h>
#include <common/flexio_common.h>
// #include <libutil/util.h>


#define DBG_EVENT_HANDLER_CHECK 0x12345604  /*this is only used to check event handler is right*/
#define BIT_ULL(nr)             (1ULL << (nr))
// #define MAX_FETCHED_DESC	32
// #define MAX_EHDR_LEN		142
// #define MAX_VIRTQ_SIZE		256
// #define MAX_VIRTQ_NUM		64
// #define VIRTNET_DPA_SYNC_TIMEOUT 1000

enum{
	MLX5_CTRL_SEG_OPCODE_RDMA_WRITE                      = 0x8,
	MLX5_CTRL_SEG_OPCODE_RDMA_WRITE_WITH_IMMEDIATE       = 0x9,
	MLX5_CTRL_SEG_OPCODE_SEND                            = 0xa,
	MLX5_CTRL_SEG_OPCODE_RDMA_READ                       = 0x10,
};

enum{
	VRDMA_DB_CQ_LOG_DEPTH = 2,
	VRDMA_DB_CQ_ELEM_DEPTH = 6,
};

enum dpa_sync_state_t{
	VRDMA_DPA_SYNC_HOST_RDY = 1,
	VRDMA_DPA_SYNC_DEV_RDY = 2,
};

struct vrdma_dpa_cq {
	uint32_t cq_num;
	uint32_t log_cq_size;/*looks like we don't need*/
	flexio_uintptr_t cq_ring_daddr;
	flexio_uintptr_t cq_dbr_daddr;
	struct flexio_cq *cq;
	uint32_t overrun_ignore;
	uint32_t always_armed;
};

struct vrdma_dpa_vq_desc {
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
};

struct vrdma_dpa_cq_ctx {
	uint32_t cqn;
	uint32_t ci;
	struct flexio_dev_cqe64 *ring;
	struct flexio_dev_cqe64 *cqe;
	uint32_t *dbr;
	uint8_t hw_owner_bit;
	uint32_t log_cq_depth;
};

struct vrdma_dpa_ring_ctx {
        /*Todo: need check*/
	uint32_t num;
	/* Stores the q number which is right shifted by 8 bits to directly
	 * write into the WQE
	 */
	uint32_t num_shift;
	union flexio_dev_sqe_seg *ring;
	uint32_t wqe_seg_idx;
	uint32_t *dbr;
	uint32_t pi;
	uint32_t ci;
};

/* vrdma_dpa_vq_state values:
 *
 * @VRDMA_DPA_VQ_STATE_INIT - VQ is created, but cannot handle doorbells.
 * @VRDMA_DPA_VQ_STATE_SUSPEND - VQ is suspended, no outgoing DMA, can be restarted.
 * @VRDMA_DPA_VQ_STATE_RDY - Can handle doorbells.
 * @VRDMA_DPA_VQ_STATE_ERR - VQ is in error state.
 */
enum vrdma_dpa_vq_state {
	VRDMA_DPA_VQ_STATE_INIT = 1 << 0,
	VRDMA_DPA_VQ_STATE_RDY = 1 << 1,
	VRDMA_DPA_VQ_STATE_SUSPEND = 1 << 2,
	VRDMA_DPA_VQ_STATE_ERR = 1 << 3,
};

struct vrdma_window_dev_config {
	uint32_t window_id;
	uint32_t mkey;
	flexio_uintptr_t haddr;
	flexio_uintptr_t heap_memory;
} __attribute__((__packed__, aligned(8)));


/*host rdma parameters*/
struct vrdma_host_vq_ctx {
	uint64_t rq_pi_paddr;
	uint64_t sq_pi_paddr;
	uint64_t rq_wqe_buff_pa;
	uint64_t sq_wqe_buff_pa;
	uint16_t rq_wqebb_cnt;/*maxed wqebb, pi%rq_wqebb_cnt*/
	uint16_t sq_wqebb_cnt;
	uint16_t rq_wqebb_size;
	uint16_t sq_wqebb_size;

        /*now no sf, sf_ means emu manager*/
	uint32_t sf_crossing_mkey;
	uint32_t emu_crossing_mkey;
} __attribute__((__packed__, aligned(8)));

struct vrdma_arm_vq_ctx {
	/*arm rdma parameters*/
	uint64_t rq_buff_addr;
	uint64_t sq_buff_addr;
	uint64_t rq_pi_addr;
	uint64_t sq_pi_addr;
	uint32_t rq_lkey;
	uint32_t sq_lkey;
} __attribute__((__packed__, aligned(8)));

struct vrdma_dpa_batch {
	uint32_t rq_batch;
	uint32_t rq_times;
	uint64_t rq_total_batchess;
	uint32_t sq_batch;
	uint32_t sq_times;
	uint64_t sq_total_batchess;
};

struct vrdma_dpa_event_handler_ctx {
	uint32_t dbg_signature; /*Todo: used to confirm event handler is right*/

	struct vrdma_dpa_cq_ctx guest_db_cq_ctx;
	struct vrdma_dpa_cq_ctx msix_cq_ctx;

	struct flexio_cq *db_handler_cq;

	uint32_t emu_outbox;
        /*now no sf, so sf_outbox mean emu_manager_outbox*/
	uint32_t sf_outbox;

	uint32_t emu_db_to_cq_id;
	uint32_t window_id;
	flexio_uintptr_t window_base_addr;
	uint16_t vq_index;
	uint16_t rq_last_fetch_start;
	uint16_t sq_last_fetch_start;
	uint16_t rq_last_fetch_end;
	uint16_t sq_last_fetch_end;
	struct {
		struct vrdma_dpa_cq qp_rqcq;
		uint32_t hw_qp_sq_pi;
		uint32_t hw_qp_cq_ci;
		uint32_t hw_qp_depth;
		uint16_t qp_num;
		uint16_t reserved1;
		flexio_uintptr_t qp_sq_buff;
		flexio_uintptr_t qp_rq_buff;
		flexio_uintptr_t dbr_daddr;
		struct vrdma_host_vq_ctx host_vq_ctx; /*host rdma parameters*/
		struct vrdma_arm_vq_ctx arm_vq_ctx; /*arm rdma parameters*/
		enum vrdma_dpa_vq_state state;
	} dma_qp;
	struct vrdma_dpa_batch batch_stats;
	uint32_t pi_count;
	uint32_t wqe_send_count;
	uint32_t count[8];
};

struct vrdma_dpa_vq_data {
	struct vrdma_dpa_event_handler_ctx ehctx;
	enum dpa_sync_state_t state;
	uint8_t err;
} __attribute__((__packed__, aligned(8)));

struct vrdma_dpa_msix_send {
	uint32_t outbox_id;
	uint32_t cqn;
};

enum vrdma_dpa_vq_type {
	VRDMA_DPA_VQ_QP = 0,
	VRDMA_DPA_VQ_MAX
};
#endif
