/*-
 *   BSD LICENSE
 *
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
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

/** \file
 * Block device abstraction layer
 */

#ifndef SPDK_BDEV_H_
#define SPDK_BDEV_H_

#include "spdk/stdinc.h"

#include "spdk/scsi_spec.h"
#include "spdk/nvme_spec.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SPDK_BDEV_SMALL_BUF_MAX_SIZE 8192
#define SPDK_BDEV_LARGE_BUF_MAX_SIZE (64 * 1024)

typedef void (*spdk_bdev_remove_cb_t)(void *remove_ctx);

/**
 * Block device I/O
 *
 * This is an I/O that is passed to an spdk_bdev.
 */
struct spdk_bdev_io;

struct spdk_bdev_fn_table;
struct spdk_io_channel;
struct spdk_json_write_ctx;

/** bdev status */
enum spdk_bdev_status {
	SPDK_BDEV_STATUS_INVALID,
	SPDK_BDEV_STATUS_READY,
	SPDK_BDEV_STATUS_REMOVING,
};

/**
 * \brief SPDK block device.
 *
 * This is a virtual representation of a block device that is exported by the backend.
 */
struct spdk_bdev;

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
};

/**
 * Block device completion callback.
 *
 * \param bdev_io Block device I/O that has completed.
 * \param success true if I/O completed successfully or false if it failed; additional error
 *                information may be retrieved from bdev_io by calling
 *                spdk_bdev_io_get_nvme_status() or spdk_bdev_io_get_scsi_status().
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
	uint64_t read_latency_ticks;
	uint64_t write_latency_ticks;
	uint64_t ticks_rate;
};

typedef void (*spdk_bdev_init_cb)(void *cb_arg, int rc);
typedef void (*spdk_bdev_fini_cb)(void *cb_arg);

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
 * Get the configuration options for the registered block device modules.
 *
 * \param fp The pointer to a file that will be written to the configuration options.
 */
void spdk_bdev_config_text(FILE *fp);

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
 * \param bdev Block device to open.
 * \param write true is read/write access requested, false if read-only
 * \param remove_cb callback function for hot remove the device.
 * \param remove_ctx param for hot removal callback function.
 * \param desc output parameter for the descriptor when operation is successful
 * \return 0 if operation is successful, suitable errno value otherwise
 */
int spdk_bdev_open(struct spdk_bdev *bdev, bool write, spdk_bdev_remove_cb_t remove_cb,
		   void *remove_ctx, struct spdk_bdev_desc **desc);

/**
 * Close a previously opened block device.
 *
 * \param desc Block device descriptor to close.
 */
void spdk_bdev_close(struct spdk_bdev_desc *desc);

/**
 * Get the bdev associated with a bdev descriptor.
 *
 * \param desc Open block device desciptor
 * \return bdev associated with the descriptor
 */
struct spdk_bdev *spdk_bdev_desc_get_bdev(struct spdk_bdev_desc *desc);

/**
 * Check whether the block device supports the I/O type.
 *
 * \param bdev Block device to check.
 * \param io_type The specific I/O type like read, write, flush, unmap.
 * \return true if support, false otherwise.
 */
bool spdk_bdev_io_type_supported(struct spdk_bdev *bdev, enum spdk_bdev_io_type io_type);

/**
 * Output driver-specific configuration to a JSON stream.
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
 * Get size of block device in logical blocks.
 *
 * \param bdev Block device to query.
 * \return Size of bdev in logical blocks.
 *
 * Logical blocks are numbered from 0 to spdk_bdev_get_num_blocks(bdev) - 1, inclusive.
 */
uint64_t spdk_bdev_get_num_blocks(const struct spdk_bdev *bdev);

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
 * Submit a read request to the bdev on the given channel.
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
 */
int spdk_bdev_read(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		   void *buf, uint64_t offset, uint64_t nbytes,
		   spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a read request to the bdev on the given channel.
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
 */
int spdk_bdev_read_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			  void *buf, uint64_t offset_blocks, uint64_t num_blocks,
			  spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a read request to the bdev on the given channel. This differs from
 * spdk_bdev_read by allowing the data buffer to be described in a scatter
 * gather list. Some physical devices place memory alignment requirements on
 * data and may not be able to directly transfer into the buffers provided. In
 * this case, the request may fail.
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
 */
int spdk_bdev_readv_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			   struct iovec *iov, int iovcnt,
			   uint64_t offset_blocks, uint64_t num_blocks,
			   spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a write request to the bdev on the given channel.
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
 */
int spdk_bdev_write(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		    void *buf, uint64_t offset, uint64_t nbytes,
		    spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a write request to the bdev on the given channel.
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
 */
int spdk_bdev_write_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			   void *buf, uint64_t offset_blocks, uint64_t num_blocks,
			   spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a write request to the bdev on the given channel. This differs from
 * spdk_bdev_write by allowing the data buffer to be described in a scatter
 * gather list. Some physical devices place memory alignment requirements on
 * data and may not be able to directly transfer out of the buffers provided. In
 * this case, the request may fail.
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
 */
int spdk_bdev_writev_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			    struct iovec *iov, int iovcnt,
			    uint64_t offset_blocks, uint64_t num_blocks,
			    spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a write zeroes request to the bdev on the given channel. This command
 *  ensures that all bytes in the specified range are set to 00h
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
 */
int spdk_bdev_write_zeroes(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			   uint64_t offset, uint64_t len,
			   spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a write zeroes request to the bdev on the given channel. This command
 *  ensures that all bytes in the specified range are set to 00h
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
 */
int spdk_bdev_write_zeroes_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				  uint64_t offset_blocks, uint64_t num_blocks,
				  spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit an unmap request to the block device. Unmap is sometimes also called trim or
 * deallocate. This notifies the device that the data in the blocks described is no
 * longer valid. Reading blocks that have been unmapped results in indeterminate data.
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
 */
int spdk_bdev_unmap(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		    uint64_t offset, uint64_t nbytes,
		    spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit an unmap request to the block device. Unmap is sometimes also called trim or
 * deallocate. This notifies the device that the data in the blocks described is no
 * longer valid. Reading blocks that have been unmapped results in indeterminate data.
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
 */
int spdk_bdev_unmap_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			   uint64_t offset_blocks, uint64_t num_blocks,
			   spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a flush request to the bdev on the given channel. For devices with volatile
 * caches, data is not guaranteed to be persistent until the completion of a flush
 * request. Call spdk_bdev_has_write_cache() to check if the bdev has a volatile cache.
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
 */
int spdk_bdev_flush(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		    uint64_t offset, uint64_t length,
		    spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a flush request to the bdev on the given channel. For devices with volatile
 * caches, data is not guaranteed to be persistent until the completion of a flush
 * request. Call spdk_bdev_has_write_cache() to check if the bdev has a volatile cache.
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
 */
int spdk_bdev_flush_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			   uint64_t offset_blocks, uint64_t num_blocks,
			   spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a reset request to the bdev on the given channel.
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 */
int spdk_bdev_reset(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		    spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit an NVMe Admin command to the bdev. This passes directly through
 * the block layer to the device. Support for NVMe passthru is optional,
 * indicated by calling spdk_bdev_io_type_supported().
 *
 * The SGL/PRP will be automated generated based on the given buffer,
 * so that portion of the command may be left empty.
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
 * The SGL/PRP will be automated generated based on the given buffer,
 * so that portion of the command may be left empty. Also, the namespace
 * id (nsid) will be populated automatically.
 *
 * \param bdev Block device
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
 */
int spdk_bdev_nvme_io_passthru_md(struct spdk_bdev_desc *bdev_desc,
				  struct spdk_io_channel *ch,
				  const struct spdk_nvme_cmd *cmd,
				  void *buf, size_t nbytes, void *md_buf, size_t md_len,
				  spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Free an I/O request. This should be called after the callback for the I/O has
 * been called and notifies the bdev layer that memory may now be released.
 *
 * \param bdev_io I/O request.
 *
 * \return -1 on failure, 0 on success.
 */
int spdk_bdev_free_io(struct spdk_bdev_io *bdev_io);

/**
 * Return I/O statistics for this channel. After returning stats, zero out
 * the current state of the statistics.
 *
 * \param bdev Block device.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param stat The per-channel statistics.
 *
 */
void spdk_bdev_get_io_stat(struct spdk_bdev *bdev, struct spdk_io_channel *ch,
			   struct spdk_bdev_io_stat *stat);

/**
 * Get the status of bdev_io as an NVMe status code.
 *
 * \param bdev_io I/O to get the status from.
 * \param sct Status Code Type return value, as defined by the NVMe specification.
 * \param sc Status Code return value, as defined by the NVMe specification.
 */
void spdk_bdev_io_get_nvme_status(const struct spdk_bdev_io *bdev_io, int *sct, int *sc);

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
 * Get the iovec describing the data buffer of a bdev_io.
 *
 * \param bdev_io I/O to describe with iovec.
 * \param iovp Pointer to be filled with iovec.
 * \param iovcntp Pointer to be filled with number of iovec entries.
 */
void spdk_bdev_io_get_iovec(struct spdk_bdev_io *bdev_io, struct iovec **iovp, int *iovcntp);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_BDEV_H_ */
