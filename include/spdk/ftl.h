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

#ifndef SPDK_FTL_H
#define SPDK_FTL_H

#include "spdk/stdinc.h"
#include "spdk/uuid.h"
#include "spdk/thread.h"
#include "spdk/bdev.h"

#ifdef __cplusplus
extern "C" {
#endif

struct spdk_ftl_dev;

struct spdk_ftl_conf {
	/* Number of reserved addresses not exposed to the user */
	size_t					lba_rsvd;

	/* Core mask - core thread plus additional relocation threads */
	char					*core_mask;

	/* IO pool size per user thread */
	size_t					user_io_pool_size;

	/* Use zone devices, use append instead of write if applicable */
	bool					use_append;

	/* FTL startup mode mask, see spdk_ftl_mode enum for possible values*/
	uint32_t				mode;

	struct {
		/* Maximum number of blocks per one request */
		size_t				max_request_size;

		/* Start compaction when full chunks exceed given % of entire chunks */
		uint32_t			chunk_compaction_threshold;

		/* Percent of chunks to maintain free */
		uint32_t			chunk_free_target;
	} nv_cache;

	/* Create l2p table on l2p_path persistent memory file or device instead of in DRAM */
	char					*l2p_path;

	/* Name of base block device (zoned or non-zoned) */
	char					*base_bdev;

	/* Name of cache block device (must support extended metadata) */
	char					*cache_bdev;

	/* Base bdev reclaim uint size */
	uint64_t				base_bdev_reclaim_unit_size;
};

enum spdk_ftl_mode {
	/* Create new device */
	SPDK_FTL_MODE_CREATE = (1 << 0),
};

struct spdk_ftl_dev_init_opts {
	/* Underlying device */
	const char				*base_bdev;
	/* Write buffer cache */
	const char				*cache_bdev;
	/* Device's config */
	const struct spdk_ftl_conf		*conf;
	/* Device's name */
	const char				*name;
	/* Mode flags */
	unsigned int				mode;
	/* Device UUID (valid when restoring device from disk) */
	struct spdk_uuid			uuid;
};

typedef void (*spdk_ftl_fn)(void *, int);
typedef void (*spdk_ftl_init_fn)(struct spdk_ftl_dev *, void *, int);

/**
 * Initialize the FTL on the given pair of bdevs - base and cache bdev.
 * Upon receiving the completion callback user is free to use I/O calls.
 *
 * \param opts configuration for new device
 * \param cb callback function to call when the device is created
 * \param cb_arg callback's argument
 *
 * \return 0 if initialization was started successfully, negative errno otherwise.
 */
int spdk_ftl_dev_init(const struct spdk_ftl_dev_init_opts *opts, spdk_ftl_init_fn cb, void *cb_arg);

/**
 * Deinitialize and free given device.
 *
 * \param dev device
 * \param cb callback function to call when the device is freed
 * \param cb_arg callback's argument
 *
 * \return 0 if successfully scheduled free, negative errno otherwise.
 */
int spdk_ftl_dev_free(struct spdk_ftl_dev *dev, spdk_ftl_init_fn cb, void *cb_arg);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_FTL_H */
