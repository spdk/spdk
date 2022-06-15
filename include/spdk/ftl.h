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

/**
 * Initializes the FTL library.
 *
 * @return 0 on success, negative errno otherwise.
 */
int spdk_ftl_init(void);

/**
 * Deinitializes the FTL library.
 */
void spdk_ftl_fini(void);

struct spdk_ftl_dev;
struct ftl_io;

/* Limit thresholds */
enum {
	SPDK_FTL_LIMIT_CRIT,
	SPDK_FTL_LIMIT_HIGH,
	SPDK_FTL_LIMIT_LOW,
	SPDK_FTL_LIMIT_START,
	SPDK_FTL_LIMIT_MAX
};

struct spdk_ftl_conf {
	/* Number of reserved addresses not exposed to the user */
	size_t					lba_rsvd;

	/* l2p cache size that could reside in DRAM (in MiB) */
	size_t					l2p_dram_limit;

	/* Core mask - core thread plus additional relocation threads */
	char					*core_mask;

	/* IO pool size per user thread */
	size_t					user_io_pool_size;

	/* User writes limits */
	size_t			                limits[SPDK_FTL_LIMIT_MAX];

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

	/* Enable fast shutdown path */
	bool					fast_shdn;

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

struct spdk_ftl_attrs {
	/* Device's UUID */
	struct spdk_uuid		uuid;
	/* Number of logical blocks */
	uint64_t				num_blocks;
	/* Logical block size */
	uint64_t				block_size;
	/* Underlying device */
	const char				*base_bdev;
	/* Write buffer cache */
	const char				*cache_bdev;
	/* Number of zones per parallel unit in the underlying device (including any offline ones) */
	uint64_t				num_zones;
	/* Number of logical blocks per zone */
	uint64_t				zone_size;
	/* Optimal IO size - bdev layer will split requests over this size */
	uint64_t				optimum_io_size;
	/* Device specific configuration */
	struct spdk_ftl_conf	conf;
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

/**
 * Initialize FTL configuration structure with default values.
 *
 * \param conf FTL configuration to initialize
 */
void spdk_ftl_conf_init_defaults(struct spdk_ftl_conf *conf);

/**
 * Retrieve device’s attributes.
 *
 * \param dev device
 * \param attr Attribute structure to fill
 */
void spdk_ftl_dev_get_attrs(const struct spdk_ftl_dev *dev, struct spdk_ftl_attrs *attr);

/**
 * Obtain an I/O channel for the device.
 *
 * \param dev device
 *
 * \return A handle to the I/O channel or NULL on failure.
 */
struct spdk_io_channel *spdk_ftl_get_io_channel(struct spdk_ftl_dev *dev);


/**
 * Submits a read to the specified device.
 *
 * \param dev Device
 * \param io Allocated ftl_io
 * \param ch I/O channel
 * \param lba Starting LBA to read the data
 * \param lba_cnt Number of sectors to read
 * \param iov Single IO vector or pointer to IO vector table
 * \param iov_cnt Number of IO vectors
 * \param cb_fn Callback function to invoke when the I/O is completed
 * \param cb_arg Argument to pass to the callback function
 *
 * \return 0 if successfully submitted, negative errno otherwise.
 */
int spdk_ftl_readv(struct spdk_ftl_dev *dev, struct ftl_io *io, struct spdk_io_channel *ch,
		   uint64_t lba, size_t lba_cnt,
		   struct iovec *iov, size_t iov_cnt, spdk_ftl_fn cb_fn, void *cb_arg);

/**
 * Submits a write to the specified device.
 *
 * \param dev Device
 * \param io Allocated ftl_io
 * \param ch I/O channel
 * \param lba Starting LBA to write the data
 * \param lba_cnt Number of sectors to write
 * \param iov Single IO vector or pointer to IO vector table
 * \param iov_cnt Number of IO vectors
 * \param cb_fn Callback function to invoke when the I/O is completed
 * \param cb_arg Argument to pass to the callback function
 *
 * \return 0 if successfully submitted, negative errno otherwise.
 */
int spdk_ftl_writev(struct spdk_ftl_dev *dev, struct ftl_io *io, struct spdk_io_channel *ch,
		    uint64_t lba, size_t lba_cnt,
		    struct iovec *iov, size_t iov_cnt, spdk_ftl_fn cb_fn, void *cb_arg);

/**
 * Returns the size of ftl_io struct that needs to be passed to spdk_ftl_read/write
 *
 * \return The size of struct
 */
size_t spdk_ftl_io_size(void);

/**
 * Enable fast shutdown.
 *
 * During fast shutdown FTL will keep the necessary metadata in shared memory instead
 * of serializing it to storage. This allows for shorter downtime during upgrade process.
 */
void spdk_ftl_dev_set_fast_shdn(struct spdk_ftl_dev *dev, bool fast_shdn);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_FTL_H */
