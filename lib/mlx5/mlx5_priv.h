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
