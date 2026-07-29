/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#ifndef SPDK_ACCEL_MLX5_INTERNAL_H
#define SPDK_ACCEL_MLX5_INTERNAL_H

#include "spdk/stdinc.h"
#include "spdk/module/accel/mlx5.h"

typedef void(*accel_mlx5_dump_stat_done_cb)(void *ctx, int rc);

int accel_mlx5_dump_stats(struct spdk_json_write_ctx *w,
			  enum spdk_accel_mlx5_dump_state_level level,
			  accel_mlx5_dump_stat_done_cb cb, void *ctx);

#endif /* SPDK_ACCEL_MLX5_INTERNAL_H */
