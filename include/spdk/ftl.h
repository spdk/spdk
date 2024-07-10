/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
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
struct ftl_io;
struct spdk_jsonrpc_request;

/* Limit thresholds */
enum {
	SPDK_FTL_LIMIT_CRIT,
	SPDK_FTL_LIMIT_HIGH,
	SPDK_FTL_LIMIT_LOW,
	SPDK_FTL_LIMIT_START,
	SPDK_FTL_LIMIT_MAX
};

struct ftl_stats_error {
	uint64_t media;
	uint64_t crc;
	uint64_t other;
};

struct ftl_stats_group {
	uint64_t ios;
	uint64_t blocks;
	struct ftl_stats_error errors;
};

struct ftl_stats_entry {
	struct ftl_stats_group read;
	struct ftl_stats_group write;
};

enum ftl_stats_type {
	FTL_STATS_TYPE_USER = 0,
	FTL_STATS_TYPE_CMP,
	FTL_STATS_TYPE_GC,
	FTL_STATS_TYPE_MD_BASE,
	FTL_STATS_TYPE_MD_NV_CACHE,
	FTL_STATS_TYPE_L2P,
	FTL_STATS_TYPE_MAX,
};

struct ftl_stats {
	/* Number of times write limits were triggered by FTL writers
	 * (gc and compaction) dependent on number of free bands. GC starts at
	 * SPDK_FTL_LIMIT_START level, while at SPDK_FTL_LIMIT_CRIT compaction stops
	 * and only GC is allowed to work.
	 */
	uint64_t		limits[SPDK_FTL_LIMIT_MAX];

	/* Total number of blocks with IO to the underlying devices
	 * 1. nv cache read/write
	 * 2. base bdev read/write
	 */
	uint64_t		io_activity_total;

	struct ftl_stats_entry	entries[FTL_STATS_TYPE_MAX];
};

typedef void (*spdk_ftl_stats_fn)(struct ftl_stats *stats, void *cb_arg);

/*
 * FTL configuration.
 *
 * NOTE: Do not change the layout of this structure. Only add new fields at the end.
 */
struct spdk_ftl_conf {
	/* Device's name */
	char					*name;

	/* Device UUID (valid when restoring device from disk) */
	struct spdk_uuid			uuid;

	/* Percentage of base device blocks not exposed to the user */
	uint64_t				overprovisioning;

	/* l2p cache size that could reside in DRAM (in MiB) */
	size_t					l2p_dram_limit;

	/* Core mask - core thread plus additional relocation threads */
	char					*core_mask;

	/* IO pool size per user thread */
	size_t					user_io_pool_size;

	/* User writes limits */
	size_t					limits[SPDK_FTL_LIMIT_MAX];

	/* FTL startup mode mask, see spdk_ftl_mode enum for possible values */
	uint32_t				mode;

	struct {
		/* Start compaction when full chunks exceed given % of entire chunks */
		uint32_t			chunk_compaction_threshold;

		/* Percentage of chunks to maintain free */
		uint32_t			chunk_free_target;
	} nv_cache;

	/*
	 * This flags indicates that FTL during shutdown should execute all
	 * actions which are needed for upgrade to a new version
	 */
	bool					prep_upgrade_on_shutdown;

	/* In verbose mode, user is able to get access to additional advanced FTL properties.
	 *
	 * Advanced properties currently include entries, which will result in printing large amount of data
	 * (e.g. state of all bands, or chunks); or allow for receiving internal state of FTL (e.g. bands currently
	 * used for garbage collection) - live data which may be useful for profiling, or debugging.
	 */
	bool					verbose_mode;

	/* Hole at bytes 0x66 - 0x67. */
	uint8_t					reserved[2];

	/* Name of base block device (zoned or non-zoned) */
	char					*base_bdev;

	/* Name of cache block device (must support extended metadata) */
	char					*cache_bdev;

	/* Enable fast shutdown path */
	bool					fast_shutdown;

	/* Hole at bytes 0x79 - 0x7f. */
	uint8_t					reserved2[7];

	/*
	 * The size of spdk_ftl_conf according to the caller of this library is used for ABI
	 * compatibility. The library uses this field to know how many fields in this
	 * structure are valid. And the library will populate any remaining fields with default values.
	 */
	size_t					conf_size;
} __attribute__((packed));
SPDK_STATIC_ASSERT(sizeof(struct spdk_ftl_conf) == 136, "Incorrect size");

enum spdk_ftl_mode {
	/* Create new device */
	SPDK_FTL_MODE_CREATE = (1 << 0),
};

/*
 * FTL device attributes.
 *
 * NOTE: Do not change the layout of this structure. Only add new fields at the end.
 */
struct spdk_ftl_attrs {
	/* Number of logical blocks */
	uint64_t			num_blocks;
	/* Logical block size */
	uint64_t			block_size;
	/* Optimal IO size - bdev layer will split requests over this size */
	uint64_t			optimum_io_size;
};

typedef void (*spdk_ftl_fn)(void *cb_arg, int status);
typedef void (*spdk_ftl_init_fn)(struct spdk_ftl_dev *dev, void *cb_arg, int status);

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

/**
 * Initialize the FTL on the given pair of bdevs - base and cache bdev.
 * Upon receiving a successful completion callback user is free to use I/O calls.
 *
 * \param conf configuration for new device
 * \param cb callback function to call when the device is created
 * \param cb_arg callback's argument
 *
 * \return 0 if initialization was started successfully, negative errno otherwise.
 */
int spdk_ftl_dev_init(const struct spdk_ftl_conf *conf, spdk_ftl_init_fn cb, void *cb_arg);

/**
 * Deinitialize and free given device.
 *
 * \param dev device
 * \param cb callback function to call when the device is freed
 * \param cb_arg callback's argument
 *
 * \return 0 if deinitialization was started successfully, negative errno otherwise.
 */
int spdk_ftl_dev_free(struct spdk_ftl_dev *dev, spdk_ftl_fn cb, void *cb_arg);

/**
 * Retrieve device’s attributes.
 *
 * \param dev device
 * \param attr Attribute structure to fill
 * \param attrs_size Must be set to sizeof(struct spdk_ftl_attrs)
 */
void spdk_ftl_dev_get_attrs(const struct spdk_ftl_dev *dev, struct spdk_ftl_attrs *attr,
			    size_t attrs_size);

/**
 * Retrieve device’s configuration.
 *
 * \param dev device
 * \param conf FTL configuration structure to fill
 * \param conf_size Must be set to sizeof(struct spdk_ftl_conf)
 */
void spdk_ftl_dev_get_conf(const struct spdk_ftl_dev *dev, struct spdk_ftl_conf *conf,
			   size_t conf_size);

/**
 * Obtain an I/O channel for the device.
 *
 * \param dev device
 *
 * \return A handle to the I/O channel or NULL on failure.
 */
struct spdk_io_channel *spdk_ftl_get_io_channel(struct spdk_ftl_dev *dev);

/**
 * Make a deep copy of an FTL configuration structure
 *
 * \param dst The destination FTL configuration
 * \param src The source FTL configuration
 */
int spdk_ftl_conf_copy(struct spdk_ftl_conf *dst, const struct spdk_ftl_conf *src);

/**
 * Release the FTL configuration resources. This does not free the structure itself.
 *
 * \param conf FTL configuration to deinitialize
 */
void spdk_ftl_conf_deinit(struct spdk_ftl_conf *conf);

/**
 * Initialize FTL configuration structure with default values.
 *
 * \param conf FTL configuration to initialize
 * \param conf_size Must be set to sizeof(struct spdk_ftl_conf)
 */
void spdk_ftl_get_default_conf(struct spdk_ftl_conf *conf, size_t conf_size);

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
		   uint64_t lba, uint64_t lba_cnt,
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
		    uint64_t lba, uint64_t lba_cnt,
		    struct iovec *iov, size_t iov_cnt, spdk_ftl_fn cb_fn, void *cb_arg);

/**
 * Submits a unmap to the specified device.
 *
 * \param dev Device
 * \param io Allocated ftl_io
 * \param ch I/O channel
 * \param lba Starting LBA to write the data
 * \param lba_cnt Number of blocks to unmap
 * \param cb_fn Callback function to invoke when the I/O is completed
 * \param cb_arg Argument to pass to the callback function
 *
 * \return 0 if successfully submitted, negative errno otherwise.
 */
int spdk_ftl_unmap(struct spdk_ftl_dev *dev, struct ftl_io *io, struct spdk_io_channel *ch,
		   uint64_t lba, uint64_t lba_cnt, spdk_ftl_fn cb_fn, void *cb_arg);

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
void spdk_ftl_dev_set_fast_shutdown(struct spdk_ftl_dev *dev, bool fast_shutdown);

/*
 * Returns current FTL I/O statistics.
 *
 * \param dev Device
 * \param stats Allocated ftl_stats
 * \param cb_fn Callback function to invoke when the call is completed
 * \param cb_arg Argument to pass to the callback function
 *
 * \return 0 if successfully submitted, negative errno otherwise.
 */
int spdk_ftl_get_stats(struct spdk_ftl_dev *dev, struct ftl_stats *stats, spdk_ftl_stats_fn cb_fn,
		       void *cb_arg);

/**
 * Gets properties of the specified device.
 *
 * \param dev FTL device
 * \param request JSON RPC request where the properties will be stored
 * \param cb_fn Callback function to invoke when the operation is completed
 * \param cb_arg Argument to pass to the callback function
 *
 * \return 0 if successfully submitted, negative errno otherwise.
 */
int spdk_ftl_get_properties(struct spdk_ftl_dev *dev, struct spdk_jsonrpc_request *request,
			    spdk_ftl_fn cb_fn, void *cb_arg);

/**
 * Sets the property of the specified device.
 *
 * \param dev FTL device
 * \param property The property name to be modified
 * \param value The new value to property
 * \param value_size The size of the value buffer
 * \param cb_fn Callback function to invoke when the operation is completed
 * \param cb_arg Argument to pass to the callback function
 *
 * \return 0 if successfully submitted, negative errno otherwise.
 */
int spdk_ftl_set_property(struct spdk_ftl_dev *dev, const char *property, const char *value,
			  size_t value_size, spdk_ftl_fn cb_fn, void *cb_arg);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_FTL_H */
