/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

/** \file
 * Block device abstraction layer
 */

#ifndef SPDK_BDEV_H_
#define SPDK_BDEV_H_

#include "spdk/stdinc.h"

#include "spdk/scsi_spec.h"
#include "spdk/nvme_spec.h"
#include "spdk/json.h"
#include "spdk/queue.h"
#include "spdk/histogram_data.h"
#include "spdk/dif.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SPDK_BDEV_SMALL_BUF_MAX_SIZE 8192
#define SPDK_BDEV_LARGE_BUF_MAX_SIZE (64 * 1024)

/* Increase the buffer size to store interleaved metadata.  Increment is the
 *  amount necessary to store metadata per data block.  16 byte metadata per
 *  512 byte data block is the current maximum ratio of metadata per block.
 */
#define SPDK_BDEV_BUF_SIZE_WITH_MD(x)	(((x) / 512) * (512 + 16))

/** Asynchronous event type */
enum spdk_bdev_event_type {
	SPDK_BDEV_EVENT_REMOVE,
	SPDK_BDEV_EVENT_RESIZE,
	SPDK_BDEV_EVENT_MEDIA_MANAGEMENT,
};

/** Media management event details */
struct spdk_bdev_media_event {
	uint64_t	offset;
	uint64_t	num_blocks;
};

/**
 * \brief SPDK block device.
 *
 * This is a virtual representation of a block device that is exported by the backend.
 */
struct spdk_bdev;

/**
 * Block device remove callback.
 *
 * \param remove_ctx Context for the removed block device.
 */
typedef void (*spdk_bdev_remove_cb_t)(void *remove_ctx);

/**
 * Block device event callback.
 *
 * \param type Event type.
 * \param bdev Block device that triggered event.
 * \param event_ctx Context for the block device event.
 */
typedef void (*spdk_bdev_event_cb_t)(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
				     void *event_ctx);

/**
 * Block device I/O
 *
 * This is an I/O that is passed to an spdk_bdev.
 */
struct spdk_bdev_io;

struct spdk_bdev_fn_table;
struct spdk_io_channel;
struct spdk_json_write_ctx;
struct spdk_uuid;

/** bdev status */
enum spdk_bdev_status {
	SPDK_BDEV_STATUS_INVALID,
	SPDK_BDEV_STATUS_READY,
	SPDK_BDEV_STATUS_REMOVING,
};

/**
 * \brief Handle to an opened SPDK block device.
 */
struct spdk_bdev_desc;

/** bdev I/O type */
enum spdk_bdev_io_type {
	SPDK_BDEV_IO_TYPE_INVALID = 0,
	SPDK_BDEV_IO_TYPE_READ,
	SPDK_BDEV_IO_TYPE_WRITE,
	SPDK_BDEV_IO_TYPE_UNMAP,
	SPDK_BDEV_IO_TYPE_FLUSH,
	SPDK_BDEV_IO_TYPE_RESET,
	SPDK_BDEV_IO_TYPE_NVME_ADMIN,
	SPDK_BDEV_IO_TYPE_NVME_IO,
	SPDK_BDEV_IO_TYPE_NVME_IO_MD,
	SPDK_BDEV_IO_TYPE_WRITE_ZEROES,
	SPDK_BDEV_IO_TYPE_ZCOPY,
	SPDK_BDEV_IO_TYPE_GET_ZONE_INFO,
	SPDK_BDEV_IO_TYPE_ZONE_MANAGEMENT,
	SPDK_BDEV_IO_TYPE_ZONE_APPEND,
	SPDK_BDEV_IO_TYPE_COMPARE,
	SPDK_BDEV_IO_TYPE_COMPARE_AND_WRITE,
	SPDK_BDEV_IO_TYPE_ABORT,
	SPDK_BDEV_NUM_IO_TYPES /* Keep last */
};

/** bdev QoS rate limit type */
enum spdk_bdev_qos_rate_limit_type {
	/** IOPS rate limit for both read and write */
	SPDK_BDEV_QOS_RW_IOPS_RATE_LIMIT = 0,
	/** Byte per second rate limit for both read and write */
	SPDK_BDEV_QOS_RW_BPS_RATE_LIMIT,
	/** Byte per second rate limit for read only */
	SPDK_BDEV_QOS_R_BPS_RATE_LIMIT,
	/** Byte per second rate limit for write only */
	SPDK_BDEV_QOS_W_BPS_RATE_LIMIT,
	/** Keep last */
	SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES
};

/**
 * Block device completion callback.
 *
 * \param bdev_io Block device I/O that has completed.
 * \param success True if I/O completed successfully or false if it failed;
 * additional error information may be retrieved from bdev_io by calling
 * spdk_bdev_io_get_nvme_status() or spdk_bdev_io_get_scsi_status().
 * \param cb_arg Callback argument specified when bdev_io was submitted.
 */
typedef void (*spdk_bdev_io_completion_cb)(struct spdk_bdev_io *bdev_io,
		bool success,
		void *cb_arg);

struct spdk_bdev_io_stat {
	uint64_t bytes_read;
	uint64_t num_read_ops;
	uint64_t bytes_written;
	uint64_t num_write_ops;
	uint64_t bytes_unmapped;
	uint64_t num_unmap_ops;
	uint64_t read_latency_ticks;
	uint64_t write_latency_ticks;
	uint64_t unmap_latency_ticks;
	uint64_t ticks_rate;
};

struct spdk_bdev_opts {
	uint32_t bdev_io_pool_size;
	uint32_t bdev_io_cache_size;
	bool bdev_auto_examine;
	/**
	 * The size of spdk_bdev_opts according to the caller of this library is used for ABI
	 * compatibility.  The library uses this field to know how many fields in this
	 * structure are valid. And the library will populate any remaining fields with default values.
	 * New added fields should be put at the end of the struct.
	 */
	size_t opts_size;

	uint32_t small_buf_pool_size;
	uint32_t large_buf_pool_size;
};

/**
 * Structure with optional IO request parameters
 * The content of this structure must be valid until the IO request is completed
 */
struct spdk_bdev_ext_io_opts {
	/** Size of this structure in bytes */
	size_t size;
	/** Memory domain which describes payload in this IO request. bdev must support DMA device type that
	 * can access this memory domain, refer to \ref spdk_bdev_get_memory_domains and \ref spdk_memory_domain_get_dma_device_type
	 * If set, that means that data buffers can't be accessed directly and the memory domain must
	 * be used to fetch data to local buffers or to translate data to another memory domain */
	struct spdk_memory_domain *memory_domain;
	/** Context to be passed to memory domain operations */
	void *memory_domain_ctx;
	/** Metadata buffer, optional */
	void *metadata;
};

/**
 * Get the options for the bdev module.
 *
 * \param opts Output parameter for options.
 * \param opts_size sizeof(*opts)
 */
void spdk_bdev_get_opts(struct spdk_bdev_opts *opts, size_t opts_size);

int spdk_bdev_set_opts(struct spdk_bdev_opts *opts);

typedef void (*spdk_bdev_wait_for_examine_cb)(void *arg);

/**
 * Report when all bdevs finished the examine process.
 * The registered cb_fn will be called just once.
 * This function needs to be called again to receive
 * further reports on examine process.
 *
 * \param cb_fn Callback function.
 * \param cb_arg Callback argument.
 * \return 0 if function was registered, suitable errno value otherwise
 */
int spdk_bdev_wait_for_examine(spdk_bdev_wait_for_examine_cb cb_fn, void *cb_arg);

/**
 * Examine a block device explicitly
 *
 * \param name the name or alias of the block device
 * \return 0 if block device was examined successfully, suitable errno value otherwise
 */
int spdk_bdev_examine(const char *name);

/**
 * Block device initialization callback.
 *
 * \param cb_arg Callback argument.
 * \param rc 0 if block device initialized successfully or negative errno if it failed.
 */
typedef void (*spdk_bdev_init_cb)(void *cb_arg, int rc);

/**
 * Block device finish callback.
 *
 * \param cb_arg Callback argument.
 */
typedef void (*spdk_bdev_fini_cb)(void *cb_arg);
typedef void (*spdk_bdev_get_device_stat_cb)(struct spdk_bdev *bdev,
		struct spdk_bdev_io_stat *stat, void *cb_arg, int rc);

/**
 * Block device channel IO timeout callback
 *
 * \param cb_arg Callback argument
 * \param bdev_io The IO cause the timeout
 */
typedef void (*spdk_bdev_io_timeout_cb)(void *cb_arg, struct spdk_bdev_io *bdev_io);

/**
 * Initialize block device modules.
 *
 * \param cb_fn Called when the initialization is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bdev_initialize(spdk_bdev_init_cb cb_fn, void *cb_arg);

/**
 * Perform cleanup work to remove the registered block device modules.
 *
 * \param cb_fn Called when the removal is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bdev_finish(spdk_bdev_fini_cb cb_fn, void *cb_arg);

/**
 * Get the full configuration options for the registered block device modules and created bdevs.
 *
 * \param w pointer to a JSON write context where the configuration will be written.
 */
void spdk_bdev_subsystem_config_json(struct spdk_json_write_ctx *w);

/**
 * Get block device module name.
 *
 * \param bdev Block device to query.
 * \return Name of bdev module as a null-terminated string.
 */
const char *spdk_bdev_get_module_name(const struct spdk_bdev *bdev);

/**
 * Get block device by the block device name.
 *
 * \param bdev_name The name of the block device.
 * \return Block device associated with the name or NULL if no block device with
 * bdev_name is currently registered.
 */
struct spdk_bdev *spdk_bdev_get_by_name(const char *bdev_name);

/**
 * Get the first registered block device.
 *
 * \return The first registered block device.
 */
struct spdk_bdev *spdk_bdev_first(void);

/**
 * Get the next registered block device.
 *
 * \param prev The current block device.
 * \return The next registered block device.
 */
struct spdk_bdev *spdk_bdev_next(struct spdk_bdev *prev);

/**
 * Get the first block device without virtual block devices on top.
 *
 * This function only traverses over block devices which have no virtual block
 * devices on top of them, then get the first one.
 *
 * \return The first block device without virtual block devices on top.
 */
struct spdk_bdev *spdk_bdev_first_leaf(void);

/**
 * Get the next block device without virtual block devices on top.
 *
 * This function only traverses over block devices which have no virtual block
 * devices on top of them, then get the next one.
 *
 * \param prev The current block device.
 * \return The next block device without virtual block devices on top.
 */
struct spdk_bdev *spdk_bdev_next_leaf(struct spdk_bdev *prev);

/**
 * Open a block device for I/O operations.
 *
 * \param bdev_name Block device name to open.
 * \param write true is read/write access requested, false if read-only
 * \param event_cb notification callback to be called when the bdev triggers
 * asynchronous event such as bdev removal. This will always be called on the
 * same thread that spdk_bdev_open_ext() was called on. In case of removal event
 * the descriptor will have to be manually closed to make the bdev unregister
 * proceed.
 * \param event_ctx param for event_cb.
 * \param desc output parameter for the descriptor when operation is successful
 * \return 0 if operation is successful, suitable errno value otherwise
 */
int spdk_bdev_open_ext(const char *bdev_name, bool write, spdk_bdev_event_cb_t event_cb,
		       void *event_ctx, struct spdk_bdev_desc **desc);

/**
 * Close a previously opened block device.
 *
 * Must be called on the same thread that the spdk_bdev_open_ext()
 * was performed on.
 *
 * \param desc Block device descriptor to close.
 */
void spdk_bdev_close(struct spdk_bdev_desc *desc);

/**
 * Get the bdev associated with a bdev descriptor.
 *
 * \param desc Open block device descriptor
 * \return bdev associated with the descriptor
 */
struct spdk_bdev *spdk_bdev_desc_get_bdev(struct spdk_bdev_desc *desc);

/**
 * Set a time limit for the timeout IO of the bdev and timeout callback.
 * We can use this function to enable/disable the timeout handler. If
 * the timeout_in_sec > 0 then it means to enable the timeout IO handling
 * or change the time limit. If the timeout_in_sec == 0 it means to
 * disable the timeout IO handling. If you want to enable or change the
 * timeout IO handle you need to specify the spdk_bdev_io_timeout_cb it
 * means the upper user determines what to do if you meet the timeout IO,
 * for example, you can reset the device or abort the IO.
 * Note: This function must run in the desc's thread.
 *
 * \param desc Block device descriptor.
 * \param timeout_in_sec Timeout value
 * \param cb_fn Bdev IO timeout callback
 * \param cb_arg Callback argument
 *
 * \return 0 on success, negated errno on failure.
 */
int spdk_bdev_set_timeout(struct spdk_bdev_desc *desc, uint64_t timeout_in_sec,
			  spdk_bdev_io_timeout_cb cb_fn, void *cb_arg);

/**
 * Check whether the block device supports the I/O type.
 *
 * \param bdev Block device to check.
 * \param io_type The specific I/O type like read, write, flush, unmap.
 * \return true if support, false otherwise.
 */
bool spdk_bdev_io_type_supported(struct spdk_bdev *bdev, enum spdk_bdev_io_type io_type);

/**
 * Output driver-specific information to a JSON stream.
 *
 * The JSON write context will be initialized with an open object, so the bdev
 * driver should write a name(based on the driver name) followed by a JSON value
 * (most likely another nested object).
 *
 * \param bdev Block device to query.
 * \param w JSON write context. It will store the driver-specific configuration context.
 * \return 0 on success, negated errno on failure.
 */
int spdk_bdev_dump_info_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w);

/**
 * Get block device name.
 *
 * \param bdev Block device to query.
 * \return Name of bdev as a null-terminated string.
 */
const char *spdk_bdev_get_name(const struct spdk_bdev *bdev);

/**
 * Get block device product name.
 *
 * \param bdev Block device to query.
 * \return Product name of bdev as a null-terminated string.
 */
const char *spdk_bdev_get_product_name(const struct spdk_bdev *bdev);

/**
 * Get block device logical block size.
 *
 * \param bdev Block device to query.
 * \return Size of logical block for this bdev in bytes.
 */
uint32_t spdk_bdev_get_block_size(const struct spdk_bdev *bdev);

/**
 * Get the write unit size for this bdev.
 *
 * Write unit size is required number of logical blocks to perform write
 * operation on block device.
 *
 * Unit of write unit size is logical block and the minimum of write unit
 * size is one. Write operations must be multiple of write unit size.
 *
 * \param bdev Block device to query.
 *
 * \return The write unit size in logical blocks.
 */
uint32_t spdk_bdev_get_write_unit_size(const struct spdk_bdev *bdev);

/**
 * Get size of block device in logical blocks.
 *
 * \param bdev Block device to query.
 * \return Size of bdev in logical blocks.
 *
 * Logical blocks are numbered from 0 to spdk_bdev_get_num_blocks(bdev) - 1, inclusive.
 */
uint64_t spdk_bdev_get_num_blocks(const struct spdk_bdev *bdev);

/**
 * Get the string of quality of service rate limit.
 *
 * \param type Type of rate limit to query.
 * \return String of QoS type.
 */
const char *spdk_bdev_get_qos_rpc_type(enum spdk_bdev_qos_rate_limit_type type);

/**
 * Get the quality of service rate limits on a bdev.
 *
 * \param bdev Block device to query.
 * \param limits Pointer to the QoS rate limits array which holding the limits.
 *
 * The limits are ordered based on the @ref spdk_bdev_qos_rate_limit_type enum.
 */
void spdk_bdev_get_qos_rate_limits(struct spdk_bdev *bdev, uint64_t *limits);

/**
 * Set the quality of service rate limits on a bdev.
 *
 * \param bdev Block device.
 * \param limits Pointer to the QoS rate limits array which holding the limits.
 * \param cb_fn Callback function to be called when the QoS limit has been updated.
 * \param cb_arg Argument to pass to cb_fn.
 *
 * The limits are ordered based on the @ref spdk_bdev_qos_rate_limit_type enum.
 */
void spdk_bdev_set_qos_rate_limits(struct spdk_bdev *bdev, uint64_t *limits,
				   void (*cb_fn)(void *cb_arg, int status), void *cb_arg);

/**
 * Get minimum I/O buffer address alignment for a bdev.
 *
 * \param bdev Block device to query.
 * \return Required alignment of I/O buffers in bytes.
 */
size_t spdk_bdev_get_buf_align(const struct spdk_bdev *bdev);

/**
 * Get optimal I/O boundary for a bdev.
 *
 * \param bdev Block device to query.
 * \return Optimal I/O boundary in blocks that should not be crossed for best performance, or 0 if
 *         no optimal boundary is reported.
 */
uint32_t spdk_bdev_get_optimal_io_boundary(const struct spdk_bdev *bdev);

/**
 * Query whether block device has an enabled write cache.
 *
 * \param bdev Block device to query.
 * \return true if block device has a volatile write cache enabled.
 *
 * If this function returns true, written data may not be persistent until a flush command
 * is issued.
 */
bool spdk_bdev_has_write_cache(const struct spdk_bdev *bdev);

/**
 * Get a bdev's UUID.
 *
 * \param bdev Block device to query.
 * \return Pointer to UUID.
 *
 * All bdevs will have a UUID, but not all UUIDs will be persistent across
 * application runs.
 */
const struct spdk_uuid *spdk_bdev_get_uuid(const struct spdk_bdev *bdev);

/**
 * Get block device atomic compare and write unit.
 *
 * \param bdev Block device to query.
 * \return Atomic compare and write unit for this bdev in blocks.
 */
uint16_t spdk_bdev_get_acwu(const struct spdk_bdev *bdev);

/**
 * Get block device metadata size.
 *
 * \param bdev Block device to query.
 * \return Size of metadata for this bdev in bytes.
 */
uint32_t spdk_bdev_get_md_size(const struct spdk_bdev *bdev);

/**
 * Query whether metadata is interleaved with block data or separated
 * with block data.
 *
 * \param bdev Block device to query.
 * \return true if metadata is interleaved with block data or false
 * if metadata is separated with block data.
 *
 * Note this function is valid only if there is metadata.
 */
bool spdk_bdev_is_md_interleaved(const struct spdk_bdev *bdev);

/**
 * Query whether metadata is interleaved with block data or separated
 * from block data.
 *
 * \param bdev Block device to query.
 * \return true if metadata is separated from block data, false
 * otherwise.
 *
 * Note this function is valid only if there is metadata.
 */
bool spdk_bdev_is_md_separate(const struct spdk_bdev *bdev);

/**
 * Checks if bdev supports zoned namespace semantics.
 *
 * \param bdev Block device to query.
 * \return true if device supports zoned namespace semantics.
 */
bool spdk_bdev_is_zoned(const struct spdk_bdev *bdev);

/**
 * Get block device data block size.
 *
 * Data block size is equal to block size if there is no metadata or
 * metadata is separated with block data, or equal to block size minus
 * metadata size if there is metadata and it is interleaved with
 * block data.
 *
 * \param bdev Block device to query.
 * \return Size of data block for this bdev in bytes.
 */
uint32_t spdk_bdev_get_data_block_size(const struct spdk_bdev *bdev);

/**
 * Get block device physical block size.
 *
 * \param bdev Block device to query.
 * \return Size of physical block size for this bdev in bytes.
 */
uint32_t spdk_bdev_get_physical_block_size(const struct spdk_bdev *bdev);

/**
 * Get DIF type of the block device.
 *
 * \param bdev Block device to query.
 * \return DIF type of the block device.
 */
enum spdk_dif_type spdk_bdev_get_dif_type(const struct spdk_bdev *bdev);

/**
 * Check whether DIF is set in the first 8 bytes or the last 8 bytes of metadata.
 *
 * \param bdev Block device to query.
 * \return true if DIF is set in the first 8 bytes of metadata, or false
 * if DIF is set in the last 8 bytes of metadata.
 *
 * Note that this function is valid only if DIF type is not SPDK_DIF_DISABLE.
 */
bool spdk_bdev_is_dif_head_of_md(const struct spdk_bdev *bdev);

/**
 * Check whether the DIF check type is enabled.
 *
 * \param bdev Block device to query.
 * \param check_type The specific DIF check type.
 * \return true if enabled, false otherwise.
 */
bool spdk_bdev_is_dif_check_enabled(const struct spdk_bdev *bdev,
				    enum spdk_dif_check_type check_type);

/**
 * Get the most recently measured queue depth from a bdev.
 *
 * The reported queue depth is the aggregate of outstanding I/O
 * across all open channels associated with this bdev.
 *
 * \param bdev Block device to query.
 *
 * \return The most recent queue depth measurement for the bdev.
 * If tracking is not enabled, the function will return UINT64_MAX
 * It is also possible to receive UINT64_MAX after enabling tracking
 * but before the first period has expired.
 */
uint64_t
spdk_bdev_get_qd(const struct spdk_bdev *bdev);

/**
 * Get the queue depth polling period.
 *
 * The return value of this function is only valid if the bdev's
 * queue depth tracking status is set to true.
 *
 * \param bdev Block device to query.
 *
 * \return The period at which this bdev's gueue depth is being refreshed.
 */
uint64_t
spdk_bdev_get_qd_sampling_period(const struct spdk_bdev *bdev);

/**
 * Enable or disable queue depth sampling for this bdev.
 *
 * Enables queue depth sampling when period is greater than 0. Disables it when the period
 * is equal to zero. The resulting queue depth is stored in the spdk_bdev object as
 * measured_queue_depth.
 *
 * \param bdev Block device on which to enable queue depth tracking.
 * \param period The period at which to poll this bdev's queue depth. If this is set
 * to zero, polling will be disabled.
 */
void spdk_bdev_set_qd_sampling_period(struct spdk_bdev *bdev, uint64_t period);

/**
 * Get the time spent processing IO for this device.
 *
 * This value is dependent upon the queue depth sampling period and is
 * incremented at sampling time by the sampling period only if the measured
 * queue depth is greater than 0.
 *
 * The disk utilization can be calculated by the following formula:
 * disk_util = (io_time_2 - io_time_1) / elapsed_time.
 * The user is responsible for tracking the elapsed time between two measurements.
 *
 * \param bdev Block device to query.
 *
 * \return The io time for this device in microseconds.
 */
uint64_t spdk_bdev_get_io_time(const struct spdk_bdev *bdev);

/**
 * Get the weighted IO processing time for this bdev.
 *
 * This value is dependent upon the queue depth sampling period and is
 * equal to the time spent reading from or writing to a device times
 * the measured queue depth during each sampling period.
 *
 * The average queue depth can be calculated by the following formula:
 * queue_depth = (weighted_io_time_2 - weighted_io_time_1) / elapsed_time.
 * The user is responsible for tracking the elapsed time between two measurements.
 *
 * \param bdev Block device to query.
 *
 * \return The weighted io time for this device in microseconds.
 */
uint64_t spdk_bdev_get_weighted_io_time(const struct spdk_bdev *bdev);

/**
 * Obtain an I/O channel for the block device opened by the specified
 * descriptor. I/O channels are bound to threads, so the resulting I/O
 * channel may only be used from the thread it was originally obtained
 * from.
 *
 * \param desc Block device descriptor.
 *
 * \return A handle to the I/O channel or NULL on failure.
 */
struct spdk_io_channel *spdk_bdev_get_io_channel(struct spdk_bdev_desc *desc);

/**
 * Obtain a bdev module context for the block device opened by the specified
 * descriptor.
 *
 * \param desc Block device descriptor.
 *
 * \return A bdev module context or NULL on failure.
 */
void *spdk_bdev_get_module_ctx(struct spdk_bdev_desc *desc);

/**
 * \defgroup bdev_io_submit_functions bdev I/O Submit Functions
 *
 * These functions submit a new I/O request to a bdev.  The I/O request will
 *  be represented by an spdk_bdev_io structure allocated from a global pool.
 *  These functions will return -ENOMEM if the spdk_bdev_io pool is empty.
 */

/**
 * Submit a read request to the bdev on the given channel.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param buf Data buffer to read into.
 * \param offset The offset, in bytes, from the start of the block device.
 * \param nbytes The number of bytes to read.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset and/or nbytes are not aligned or out of range
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_bdev_read(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		   void *buf, uint64_t offset, uint64_t nbytes,
		   spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a read request to the bdev on the given channel.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param buf Data buffer to read into.
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param num_blocks The number of blocks to read.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset_blocks and/or num_blocks are out of range
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_bdev_read_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			  void *buf, uint64_t offset_blocks, uint64_t num_blocks,
			  spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a read request to the bdev on the given channel. This function uses
 * separate buffer for metadata transfer (valid only if bdev supports this
 * mode).
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param buf Data buffer to read into.
 * \param md Metadata buffer.
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param num_blocks The number of blocks to read.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset_blocks and/or num_blocks are out of range or separate
 *               metadata is not supported
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_bdev_read_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				  void *buf, void *md, uint64_t offset_blocks, uint64_t num_blocks,
				  spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a read request to the bdev on the given channel. This differs from
 * spdk_bdev_read by allowing the data buffer to be described in a scatter
 * gather list. Some physical devices place memory alignment requirements on
 * data and may not be able to directly transfer into the buffers provided. In
 * this case, the request may fail.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param iov A scatter gather list of buffers to be read into.
 * \param iovcnt The number of elements in iov.
 * \param offset The offset, in bytes, from the start of the block device.
 * \param nbytes The number of bytes to read.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset and/or nbytes are not aligned or out of range
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_bdev_readv(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		    struct iovec *iov, int iovcnt,
		    uint64_t offset, uint64_t nbytes,
		    spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a read request to the bdev on the given channel. This differs from
 * spdk_bdev_read by allowing the data buffer to be described in a scatter
 * gather list. Some physical devices place memory alignment requirements on
 * data and may not be able to directly transfer into the buffers provided. In
 * this case, the request may fail.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param iov A scatter gather list of buffers to be read into.
 * \param iovcnt The number of elements in iov.
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param num_blocks The number of blocks to read.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset_blocks and/or num_blocks are out of range
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_bdev_readv_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			   struct iovec *iov, int iovcnt,
			   uint64_t offset_blocks, uint64_t num_blocks,
			   spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a read request to the bdev on the given channel. This differs from
 * spdk_bdev_read by allowing the data buffer to be described in a scatter
 * gather list. Some physical devices place memory alignment requirements on
 * data or metadata and may not be able to directly transfer into the buffers
 * provided. In this case, the request may fail. This function uses separate
 * buffer for metadata transfer (valid only if bdev supports this mode).
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param iov A scatter gather list of buffers to be read into.
 * \param iovcnt The number of elements in iov.
 * \param md Metadata buffer.
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param num_blocks The number of blocks to read.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset_blocks and/or num_blocks are out of range or separate
 *               metadata is not supported
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_bdev_readv_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				   struct iovec *iov, int iovcnt, void *md,
				   uint64_t offset_blocks, uint64_t num_blocks,
				   spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a read request to the bdev on the given channel. This differs from
 * spdk_bdev_read by allowing the data buffer to be described in a scatter
 * gather list. Some physical devices place memory alignment requirements on
 * data or metadata and may not be able to directly transfer into the buffers
 * provided. In this case, the request may fail. This function uses separate
 * buffer for metadata transfer (valid only if bdev supports this mode).
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param iov A scatter gather list of buffers to be read into.
 * \param iovcnt The number of elements in iov.
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param num_blocks The number of blocks to read.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 * \param opts Optional structure with extended IO request options. If set, this structure must be
 * valid until the IO is completed.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset_blocks and/or num_blocks are out of range or separate
 *               metadata is not supported or opts_size is incorrect
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_bdev_readv_blocks_ext(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			       struct iovec *iov, int iovcnt, uint64_t offset_blocks,
			       uint64_t num_blocks, spdk_bdev_io_completion_cb cb, void *cb_arg,
			       struct spdk_bdev_ext_io_opts *opts);

/**
 * Submit a write request to the bdev on the given channel.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param buf Data buffer to written from.
 * \param offset The offset, in bytes, from the start of the block device.
 * \param nbytes The number of bytes to write. buf must be greater than or equal to this size.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset and/or nbytes are not aligned or out of range
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 *   * -EBADF - desc not open for writing
 */
int spdk_bdev_write(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		    void *buf, uint64_t offset, uint64_t nbytes,
		    spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a write request to the bdev on the given channel.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param buf Data buffer to written from.
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param num_blocks The number of blocks to write. buf must be greater than or equal to this size.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset_blocks and/or num_blocks are out of range
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 *   * -EBADF - desc not open for writing
 */
int spdk_bdev_write_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			   void *buf, uint64_t offset_blocks, uint64_t num_blocks,
			   spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a write request to the bdev on the given channel. This function uses
 * separate buffer for metadata transfer (valid only if bdev supports this
 * mode).
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param buf Data buffer to written from.
 * \param md Metadata buffer.
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param num_blocks The number of blocks to write. buf must be greater than or equal to this size.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset_blocks and/or num_blocks are out of range or separate
 *               metadata is not supported
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 *   * -EBADF - desc not open for writing
 */
int spdk_bdev_write_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				   void *buf, void *md, uint64_t offset_blocks, uint64_t num_blocks,
				   spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a write request to the bdev on the given channel. This differs from
 * spdk_bdev_write by allowing the data buffer to be described in a scatter
 * gather list. Some physical devices place memory alignment requirements on
 * data and may not be able to directly transfer out of the buffers provided. In
 * this case, the request may fail.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param iov A scatter gather list of buffers to be written from.
 * \param iovcnt The number of elements in iov.
 * \param offset The offset, in bytes, from the start of the block device.
 * \param len The size of data to write.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset and/or nbytes are not aligned or out of range
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 *   * -EBADF - desc not open for writing
 */
int spdk_bdev_writev(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		     struct iovec *iov, int iovcnt,
		     uint64_t offset, uint64_t len,
		     spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a write request to the bdev on the given channel. This differs from
 * spdk_bdev_write by allowing the data buffer to be described in a scatter
 * gather list. Some physical devices place memory alignment requirements on
 * data and may not be able to directly transfer out of the buffers provided. In
 * this case, the request may fail.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param iov A scatter gather list of buffers to be written from.
 * \param iovcnt The number of elements in iov.
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param num_blocks The number of blocks to write.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset_blocks and/or num_blocks are out of range
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 *   * -EBADF - desc not open for writing
 */
int spdk_bdev_writev_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			    struct iovec *iov, int iovcnt,
			    uint64_t offset_blocks, uint64_t num_blocks,
			    spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a write request to the bdev on the given channel. This differs from
 * spdk_bdev_write by allowing the data buffer to be described in a scatter
 * gather list. Some physical devices place memory alignment requirements on
 * data or metadata and may not be able to directly transfer out of the buffers
 * provided. In this case, the request may fail.  This function uses separate
 * buffer for metadata transfer (valid only if bdev supports this mode).
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param iov A scatter gather list of buffers to be written from.
 * \param iovcnt The number of elements in iov.
 * \param md Metadata buffer.
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param num_blocks The number of blocks to write.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset_blocks and/or num_blocks are out of range or separate
 *               metadata is not supported
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 *   * -EBADF - desc not open for writing
 */
int spdk_bdev_writev_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				    struct iovec *iov, int iovcnt, void *md,
				    uint64_t offset_blocks, uint64_t num_blocks,
				    spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a write request to the bdev on the given channel. This differs from
 * spdk_bdev_write by allowing the data buffer to be described in a scatter
 * gather list. Some physical devices place memory alignment requirements on
 * data or metadata and may not be able to directly transfer out of the buffers
 * provided. In this case, the request may fail.  This function uses separate
 * buffer for metadata transfer (valid only if bdev supports this mode).
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param iov A scatter gather list of buffers to be written from.
 * \param iovcnt The number of elements in iov.
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param num_blocks The number of blocks to write.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 * \param opts Optional structure with extended IO request options. If set, this structure must be
 * valid until the IO is completed.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset_blocks and/or num_blocks are out of range or separate
 *               metadata is not supported or opts_size is incorrect
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 *   * -EBADF - desc not open for writing
 */
int spdk_bdev_writev_blocks_ext(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				struct iovec *iov, int iovcnt, uint64_t offset_blocks,
				uint64_t num_blocks, spdk_bdev_io_completion_cb cb, void *cb_arg,
				struct spdk_bdev_ext_io_opts *opts);

/**
 * Submit a compare request to the bdev on the given channel.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param buf Data buffer to compare to.
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param num_blocks The number of blocks to compare. buf must be greater than or equal to this size.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset_blocks and/or num_blocks are out of range
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_bdev_compare_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			     void *buf, uint64_t offset_blocks, uint64_t num_blocks,
			     spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a compare request to the bdev on the given channel. This function uses
 * separate buffer for metadata transfer (valid only if bdev supports this
 * mode).
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param buf Data buffer to compare to.
 * \param md Metadata buffer.
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param num_blocks The number of blocks to compare. buf must be greater than or equal to this size.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset_blocks and/or num_blocks are out of range or separate
 *               metadata is not supported
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_bdev_compare_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				     void *buf, void *md, uint64_t offset_blocks, uint64_t num_blocks,
				     spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a compare request to the bdev on the given channel. This differs from
 * spdk_bdev_compare by allowing the data buffer to be described in a scatter
 * gather list. Some physical devices place memory alignment requirements on
 * data and may not be able to directly transfer out of the buffers provided. In
 * this case, the request may fail.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param iov A scatter gather list of buffers to be compared to.
 * \param iovcnt The number of elements in iov.
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param num_blocks The number of blocks to compare.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset_blocks and/or num_blocks are out of range
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_bdev_comparev_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			      struct iovec *iov, int iovcnt,
			      uint64_t offset_blocks, uint64_t num_blocks,
			      spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a compare request to the bdev on the given channel. This differs from
 * spdk_bdev_compare by allowing the data buffer to be described in a scatter
 * gather list. Some physical devices place memory alignment requirements on
 * data or metadata and may not be able to directly transfer out of the buffers
 * provided. In this case, the request may fail.  This function uses separate
 * buffer for metadata transfer (valid only if bdev supports this mode).
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param iov A scatter gather list of buffers to be compared to.
 * \param iovcnt The number of elements in iov.
 * \param md Metadata buffer.
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param num_blocks The number of blocks to compare.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset_blocks and/or num_blocks are out of range or separate
 *               metadata is not supported
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_bdev_comparev_blocks_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				      struct iovec *iov, int iovcnt, void *md,
				      uint64_t offset_blocks, uint64_t num_blocks,
				      spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit an atomic compare-and-write request to the bdev on the given channel.
 * For bdevs that do not natively support atomic compare-and-write, the bdev layer
 * will quiesce I/O to the specified LBA range, before performing the read,
 * compare and write operations.
 *
 * Currently this supports compare-and-write of only one block.
 *
 * The data buffers for both the compare and write operations are described in a
 * scatter gather list. Some physical devices place memory alignment requirements on
 * data and may not be able to directly transfer out of the buffers provided. In
 * this case, the request may fail.
 *
 * spdk_bdev_io_get_nvme_fused_status() function should be called in callback function
 * to get status for the individual operation.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param compare_iov A scatter gather list of buffers to be compared.
 * \param compare_iovcnt The number of elements in compare_iov.
 * \param write_iov A scatter gather list of buffers to be written if the compare is
 *                  successful.
 * \param write_iovcnt The number of elements in write_iov.
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param num_blocks The number of blocks to compare-and-write.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset_blocks and/or num_blocks are out of range
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 *   * -EBADF - desc not open for writing
 */
int spdk_bdev_comparev_and_writev_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		struct iovec *compare_iov, int compare_iovcnt,
		struct iovec *write_iov, int write_iovcnt,
		uint64_t offset_blocks, uint64_t num_blocks,
		spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a request to acquire a data buffer that represents the given
 * range of blocks. The data buffer is placed in the spdk_bdev_io structure
 * and can be obtained by calling spdk_bdev_io_get_iovec().
 *
 * \param desc Block device descriptor
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param iov A scatter gather list to be populated with the buffers
 * \param iovcnt The maximum number of elements in iov.
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param num_blocks The number of blocks.
 * \param populate Whether the data buffer should be populated with the
 *                 data at the given blocks. Populating the data buffer can
 *                 be skipped if the user writes new data to the entire buffer.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 */
int spdk_bdev_zcopy_start(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			  struct iovec *iov, int iovcnt,
			  uint64_t offset_blocks, uint64_t num_blocks,
			  bool populate,
			  spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a request to release a data buffer representing a range of blocks.
 *
 * \param bdev_io I/O request returned in the completion callback of spdk_bdev_zcopy_start().
 * \param commit Whether to commit the data in the buffers to the blocks before releasing.
 *               The data does not need to be committed if it was not modified.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 */
int spdk_bdev_zcopy_end(struct spdk_bdev_io *bdev_io, bool commit,
			spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a write zeroes request to the bdev on the given channel. This command
 *  ensures that all bytes in the specified range are set to 00h
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param offset The offset, in bytes, from the start of the block device.
 * \param len The size of data to zero.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset and/or nbytes are not aligned or out of range
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 *   * -EBADF - desc not open for writing
 */
int spdk_bdev_write_zeroes(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			   uint64_t offset, uint64_t len,
			   spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a write zeroes request to the bdev on the given channel. This command
 *  ensures that all bytes in the specified range are set to 00h
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param num_blocks The number of blocks to zero.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset_blocks and/or num_blocks are out of range
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 *   * -EBADF - desc not open for writing
 */
int spdk_bdev_write_zeroes_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				  uint64_t offset_blocks, uint64_t num_blocks,
				  spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit an unmap request to the block device. Unmap is sometimes also called trim or
 * deallocate. This notifies the device that the data in the blocks described is no
 * longer valid. Reading blocks that have been unmapped results in indeterminate data.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param offset The offset, in bytes, from the start of the block device.
 * \param nbytes The number of bytes to unmap. Must be a multiple of the block size.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset and/or nbytes are not aligned or out of range
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 *   * -EBADF - desc not open for writing
 */
int spdk_bdev_unmap(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		    uint64_t offset, uint64_t nbytes,
		    spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit an unmap request to the block device. Unmap is sometimes also called trim or
 * deallocate. This notifies the device that the data in the blocks described is no
 * longer valid. Reading blocks that have been unmapped results in indeterminate data.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param num_blocks The number of blocks to unmap.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset_blocks and/or num_blocks are out of range
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 *   * -EBADF - desc not open for writing
 */
int spdk_bdev_unmap_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			   uint64_t offset_blocks, uint64_t num_blocks,
			   spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a flush request to the bdev on the given channel. For devices with volatile
 * caches, data is not guaranteed to be persistent until the completion of a flush
 * request. Call spdk_bdev_has_write_cache() to check if the bdev has a volatile cache.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param offset The offset, in bytes, from the start of the block device.
 * \param length The number of bytes.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset and/or nbytes are not aligned or out of range
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 *   * -EBADF - desc not open for writing
 */
int spdk_bdev_flush(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		    uint64_t offset, uint64_t length,
		    spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a flush request to the bdev on the given channel. For devices with volatile
 * caches, data is not guaranteed to be persistent until the completion of a flush
 * request. Call spdk_bdev_has_write_cache() to check if the bdev has a volatile cache.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \param num_blocks The number of blocks.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -EINVAL - offset_blocks and/or num_blocks are out of range
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 *   * -EBADF - desc not open for writing
 */
int spdk_bdev_flush_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			   uint64_t offset_blocks, uint64_t num_blocks,
			   spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a reset request to the bdev on the given channel.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_bdev_reset(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		    spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit abort requests to abort all I/Os which has bio_cb_arg as its callback
 * context to the bdev on the given channel.
 *
 * This goes all the way down to the bdev driver module and attempts to abort all
 * I/Os which have bio_cb_arg as their callback context if they exist. This is a best
 * effort command. Upon completion of this, the status SPDK_BDEV_IO_STATUS_SUCCESS
 * indicates all the I/Os were successfully aborted, or the status
 * SPDK_BDEV_IO_STATUS_FAILED indicates any I/O was failed to abort for any reason
 * or no I/O which has bio_cb_arg as its callback context was found.
 *
 * \ingroup bdev_io_submit functions
 *
 * \param desc Block device descriptor.
 * \param ch The I/O channel which the I/Os to be aborted are associated with.
 * \param bio_cb_arg Callback argument for the outstanding requests which this
 * function attempts to abort.
 * \param cb Called when the abort request is completed.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always be called (even if the
 * request ultimately failed). Return negated errno on failure, in which case the
 * callback will not be called.
 *   * -EINVAL - bio_cb_arg was not specified.
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated.
 *   * -ENOTSUP - the bdev does not support abort.
 */
int spdk_bdev_abort(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		    void *bio_cb_arg,
		    spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit an NVMe Admin command to the bdev. This passes directly through
 * the block layer to the device. Support for NVMe passthru is optional,
 * indicated by calling spdk_bdev_io_type_supported().
 *
 * The SGL/PRP will be automated generated based on the given buffer,
 * so that portion of the command may be left empty.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param cmd The raw NVMe command. Must be an admin command.
 * \param buf Data buffer to written from.
 * \param nbytes The number of bytes to transfer. buf must be greater than or equal to this size.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 *   * -EBADF - desc not open for writing
 */
int spdk_bdev_nvme_admin_passthru(struct spdk_bdev_desc *desc,
				  struct spdk_io_channel *ch,
				  const struct spdk_nvme_cmd *cmd,
				  void *buf, size_t nbytes,
				  spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit an NVMe I/O command to the bdev. This passes directly through
 * the block layer to the device. Support for NVMe passthru is optional,
 * indicated by calling spdk_bdev_io_type_supported().
 *
 * \ingroup bdev_io_submit_functions
 *
 * The SGL/PRP will be automated generated based on the given buffer,
 * so that portion of the command may be left empty. Also, the namespace
 * id (nsid) will be populated automatically.
 *
 * \param bdev_desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param cmd The raw NVMe command. Must be in the NVM command set.
 * \param buf Data buffer to written from.
 * \param nbytes The number of bytes to transfer. buf must be greater than or equal to this size.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 *   * -EBADF - desc not open for writing
 */
int spdk_bdev_nvme_io_passthru(struct spdk_bdev_desc *bdev_desc,
			       struct spdk_io_channel *ch,
			       const struct spdk_nvme_cmd *cmd,
			       void *buf, size_t nbytes,
			       spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit an NVMe I/O command to the bdev. This passes directly through
 * the block layer to the device. Support for NVMe passthru is optional,
 * indicated by calling spdk_bdev_io_type_supported().
 *
 * \ingroup bdev_io_submit_functions
 *
 * The SGL/PRP will be automated generated based on the given buffer,
 * so that portion of the command may be left empty. Also, the namespace
 * id (nsid) will be populated automatically.
 *
 * \param bdev_desc Block device descriptor
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param cmd The raw NVMe command. Must be in the NVM command set.
 * \param buf Data buffer to written from.
 * \param nbytes The number of bytes to transfer. buf must be greater than or equal to this size.
 * \param md_buf Meta data buffer to written from.
 * \param md_len md_buf size to transfer. md_buf must be greater than or equal to this size.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 *   * -EBADF - desc not open for writing
 */
int spdk_bdev_nvme_io_passthru_md(struct spdk_bdev_desc *bdev_desc,
				  struct spdk_io_channel *ch,
				  const struct spdk_nvme_cmd *cmd,
				  void *buf, size_t nbytes, void *md_buf, size_t md_len,
				  spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Free an I/O request. This should only be called after the completion callback
 * for the I/O has been called and notifies the bdev layer that memory may now
 * be released.
 *
 * \param bdev_io I/O request.
 */
void spdk_bdev_free_io(struct spdk_bdev_io *bdev_io);

/**
 * Block device I/O wait callback
 *
 * Callback function to notify when an spdk_bdev_io structure is available
 * to satisfy a call to one of the @ref bdev_io_submit_functions.
 */
typedef void (*spdk_bdev_io_wait_cb)(void *cb_arg);

/**
 * Structure to register a callback when an spdk_bdev_io becomes available.
 */
struct spdk_bdev_io_wait_entry {
	struct spdk_bdev			*bdev;
	spdk_bdev_io_wait_cb			cb_fn;
	void					*cb_arg;
	TAILQ_ENTRY(spdk_bdev_io_wait_entry)	link;
};

/**
 * Add an entry into the calling thread's queue to be notified when an
 * spdk_bdev_io becomes available.
 *
 * When one of the @ref bdev_io_submit_functions returns -ENOMEM, it means
 * the spdk_bdev_io buffer pool has no available buffers. This function may
 * be called to register a callback to be notified when a buffer becomes
 * available on the calling thread.
 *
 * The callback function will always be called on the same thread as this
 * function was called.
 *
 * This function must only be called immediately after one of the
 * @ref bdev_io_submit_functions returns -ENOMEM.
 *
 * \param bdev Block device.  The block device that the caller will submit
 *             an I/O to when the callback is invoked.  Must match the bdev
 *             member in the entry parameter.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param entry Data structure allocated by the caller specifying the callback
 *              function and argument.
 *
 * \return 0 on success.
 *         -EINVAL if bdev parameter does not match bdev member in entry
 *         -EINVAL if an spdk_bdev_io structure was available on this thread.
 */
int spdk_bdev_queue_io_wait(struct spdk_bdev *bdev, struct spdk_io_channel *ch,
			    struct spdk_bdev_io_wait_entry *entry);

/**
 * Return I/O statistics for this channel.
 *
 * \param bdev Block device.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param stat The per-channel statistics.
 *
 */
void spdk_bdev_get_io_stat(struct spdk_bdev *bdev, struct spdk_io_channel *ch,
			   struct spdk_bdev_io_stat *stat);


/**
 * Return I/O statistics for this bdev. All the required information will be passed
 * via the callback function.
 *
 * \param bdev Block device to query.
 * \param stat Structure for aggregating collected statistics.  Passed as argument to cb.
 * \param cb Called when this operation completes.
 * \param cb_arg Argument passed to callback function.
 */
void spdk_bdev_get_device_stat(struct spdk_bdev *bdev, struct spdk_bdev_io_stat *stat,
			       spdk_bdev_get_device_stat_cb cb, void *cb_arg);

/**
 * Get the status of bdev_io as an NVMe status code and command specific
 * completion queue value.
 *
 * \param bdev_io I/O to get the status from.
 * \param cdw0 Command specific completion queue value
 * \param sct Status Code Type return value, as defined by the NVMe specification.
 * \param sc Status Code return value, as defined by the NVMe specification.
 */
void spdk_bdev_io_get_nvme_status(const struct spdk_bdev_io *bdev_io, uint32_t *cdw0, int *sct,
				  int *sc);

/**
 * Get the status of bdev_io as an NVMe status codes and command specific
 * completion queue value for fused operations such as compare-and-write.
 *
 * \param bdev_io I/O to get the status from.
 * \param cdw0 Command specific completion queue value
 * \param first_sct Status Code Type return value for the first operation, as defined by the NVMe specification.
 * \param first_sc Status Code return value for the first operation, as defined by the NVMe specification.
 * \param second_sct Status Code Type return value for the second operation, as defined by the NVMe specification.
 * \param second_sc Status Code return value for the second operation, as defined by the NVMe specification.
 */
void spdk_bdev_io_get_nvme_fused_status(const struct spdk_bdev_io *bdev_io, uint32_t *cdw0,
					int *first_sct, int *first_sc, int *second_sct, int *second_sc);

/**
 * Get the status of bdev_io as a SCSI status code.
 *
 * \param bdev_io I/O to get the status from.
 * \param sc SCSI Status Code.
 * \param sk SCSI Sense Key.
 * \param asc SCSI Additional Sense Code.
 * \param ascq SCSI Additional Sense Code Qualifier.
 */
void spdk_bdev_io_get_scsi_status(const struct spdk_bdev_io *bdev_io,
				  int *sc, int *sk, int *asc, int *ascq);

/**
 * Get the status of bdev_io as aio errno.
 *
 * \param bdev_io I/O to get the status from.
 * \param aio_result Negative errno returned from AIO.
 */
void spdk_bdev_io_get_aio_status(const struct spdk_bdev_io *bdev_io, int *aio_result);

/**
 * Get the iovec describing the data buffer of a bdev_io.
 *
 * \param bdev_io I/O to describe with iovec.
 * \param iovp Pointer to be filled with iovec.
 * \param iovcntp Pointer to be filled with number of iovec entries.
 */
void spdk_bdev_io_get_iovec(struct spdk_bdev_io *bdev_io, struct iovec **iovp, int *iovcntp);

/**
 * Get metadata buffer. Only makes sense if the IO uses separate buffer for
 * metadata transfer.
 *
 * \param bdev_io I/O to retrieve the buffer from.
 * \return Pointer to metadata buffer, NULL if the IO doesn't use separate
 * buffer for metadata transfer.
 */
void *spdk_bdev_io_get_md_buf(struct spdk_bdev_io *bdev_io);

/**
 * Get the callback argument of bdev_io to abort it by spdk_bdev_abort.
 *
 * \param bdev_io I/O to get the callback argument from.
 * \return Callback argument of bdev_io.
 */
void *spdk_bdev_io_get_cb_arg(struct spdk_bdev_io *bdev_io);

typedef void (*spdk_bdev_histogram_status_cb)(void *cb_arg, int status);
typedef void (*spdk_bdev_histogram_data_cb)(void *cb_arg, int status,
		struct spdk_histogram_data *histogram);

/**
 * Enable or disable collecting histogram data on a bdev.
 *
 * \param bdev Block device.
 * \param cb_fn Callback function to be called when histograms are enabled.
 * \param cb_arg Argument to pass to cb_fn.
 * \param enable Enable/disable flag
 */
void spdk_bdev_histogram_enable(struct spdk_bdev *bdev, spdk_bdev_histogram_status_cb cb_fn,
				void *cb_arg, bool enable);

/**
 * Get aggregated histogram data from a bdev. Callback provides merged histogram
 * for specified bdev.
 *
 * \param bdev Block device.
 * \param histogram Histogram for aggregated data
 * \param cb_fn Callback function to be called with data collected on bdev.
 * \param cb_arg Argument to pass to cb_fn.
 */
void spdk_bdev_histogram_get(struct spdk_bdev *bdev, struct spdk_histogram_data *histogram,
			     spdk_bdev_histogram_data_cb cb_fn,
			     void *cb_arg);

/**
 * Retrieves media events.  Can only be called from the context of
 * SPDK_BDEV_EVENT_MEDIA_MANAGEMENT event callback.  These events are sent by
 * devices exposing raw access to the physical medium (e.g. Open Channel SSD).
 *
 * \param bdev_desc Block device descriptor
 * \param events Array of media management event descriptors
 * \param max_events Size of the events array
 *
 * \return number of events retrieved
 */
size_t spdk_bdev_get_media_events(struct spdk_bdev_desc *bdev_desc,
				  struct spdk_bdev_media_event *events, size_t max_events);

/**
 * Get SPDK memory domains used by the given bdev. If bdev reports that it uses memory domains
 * that means that it can work with data buffers located in those memory domains.
 *
 * The user can call this function with \b domains set to NULL and \b array_size set to 0 to get the
 * number of memory domains used by bdev
 *
 * \param bdev Block device
 * \param domains Pointer to an array of memory domains to be filled by this function. The user should allocate big enough
 * array to keep all memory domains used by bdev and all underlying bdevs
 * \param array_size size of \b domains array
 * \return the number of entries in \b domains array or negated errno. If returned value is bigger than \b array_size passed by the user
 * then the user should increase the size of \b domains array and call this function again. There is no guarantees that
 * the content of \b domains array is valid in that case.
 *         -EINVAL if input parameters were invalid
 */
int spdk_bdev_get_memory_domains(struct spdk_bdev *bdev, struct spdk_memory_domain **domains,
				 int array_size);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_BDEV_H_ */
