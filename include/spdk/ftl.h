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

/* Limit thresholds */
enum {
	SPDK_FTL_LIMIT_CRIT,
	SPDK_FTL_LIMIT_HIGH,
	SPDK_FTL_LIMIT_LOW,
	SPDK_FTL_LIMIT_START,
	SPDK_FTL_LIMIT_MAX
};

struct spdk_ftl_limit {
	/* Threshold from which the limiting starts */
	size_t					thld;

	/* Limit percentage */
	size_t					limit;
};

struct spdk_ftl_conf {
	/* Number of reserved addresses not exposed to the user */
	size_t					lba_rsvd;

	/* Size of the per-io_channel write buffer */
	size_t					write_buffer_size;

	/* Threshold for opening new band */
	size_t					band_thld;

	/* Maximum IO depth per band relocate */
	size_t					max_reloc_qdepth;

	/* Maximum active band relocates */
	size_t					max_active_relocs;

	/* IO pool size per user thread */
	size_t					user_io_pool_size;

	/* Lowest percentage of invalid blocks for a band to be defragged */
	size_t					invalid_thld;

	/* User writes limits */
	struct spdk_ftl_limit			limits[SPDK_FTL_LIMIT_MAX];

	/* Allow for partial recovery from open bands instead of returning error */
	bool					allow_open_bands;

	/* Use append instead of write */
	bool					use_append;

	/* Maximum supported number of IO channels */
	uint32_t				max_io_channels;

	struct {
		/* Maximum number of concurrent requests */
		size_t				max_request_cnt;
		/* Maximum number of blocks per one request */
		size_t				max_request_size;
	} nv_cache;

	/* Create l2p table on l2p_path persistent memory file or device instead of in DRAM */
	const char				*l2p_path;
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

	/* Thread responsible for core tasks execution */
	struct spdk_thread			*core_thread;

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
	struct spdk_uuid			uuid;
	/* Number of logical blocks */
	uint64_t				num_blocks;
	/* Logical block size */
	size_t					block_size;
	/* Underlying device */
	const char				*base_bdev;
	/* Write buffer cache */
	const char				*cache_bdev;
	/* Number of zones per parallel unit in the underlying device (including any offline ones) */
	size_t					num_zones;
	/* Number of logical blocks per zone */
	size_t					zone_size;
	/* Device specific configuration */
	struct spdk_ftl_conf			conf;
};

typedef void (*spdk_ftl_fn)(void *, int);
typedef void (*spdk_ftl_init_fn)(struct spdk_ftl_dev *, void *, int);

/**
 * Initialize the FTL on given NVMe device and parallel unit range.
 *
 * Covers the following:
 * - retrieve zone device information,
 * - allocate buffers and resources,
 * - initialize internal structures,
 * - initialize internal thread(s),
 * - restore or create L2P table.
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
 * Submits a read to the specified device.
 *
 * \param dev Device
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
int spdk_ftl_read(struct spdk_ftl_dev *dev, struct spdk_io_channel *ch, uint64_t lba,
		  size_t lba_cnt,
		  struct iovec *iov, size_t iov_cnt, spdk_ftl_fn cb_fn, void *cb_arg);

/**
 * Submits a write to the specified device.
 *
 * \param dev Device
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
int spdk_ftl_write(struct spdk_ftl_dev *dev, struct spdk_io_channel *ch, uint64_t lba,
		   size_t lba_cnt,
		   struct iovec *iov, size_t iov_cnt, spdk_ftl_fn cb_fn, void *cb_arg);

/**
 * Submits a flush request to the specified device.
 *
 * \param dev device
 * \param cb_fn Callback function to invoke when all prior IOs have been completed
 * \param cb_arg Argument to pass to the callback function
 *
 * \return 0 if successfully submitted, negative errno otherwise.
 */
int spdk_ftl_flush(struct spdk_ftl_dev *dev, spdk_ftl_fn cb_fn, void *cb_arg);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_FTL_H */
