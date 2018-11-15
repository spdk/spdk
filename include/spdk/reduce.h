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
	 * Size in bytes of a chunk on the compressed volume.  This
	 *  is the unit in which data is compressed.  Must be an even
	 *  multiple of backing_io_unit_size.  Must be greater than 0.
	 */
	uint32_t		chunk_size;

	/**
	 * Total size in bytes of the compressed volume.  Must be
	 *  an even multiple of chunk_size.  Must be greater than 0.
	 */
	uint64_t		vol_size;
};

/**
 * Get the required size for the pm file for a compressed volume.
 *
 * \param params Parameters for the compressed volume
 * \return Size of the required pm file (in bytes) needed to create the
 *         compressed volume.  Returns -EINVAL if params is invalid.
 */
int64_t spdk_reduce_get_pm_file_size(struct spdk_reduce_vol_params *params);

/**
 * Get the required size for the backing device for a compressed volume.
 *
 * \param params Parameters for the compressed volume
 * \return Size of the required backing device (in bytes) needed to create
 *         the compressed volume.  Returns -EINVAL if params is invalid.
 */
int64_t spdk_reduce_get_backing_device_size(struct spdk_reduce_vol_params *params);

struct spdk_reduce_vol;

typedef void (*spdk_reduce_vol_op_complete)(void *ctx, int ziperrno);
typedef void (*spdk_reduce_vol_op_with_handle_complete)(void *ctx,
		struct spdk_reduce_vol *vol,
		int ziperrno);

typedef void (*spdk_reduce_dev_cpl)(void *cb_arg, int ziperrno);

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

	void (*close)(struct spdk_reduce_backing_dev *dev);

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

#endif /* SPDK_REDUCE_H_ */
