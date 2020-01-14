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
 * Zoned device public interface
 */

#ifndef SPDK_BDEV_ZONE_H
#define SPDK_BDEV_ZONE_H

#include "spdk/stdinc.h"
#include "spdk/bdev.h"

/**
 * \brief SPDK block device.
 *
 * This is a virtual representation of a block device that is exported by the backend.
 */

struct spdk_bdev;

enum spdk_bdev_zone_action {
	SPDK_BDEV_ZONE_CLOSE,
	SPDK_BDEV_ZONE_FINISH,
	SPDK_BDEV_ZONE_OPEN,
	SPDK_BDEV_ZONE_RESET
};

enum spdk_bdev_zone_state {
	SPDK_BDEV_ZONE_STATE_EMPTY,
	SPDK_BDEV_ZONE_STATE_OPEN,
	SPDK_BDEV_ZONE_STATE_FULL,
	SPDK_BDEV_ZONE_STATE_CLOSED,
	SPDK_BDEV_ZONE_STATE_READ_ONLY,
	SPDK_BDEV_ZONE_STATE_OFFLINE
};

struct spdk_bdev_zone_info {
	uint64_t			zone_id;
	uint64_t			write_pointer;
	uint64_t			capacity;
	enum spdk_bdev_zone_state	state;
};

/**
 * Get device zone size in logical blocks.
 *
 * \param bdev Block device to query.
 * \return Size of zone for this zoned device in logical blocks.
 */
uint64_t spdk_bdev_get_zone_size(const struct spdk_bdev *bdev);

/**
 * Get device maximum number of open zones.
 *
 * If this value is 0, there is no limit.
 *
 * \param bdev Block device to query.
 * \return Maximum number of open zones for this zoned device.
 */
uint32_t spdk_bdev_get_max_open_zones(const struct spdk_bdev *bdev);

/**
 * Get device optimal number of open zones.
 *
 * \param bdev Block device to query.
 * \return Optimal number of open zones for this zoned device.
 */
uint32_t spdk_bdev_get_optimal_open_zones(const struct spdk_bdev *bdev);

/**
 * Submit a get_zone_info request to the bdev.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param zone_id First logical block of a zone.
 * \param num_zones Number of consecutive zones info to retrieve.
 * \param info Pointer to array capable of storing num_zones elements.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_bdev_get_zone_info(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			    uint64_t zone_id, size_t num_zones, struct spdk_bdev_zone_info *info,
			    spdk_bdev_io_completion_cb cb, void *cb_arg);


/**
 * Submit a zone_management request to the bdev.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param zone_id First logical block of a zone.
 * \param action Action to perform on a zone (open, close, reset, finish).
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_bdev_zone_management(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			      uint64_t zone_id, enum spdk_bdev_zone_action action,
			      spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a zone_append request to the bdev.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param buf Data buffer to written from.
 * \param zone_id First logical block of a zone.
 * \param num_blocks The number of blocks to write. buf must be greater than or equal to this size.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed).
 * Appended logical block address can be obtained with spdk_bdev_io_get_append_location().
 * Return negated errno on failure, in which case the callback will not be called.
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_bdev_zone_append(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			  void *buf, uint64_t zone_id, uint64_t num_blocks,
			  spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a zone_append request to the bdev. This differs from
 * spdk_bdev_zone_append by allowing the data buffer to be described in a scatter
 * gather list.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param iov A scatter gather list of buffers to be written from.
 * \param iovcnt The number of elements in iov.
 * \param zone_id First logical block of a zone.
 * \param num_blocks The number of blocks to write. buf must be greater than or equal to this size.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed).
 * Appended logical block address can be obtained with spdk_bdev_io_get_append_location().
 * Return negated errno on failure, in which case the callback will not be called.
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_bdev_zone_appendv(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			   struct iovec *iov, int iovcnt, uint64_t zone_id, uint64_t num_blocks,
			   spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a zone_append request with metadata to the bdev.
 *
 * This function uses separate buffer for metadata transfer (valid only if bdev supports this
 * mode).
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param buf Data buffer to written from.
 * \param md Metadata buffer.
 * \param zone_id First logical block of a zone.
 * \param num_blocks The number of blocks to write. buf must be greater than or equal to this size.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed).
 * Appended logical block address can be obtained with spdk_bdev_io_get_append_location().
 * Return negated errno on failure, in which case the callback will not be called.
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_bdev_zone_append_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				  void *buf, void *md, uint64_t zone_id, uint64_t num_blocks,
				  spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a zone_append request with metadata to the bdev. This differs from
 * spdk_bdev_zone_append by allowing the data buffer to be described in a scatter
 * gather list.
 *
 * This function uses separate buffer for metadata transfer (valid only if bdev supports this
 * mode).
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param iov A scatter gather list of buffers to be written from.
 * \param iovcnt The number of elements in iov.
 * \param md Metadata buffer.
 * \param zone_id First logical block of a zone.
 * \param num_blocks The number of blocks to write. buf must be greater than or equal to this size.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed).
 * Appended logical block address can be obtained with spdk_bdev_io_get_append_location().
 * Return negated errno on failure, in which case the callback will not be called.
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_bdev_zone_appendv_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				   struct iovec *iov, int iovcnt, void *md, uint64_t zone_id,
				   uint64_t num_blocks, spdk_bdev_io_completion_cb cb,
				   void *cb_arg);

/**
 * Get append location (offset in blocks of the bdev) for this I/O.
 *
 * \param bdev_io I/O to get append location from.
 */
uint64_t spdk_bdev_io_get_append_location(struct spdk_bdev_io *bdev_io);

#endif /* SPDK_BDEV_ZONE_H */
