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
 *         compressed volume.  Returns -1 if params is invalid.
 */
int64_t spdk_reduce_get_pm_file_size(struct spdk_reduce_vol_params *params);

/**
 * Get the required size for the backing device for a compressed volume.
 *
 * \param params Parameters for the compressed volume
 * \return Size of the required backing device (in bytes) needed to create
 *         the compressed volume.  Returns -1 if params is invalid.
 */
int64_t spdk_reduce_get_backing_device_size(struct spdk_reduce_vol_params *params);

#endif /* SPDK_REDUCE_H_ */
