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

/** \file
 * SPDK block compression
 */

#ifndef SPDK_REDUCE_H_
#define SPDK_REDUCE_H_

#include "spdk/uuid.h"

#define REDUCE_MAX_IOVECS	17

/**
 * Describes the parameters of an spdk_reduce_vol.
 */
struct spdk_reduce_vol_params {
	struct spdk_uuid	uuid;

	/**
	 * Size in bytes of the IO unit for the backing device.  This
	 *  is the unit in which space is allocated from the backing
	 *  device, and the unit in which data is read from of written
	 *  to the backing device.  Must be greater than 0.
	 */
	uint32_t		backing_io_unit_size;

	/**
	 * Size in bytes of a logical block.  This is the unit in
	 *  which users read or write data to the compressed volume.
	 *  Must be greater than 0.
	 */
	uint32_t		logical_block_size;

	/**
	 * Size in bytes of a chunk on the compressed volume.  This
	 *  is the unit in which data is compressed.  Must be an even
	 *  multiple of backing_io_unit_size and logical_block_size.
	 *  Must be greater than 0.
	 */
	uint32_t		chunk_size;

	/**
	 * Total size in bytes of the compressed volume.  During
	 *  initialization, the size is calculated from the size of
	 *  backing device size, so this must be set to 0 in the
	 *  structure passed to spdk_reduce_vol_init().  After
	 *  initialization, or a successful load, this field will
	 *  contain the total size which will be an even multiple
	 *  of the chunk size.
	 */
	uint64_t		vol_size;
};

struct spdk_reduce_vol;

typedef void (*spdk_reduce_vol_op_complete)(void *ctx, int reduce_errno);
typedef void (*spdk_reduce_vol_op_with_handle_complete)(void *ctx,
		struct spdk_reduce_vol *vol,
		int reduce_errno);

/**
 * Defines function type for callback functions called when backing_dev
 *  operations are complete.
 *
 * \param cb_arg Callback argument
 * \param reduce_errno Completion status of backing_dev operation
 *		       Negative values indicate negated errno value
 *		       0 indicates successful readv/writev/unmap operation
 *		       Positive value indicates successful compress/decompress
 *		       operations; number indicates number of bytes written to
 *		       destination iovs
 */
typedef void (*spdk_reduce_dev_cpl)(void *cb_arg, int reduce_errno);

struct spdk_reduce_vol_cb_args {
	spdk_reduce_dev_cpl	cb_fn;
	void			*cb_arg;
};

struct spdk_reduce_backing_dev {
	void (*readv)(struct spdk_reduce_backing_dev *dev, struct iovec *iov, int iovcnt,
		      uint64_t lba, uint32_t lba_count, struct spdk_reduce_vol_cb_args *args);

	void (*writev)(struct spdk_reduce_backing_dev *dev, struct iovec *iov, int iovcnt,
		       uint64_t lba, uint32_t lba_count, struct spdk_reduce_vol_cb_args *args);

	void (*unmap)(struct spdk_reduce_backing_dev *dev,
		      uint64_t lba, uint32_t lba_count, struct spdk_reduce_vol_cb_args *args);

	void (*compress)(struct spdk_reduce_backing_dev *dev,
			 struct iovec *src_iov, int src_iovcnt,
			 struct iovec *dst_iov, int dst_iovcnt,
			 struct spdk_reduce_vol_cb_args *args);

	void (*decompress)(struct spdk_reduce_backing_dev *dev,
			   struct iovec *src_iov, int src_iovcnt,
			   struct iovec *dst_iov, int dst_iovcnt,
			   struct spdk_reduce_vol_cb_args *args);

	uint64_t	blockcnt;
	uint32_t	blocklen;
};

/**
 * Get the UUID for a libreduce compressed volume.
 *
 * \param vol Previously loaded or initialized compressed volume.
 * \return UUID for the compressed volume.
 */
const struct spdk_uuid *spdk_reduce_vol_get_uuid(struct spdk_reduce_vol *vol);

/**
 * Initialize a new libreduce compressed volume.
 *
 * \param params Parameters for the new volume.
 * \param backing_dev Structure describing the backing device to use for the new volume.
 * \param pm_file_dir Directory to use for creation of the persistent memory file to
 *                    use for the new volume.  This function will append the UUID as
 *		      the filename to create in this directory.
 * \param cb_fn Callback function to signal completion of the initialization process.
 * \param cb_arg Argument to pass to the callback function.
 */
void spdk_reduce_vol_init(struct spdk_reduce_vol_params *params,
			  struct spdk_reduce_backing_dev *backing_dev,
			  const char *pm_file_dir,
			  spdk_reduce_vol_op_with_handle_complete cb_fn,
			  void *cb_arg);

/**
 * Load an existing libreduce compressed volume.
 *
 * \param backing_dev Structure describing the backing device containing the compressed volume.
 * \param cb_fn Callback function to signal completion of the loading process.
 * \param cb_arg Argument to pass to the callback function.
 */
void spdk_reduce_vol_load(struct spdk_reduce_backing_dev *backing_dev,
			  spdk_reduce_vol_op_with_handle_complete cb_fn,
			  void *cb_arg);

/**
 * Unload a previously initialized or loaded libreduce compressed volume.
 *
 * \param vol Volume to unload.
 * \param cb_fn Callback function to signal completion of the unload process.
 * \param cb_arg Argument to pass to the callback function.
 */
void spdk_reduce_vol_unload(struct spdk_reduce_vol *vol,
			    spdk_reduce_vol_op_complete cb_fn,
			    void *cb_arg);

/**
 * Destroy an existing libreduce compressed volume.
 *
 * This will zero the metadata region on the backing device and delete the associated
 * pm metadata file.  If the backing device does not contain a compressed volume, the
 * cb_fn will be called with error status without modifying the backing device nor
 * deleting a pm file.
 *
 * \param backing_dev Structure describing the backing device containing the compressed volume.
 * \param cb_fn Callback function to signal completion of the destruction process.
 * \param cb_arg Argument to pass to the callback function.
 */
void spdk_reduce_vol_destroy(struct spdk_reduce_backing_dev *backing_dev,
			     spdk_reduce_vol_op_complete cb_fn,
			     void *cb_arg);

/**
 * Read data from a libreduce compressed volume.
 *
 * This function will only read from logical blocks on the comparessed volume that
 * fall within the same chunk.
 *
 * \param vol Volume to read data.
 * \param iov iovec array describing the data to be read
 * \param iovcnt Number of elements in the iovec array
 * \param offset Offset (in logical blocks) to read the data on the compressed volume
 * \param length Length (in logical blocks) of the data to read
 * \param cb_fn Callback function to signal completion of the readv operation.
 * \param cb_arg Argument to pass to the callback function.
 */
void spdk_reduce_vol_readv(struct spdk_reduce_vol *vol,
			   struct iovec *iov, int iovcnt, uint64_t offset, uint64_t length,
			   spdk_reduce_vol_op_complete cb_fn, void *cb_arg);

/**
 * Write data to a libreduce compressed volume.
 *
 * This function will only write to logical blocks on the comparessed volume that
 * fall within the same chunk.
 *
 * \param vol Volume to write data.
 * \param iov iovec array describing the data to be written
 * \param iovcnt Number of elements in the iovec array
 * \param offset Offset (in logical blocks) to write the data on the compressed volume
 * \param length Length (in logical blocks) of the data to write
 * \param cb_fn Callback function to signal completion of the writev operation.
 * \param cb_arg Argument to pass to the callback function.
 */
void spdk_reduce_vol_writev(struct spdk_reduce_vol *vol,
			    struct iovec *iov, int iovcnt, uint64_t offset, uint64_t length,
			    spdk_reduce_vol_op_complete cb_fn, void *cb_arg);

/**
 * Get the params structure for a libreduce compressed volume.
 *
 * This function will populate the given params structure for a given volume.
 *
 * \param vol Previously loaded or initialized compressed volume.
 * \return params structure for the compressed volume.
 */
const struct spdk_reduce_vol_params *spdk_reduce_vol_get_params(struct spdk_reduce_vol *vol);

/**
 * Dump out key information for a libreduce compressed volume and its PMEM.
 *
 * This function will print key information for a given volume its PMEM.
 *
 * \param vol Previously loaded or initialized compressed volume.
 */
void spdk_reduce_vol_print_info(struct spdk_reduce_vol *vol);
#endif /* SPDK_REDUCE_H_ */
