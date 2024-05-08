/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2023-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/queue.h"
#include "spdk/barrier.h"
#include "spdk/likely.h"

#include <infiniband/mlx5dv.h>
#include "spdk_internal/mlx5.h"

/**
 * Low level CQ representation, suitable for the direct polling
 */
struct mlx5_hw_cq {
	uint64_t cq_addr;
	uint32_t cqe_cnt;
	uint32_t cqe_size;
	uint32_t ci;
	uint32_t cq_num;
};

/**
 * Low level CQ representation, suitable for the WQEs submission.
 * Only submission queue is supported, receive queue is omitted since not used right now
 */
struct mlx5_hw_qp {
	uint64_t dbr_addr;
	uint64_t sq_addr;
	uint64_t sq_bf_addr;
	uint32_t sq_wqe_cnt;
	uint16_t sq_pi;
	uint32_t sq_tx_db_nc;
	uint32_t qp_num;
};

/* qp_num is 24 bits. 2D lookup table uses upper and lower 12 bits to find a qp by qp_num */
#define SPDK_MLX5_QP_NUM_UPPER_SHIFT (12)
#define SPDK_MLX5_QP_NUM_LOWER_MASK ((1 << SPDK_MLX5_QP_NUM_UPPER_SHIFT) - 1)
#define SPDK_MLX5_QP_NUM_LUT_SIZE (1 << 12)

struct spdk_mlx5_cq {
	struct mlx5_hw_cq hw;
	struct {
		struct spdk_mlx5_qp **table;
		uint32_t count;
	} qps [SPDK_MLX5_QP_NUM_LUT_SIZE];
	struct ibv_cq *verbs_cq;
	uint32_t qps_count;
};

struct mlx5_qp_sq_completion {
	uint64_t wr_id;
	/* Number of unsignaled completions before this one. Used to track qp overflow */
	uint32_t completions;
};

struct spdk_mlx5_qp {
	struct mlx5_hw_qp hw;
	struct mlx5_qp_sq_completion *completions;
	/* Pointer to a last WQE controll segment written to SQ */
	struct mlx5_wqe_ctrl_seg *ctrl;
	struct spdk_mlx5_cq *cq;
	struct ibv_qp *verbs_qp;
	/* Number of WQEs submitted to HW which won't produce a CQE */
	uint16_t nonsignaled_outstanding;
	uint16_t max_send_sge;
	/* Number of WQEs available for submission */
	uint16_t tx_available;
	uint16_t last_pi;
	uint8_t sigmode;
};

enum {
	/* Default mode, use flags passed by the user */
	SPDK_MLX5_QP_SIG_NONE = 0,
	/* Enable completion for every control WQE segment, regardless of the flags passed by the user */
	SPDK_MLX5_QP_SIG_ALL = 1,
	/* Enable completion only for the last control WQE segment, regardless of the flags passed by the user */
	SPDK_MLX5_QP_SIG_LAST = 2,
};

/**
 * Completion and Event mode (SPDK_MLX5_WQE_CTRL_CE_*)
 * Maps internal representation of completion events configuration to PRM values
 * g_mlx5_ce_map[][X] is fm_ce_se >> 2 & 0x3 */
static uint8_t g_mlx5_ce_map[3][4] = {
	/* SPDK_MLX5_QP_SIG_NONE */
	[0] = {
		[0] = SPDK_MLX5_WQE_CTRL_CE_CQ_NO_FLUSH_ERROR,
		[1] = SPDK_MLX5_WQE_CTRL_CE_CQ_NO_FLUSH_ERROR,
		[2] = SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE,
		[3] = SPDK_MLX5_WQE_CTRL_CE_CQ_ECE
	},
	/* SPDK_MLX5_QP_SIG_ALL */
	[1] = {
		[0] = SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE,
		[1] = SPDK_MLX5_WQE_CTRL_CE_CQ_NO_FLUSH_ERROR,
		[2] = SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE,
		[3] = SPDK_MLX5_WQE_CTRL_CE_CQ_ECE
	},
	/* SPDK_MLX5_QP_SIG_LAST */
	[2] = {
		[0] = SPDK_MLX5_WQE_CTRL_CE_CQ_NO_FLUSH_ERROR,
		[1] = SPDK_MLX5_WQE_CTRL_CE_CQ_NO_FLUSH_ERROR,
		[2] = SPDK_MLX5_WQE_CTRL_CE_CQ_NO_FLUSH_ERROR,
		[3] = SPDK_MLX5_WQE_CTRL_CE_CQ_ECE
	}
};

struct mlx5_crypto_bsf_seg {
	uint8_t		size_type;
	uint8_t		enc_order;
	uint8_t		rsvd0;
	uint8_t		enc_standard;
	__be32		raw_data_size;
	uint8_t		crypto_block_size_pointer;
	uint8_t		rsvd1[7];
	uint8_t		xts_initial_tweak[16];
	__be32		dek_pointer;
	uint8_t		rsvd2[4];
	uint8_t		keytag[8];
	uint8_t		rsvd3[16];
};

struct mlx5_sig_bsf_inl {
	__be16 vld_refresh;
	__be16 dif_apptag;
	__be32 dif_reftag;
	uint8_t sig_type;
	uint8_t rp_inv_seed;
	uint8_t rsvd[3];
	uint8_t dif_inc_ref_guard_check;
	__be16 dif_app_bitmask_check;
};

struct mlx5_sig_bsf_seg {
	struct mlx5_sig_bsf_basic {
		uint8_t bsf_size_sbs;
		uint8_t check_byte_mask;
		union {
			uint8_t copy_byte_mask;
			uint8_t bs_selector;
			uint8_t rsvd_wflags;
		} wire;
		union {
			uint8_t bs_selector;
			uint8_t rsvd_mflags;
		} mem;
		__be32 raw_data_size;
		__be32 w_bfs_psv;
		__be32 m_bfs_psv;
	} basic;
	struct mlx5_sig_bsf_ext {
		__be32 t_init_gen_pro_size;
		__be32 rsvd_epi_size;
		__be32 w_tfs_psv;
		__be32 m_tfs_psv;
	} ext;
	struct mlx5_sig_bsf_inl w_inl;
	struct mlx5_sig_bsf_inl m_inl;
};

struct mlx5_wqe_set_psv_seg {
	__be32 psv_index;
	__be16 syndrome;
	uint8_t reserved[2];
	__be64 transient_signature;
};

static inline uint8_t
mlx5_qp_fm_ce_se_update(struct spdk_mlx5_qp *qp, uint8_t fm_ce_se)
{
	uint8_t ce = (fm_ce_se >> 2) & 0x3;

	assert((ce & (~0x3)) == 0);
	fm_ce_se &= ~SPDK_MLX5_WQE_CTRL_CE_MASK;
	fm_ce_se |= g_mlx5_ce_map[qp->sigmode][ce];

	return fm_ce_se;
}

static inline void *
mlx5_qp_get_wqe_bb(struct mlx5_hw_qp *hw_qp)
{
	return (void *)hw_qp->sq_addr + (hw_qp->sq_pi & (hw_qp->sq_wqe_cnt - 1)) * MLX5_SEND_WQE_BB;
}

static inline void *
mlx5_qp_get_next_wqebb(struct mlx5_hw_qp *hw_qp, uint32_t *to_end, void *cur)
{
	*to_end -= MLX5_SEND_WQE_BB;
	if (*to_end == 0) { /* wqe buffer wap around */
		*to_end = hw_qp->sq_wqe_cnt * MLX5_SEND_WQE_BB;
		return (void *)(uintptr_t)hw_qp->sq_addr;
	}

	return ((char *)cur) + MLX5_SEND_WQE_BB;
}

static inline void
mlx5_qp_set_comp(struct spdk_mlx5_qp *qp, uint16_t pi,
		 uint64_t wr_id, uint32_t fm_ce_se, uint32_t n_bb)
{
	qp->completions[pi].wr_id = wr_id;
	if ((fm_ce_se & SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE) != SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE) {
		/* non-signaled WQE, accumulate it in outstanding */
		qp->nonsignaled_outstanding += n_bb;
		qp->completions[pi].completions = 0;
		return;
	}

	/* Store number of previous nonsignaled WQEs */
	qp->completions[pi].completions = qp->nonsignaled_outstanding + n_bb;
	qp->nonsignaled_outstanding = 0;
}

#if defined(__aarch64__)
#define spdk_memory_bus_store_fence()  asm volatile("dmb oshst" ::: "memory")
#elif defined(__i386__) || defined(__x86_64__)
#define spdk_memory_bus_store_fence() spdk_wmb()
#endif

static inline void
mlx5_update_tx_db(struct spdk_mlx5_qp *qp)
{
	/*
	 * Use cpu barrier to prevent code reordering
	 */
	spdk_smp_wmb();

	((uint32_t *)qp->hw.dbr_addr)[MLX5_SND_DBR] = htobe32(qp->hw.sq_pi);
}

static inline void
mlx5_flush_tx_db(struct spdk_mlx5_qp *qp, struct mlx5_wqe_ctrl_seg *ctrl)
{
	*(uint64_t *)(qp->hw.sq_bf_addr) = *(uint64_t *)ctrl;
}

static inline void
mlx5_ring_tx_db(struct spdk_mlx5_qp *qp, struct mlx5_wqe_ctrl_seg *ctrl)
{
	/* 8.9.3.1  Posting a Work Request to Work Queue
	 * 1. Write WQE to the WQE buffer sequentially to previously-posted
	 *    WQE (on WQEBB granularity)
	 *
	 * 2. Update Doorbell Record associated with that queue by writing
	 *    the sq_wqebb_counter or wqe_counter for send and RQ respectively
	 */
	mlx5_update_tx_db(qp);

	/* Make sure that doorbell record is written before ringing the doorbell */
	spdk_memory_bus_store_fence();

	/* 3. For send request ring DoorBell by writing to the Doorbell
	 *    Register field in the UAR associated with that queue
	 */
	mlx5_flush_tx_db(qp, ctrl);

	/* If UAR is mapped as WC (write combined) we need another fence to
	 * force write. Otherwise it may take a long time.
	 * On BF2/1 uar is mapped as NC (non combined) and fence is not needed
	 * here.
	 */
#if !defined(__aarch64__)
	if (!qp->hw.sq_tx_db_nc) {
		spdk_memory_bus_store_fence();
	}
#endif
}

#ifdef DEBUG
void mlx5_qp_dump_wqe(struct spdk_mlx5_qp *qp, int n_wqe_bb);
#else
#define mlx5_qp_dump_wqe(...) do { } while (0)
#endif

static inline void
mlx5_qp_wqe_submit(struct spdk_mlx5_qp *qp, struct mlx5_wqe_ctrl_seg *ctrl, uint16_t n_wqe_bb,
		   uint16_t ctrlr_pi)
{
	mlx5_qp_dump_wqe(qp, n_wqe_bb);

	/* Delay ringing the doorbell */
	qp->hw.sq_pi += n_wqe_bb;
	qp->last_pi = ctrlr_pi;
	qp->ctrl = ctrl;
}

static inline void
mlx5_set_ctrl_seg(struct mlx5_wqe_ctrl_seg *ctrl, uint16_t pi,
		  uint8_t opcode, uint8_t opmod, uint32_t qp_num,
		  uint8_t fm_ce_se, uint8_t ds,
		  uint8_t signature, uint32_t imm)
{
	*(uint32_t *)((void *)ctrl + 8) = 0;
	mlx5dv_set_ctrl_seg(ctrl, pi, opcode, opmod, qp_num,
			    fm_ce_se, ds, signature, imm);
}

static inline struct spdk_mlx5_qp *
mlx5_cq_find_qp(struct spdk_mlx5_cq *cq, uint32_t qp_num)
{
	uint32_t qpn_upper = qp_num >> SPDK_MLX5_QP_NUM_UPPER_SHIFT;
	uint32_t qpn_mask = qp_num & SPDK_MLX5_QP_NUM_LOWER_MASK;

	if (spdk_unlikely(!cq->qps[qpn_upper].count)) {
		return NULL;
	}
	return cq->qps[qpn_upper].table[qpn_mask];
}

static inline int
mlx5_get_pd_id(struct ibv_pd *pd, uint32_t *pd_id)
{
	struct mlx5dv_pd pd_info;
	struct mlx5dv_obj obj;
	int rc;

	if (!pd) {
		return -EINVAL;
	}
	obj.pd.in = pd;
	obj.pd.out = &pd_info;
	rc = mlx5dv_init_obj(&obj, MLX5DV_OBJ_PD);
	if (rc) {
		return rc;
	}
	*pd_id = pd_info.pdn;

	return 0;
}
