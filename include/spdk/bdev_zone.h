/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

/** \file
 * Zoned device public interface
 */

#ifndef SPDK_BDEV_ZONE_H
#define SPDK_BDEV_ZONE_H

#include "spdk/stdinc.h"
#include "spdk/bdev.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \brief SPDK block device.
 *
 * This is a virtual representation of a block device that is exported by the backend.
 */

struct spdk_bdev;

enum spdk_bdev_zone_type {
	SPDK_BDEV_ZONE_TYPE_CNV		= 0x1,
	SPDK_BDEV_ZONE_TYPE_SEQWR	= 0x2,
	SPDK_BDEV_ZONE_TYPE_SEQWP	= 0x3,
};

enum spdk_bdev_zone_action {
	SPDK_BDEV_ZONE_CLOSE,
	SPDK_BDEV_ZONE_FINISH,
	SPDK_BDEV_ZONE_OPEN,
	SPDK_BDEV_ZONE_RESET,
	SPDK_BDEV_ZONE_OFFLINE,
};

enum spdk_bdev_zone_state {
	SPDK_BDEV_ZONE_STATE_EMPTY	= 0x0,
	SPDK_BDEV_ZONE_STATE_IMP_OPEN	= 0x1,
	/* OPEN is an alias for IMP_OPEN. OPEN is kept for backwards compatibility. */
	SPDK_BDEV_ZONE_STATE_OPEN	= SPDK_BDEV_ZONE_STATE_IMP_OPEN,
	SPDK_BDEV_ZONE_STATE_FULL	= 0x2,
	SPDK_BDEV_ZONE_STATE_CLOSED	= 0x3,
	SPDK_BDEV_ZONE_STATE_READ_ONLY	= 0x4,
	SPDK_BDEV_ZONE_STATE_OFFLINE	= 0x5,
	SPDK_BDEV_ZONE_STATE_EXP_OPEN	= 0x6,
	SPDK_BDEV_ZONE_STATE_NOT_WP	= 0x7,
};

struct spdk_bdev_zone_info {
	uint64_t			zone_id;
	uint64_t			write_pointer;
	uint64_t			capacity;
	enum spdk_bdev_zone_state	state;
	enum spdk_bdev_zone_type	type;
};

/**
 * Get device zone size in logical blocks.
 *
 * \param bdev Block device to query.
 * \return Size of zone for this zoned device in logical blocks.
 */
uint64_t spdk_bdev_get_zone_size(const struct spdk_bdev *bdev);

/**
 * Get the number of zones for the given device.
 *
 * \param bdev Block device to query.
 * \return The number of zones.
 */
uint64_t spdk_bdev_get_num_zones(const struct spdk_bdev *bdev);

/**
 * Get the first logical block of a zone (known as zone_id or zslba)
 * for a given offset.
 *
 * \param bdev Block device to query.
 * \param offset_blocks The offset, in blocks, from the start of the block device.
 * \return The zone_id (also known as zslba) for the given offset.
 */
uint64_t spdk_bdev_get_zone_id(const struct spdk_bdev *bdev, uint64_t offset_blocks);

/**
 * Get device maximum zone append data transfer size in logical blocks.
 *
 * If this value is 0, there is no limit.
 *
 * \param bdev Block device to query.
 * \return Maximum zone append data transfer size for this zoned device in logical blocks.
 */
uint32_t spdk_bdev_get_max_zone_append_size(const struct spdk_bdev *bdev);

/**
 * Get device maximum number of open zones.
 *
 * An open zone is defined as a zone being in zone state
 * SPDK_BDEV_ZONE_STATE_IMP_OPEN or SPDK_BDEV_ZONE_STATE_EXP_OPEN.
 *
 * If this value is 0, there is no limit.
 *
 * \param bdev Block device to query.
 * \return Maximum number of open zones for this zoned device.
 */
uint32_t spdk_bdev_get_max_open_zones(const struct spdk_bdev *bdev);

/**
 * Get device maximum number of active zones.
 *
 * An active zone is defined as a zone being in zone state
 * SPDK_BDEV_ZONE_STATE_IMP_OPEN, SPDK_BDEV_ZONE_STATE_EXP_OPEN or
 * SPDK_BDEV_ZONE_STATE_CLOSED.
 *
 * If this value is 0, there is no limit.
 *
 * \param bdev Block device to query.
 * \return Maximum number of active zones for this zoned device.
 */
uint32_t spdk_bdev_get_max_active_zones(const struct spdk_bdev *bdev);

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
 * \param action Action to perform on a zone (open, close, reset, finish, offline).
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

#ifdef __cplusplus
}
#endif

#endif /* SPDK_BDEV_ZONE_H */
