/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_VBDEV_DELAY_H
#define SPDK_VBDEV_DELAY_H

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/bdev_module.h"

enum delay_io_type {
	DELAY_AVG_READ,
	DELAY_P99_READ,
	DELAY_AVG_WRITE,
	DELAY_P99_WRITE,
	DELAY_NONE
};

/**
 * Create new delay bdev.
 *
 * \param bdev_name Bdev on which delay vbdev will be created.
 * \param vbdev_name Name of the delay bdev.
 * \param avg_read_latency Desired typical read latency.
 * \param p99_read_latency Desired p99 read latency
 * \param avg_write_latency Desired typical write latency.
 * \param p99_write_latency Desired p99 write latency
 * \return 0 on success, other on failure.
 */
int create_delay_disk(const char *bdev_name, const char *vbdev_name, uint64_t avg_read_latency,
		      uint64_t p99_read_latency, uint64_t avg_write_latency, uint64_t p99_write_latency);

/**
 * Delete delay bdev.
 *
 * \param vbdev_name Name of the delay bdev.
 * \param cb_fn Function to call after deletion.
 * \param cb_arg Argument to pass to cb_fn.
 */
void delete_delay_disk(const char *vbdev_name, spdk_bdev_unregister_cb cb_fn,
		       void *cb_arg);

/**
 * Update one of the latency values for a given delay bdev.
 *
 * \param delay_name The name of the delay bdev
 * \param latency_us The new latency value, in microseconds
 * \param type a valid value from the delay_io_type enum
 * \return 0 on success, -ENODEV if the bdev cannot be found, and -EINVAL if the bdev is not a delay device.
 */
int vbdev_delay_update_latency_value(char *delay_name, uint64_t latency_us,
				     enum delay_io_type type);

#endif /* SPDK_VBDEV_DELAY_H */
