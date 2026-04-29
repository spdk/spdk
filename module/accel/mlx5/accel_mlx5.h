/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/module/accel/mlx5.h"

struct accel_mlx5_attr {
	/* The number of entries in qp submission/receive queue */
	uint16_t qp_size;
	/* The number of requests in the global pool */
	uint32_t num_requests;
	/* Comma separated list of allowed device names */
	char *allowed_devs;
	/* Apply crypto operation for each X data blocks. Works only if multiblock crypto operation is supported by HW.
	 * 0 means no limit */
	uint16_t crypto_split_blocks;
	/* Enables accel_mlx5 platform driver. The driver can execute a limited scope of operations */
	bool enable_driver;
};

typedef void(*accel_mlx5_dump_stat_done_cb)(void *ctx, int rc);

void accel_mlx5_get_default_attr(struct accel_mlx5_attr *attr);
int accel_mlx5_enable(struct accel_mlx5_attr *attr);
int accel_mlx5_dump_stats(struct spdk_json_write_ctx *w,
			  enum spdk_accel_mlx5_dump_state_level level,
			  accel_mlx5_dump_stat_done_cb cb, void *ctx);
