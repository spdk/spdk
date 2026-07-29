/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#ifndef SPDK_ACCEL_MLX5_H
#define SPDK_ACCEL_MLX5_H

#include "spdk/stdinc.h"
#include "spdk/assert.h"

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

/**
 * Configuration attributes for the accel_mlx5 module.
 *
 * Use spdk_accel_mlx5_get_default_attr() to initialize with default values,
 * then modify as needed before passing to spdk_accel_mlx5_enable().
 */
struct spdk_accel_mlx5_attr {
	/**
	 * The size of spdk_accel_mlx5_attr according to the caller of this
	 * library is used for ABI compatibility.  The library uses this field
	 * to know how many fields in this structure are valid.  And the library
	 * will populate any remaining fields with default values.
	 */
	size_t opts_size;
	/** The number of entries in qp submission/receive queue */
	uint16_t qp_size;
	/** The number of requests in the global pool */
	uint32_t num_requests;
	/** Comma separated list of allowed device names (NULL for all devices) */
	char *allowed_devs;
	/** Apply crypto operation for each X data blocks. Works only if multiblock
	 *  crypto operation is supported by HW. 0 means no limit */
	uint16_t crypto_split_blocks;
	/** Enables accel_mlx5 platform driver for UMR operations */
	bool enable_driver;
} __attribute__((packed));
SPDK_STATIC_ASSERT(sizeof(struct spdk_accel_mlx5_attr) == 25, "Incorrect size");

/**
 * Initialize spdk_accel_mlx5_attr with default values.
 *
 * \param attr Pointer to the attribute structure to initialize.
 * \param opts_size Must be set to sizeof(struct spdk_accel_mlx5_attr).
 */
void spdk_accel_mlx5_get_default_attr(struct spdk_accel_mlx5_attr *attr, size_t opts_size);

/**
 * Enable the accel_mlx5 module with the specified attributes.
 *
 * \param attr Configuration attributes (NULL to use defaults).
 * \return 0 on success, -EEXIST if already enabled, or other negative errno on failure.
 */
int spdk_accel_mlx5_enable(struct spdk_accel_mlx5_attr *attr);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_ACCEL_MLX5_H */
