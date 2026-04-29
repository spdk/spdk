/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#ifndef SPDK_ACCEL_MLX5_H
#define SPDK_ACCEL_MLX5_H

#ifdef __cplusplus
extern "C" {
#endif

enum spdk_accel_mlx5_dump_state_level {
	/** Dump only grand total statistics */
	SPDK_ACCEL_MLX5_DUMP_STAT_LEVEL_TOTAL,
	/** Dump grand total statistics and per channel statistics over all devices */
	SPDK_ACCEL_MLX5_DUMP_STAT_LEVEL_CHANNEL,
	/** Dump grand total statistics and per channel statistics for each individual device */
	SPDK_ACCEL_MLX5_DUMP_STAT_LEVEL_DEVICE
};

#ifdef __cplusplus
}
#endif

#endif /* SPDK_ACCEL_MLX5_H */
