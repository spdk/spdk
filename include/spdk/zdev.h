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
 * Zone bdev public interface
 */

#ifndef SPDK_ZDEV_H
#define SPDK_ZDEV_H

#include "spdk/stdinc.h"

#include "spdk/bdev_module.h"

/**
 * Structure describing zoned bdev properties
 */
struct spdk_zdev_info {
	/** Default size of each zone */
	size_t		zone_size;

	/** Maximum number of open zones  */
	size_t		max_open_zones;

	/** Optimal number of open zones */
	size_t		optimal_open_zones;
};

struct spdk_zdev {
	struct spdk_bdev bdev;

	struct spdk_zdev_info info;
};

enum spdk_zdev_zone_state {
	SPDK_ZDEV_ZONE_STATE_EMPTY,
	SPDK_ZDEV_ZONE_STATE_OPEN,
	SPDK_ZDEV_ZONE_STATE_FULL,
	SPDK_ZDEV_ZONE_STATE_CLOSED,
	SPDK_ZDEV_ZONE_STATE_READ_ONLY,
	SPDK_ZDEV_ZONE_STATE_OFFLINE
};

enum spdk_zdev_zone_action {
	SPDK_ZDEV_ZONE_CLOSE,
	SPDK_ZDEV_ZONE_FINISH,
	SPDK_ZDEV_ZONE_OPEN,
	SPDK_ZDEV_ZONE_RESET
};

struct spdk_zdev_zone_info {
	uint64_t			start_lba;
	uint64_t			write_pointer;
	uint64_t			capacity;
	enum spdk_zdev_zone_state	state;
};

static inline struct spdk_zdev *
spdk_zdev_from_bdev(struct spdk_bdev *bdev)
{
	if (!spdk_bdev_is_zdev(bdev)) {
		return NULL;
	}

	return SPDK_CONTAINEROF(bdev, struct spdk_zdev, bdev);
}

/**
 * Get zone info of the device.
 *
 * \param zdev Zone device to query.
 * \return const pointer to spdk_zdev_info structure.
 */
const struct spdk_zdev_info *spdk_zdev_get_info(const struct spdk_zdev *zdev);

/**
 * Submit a get_zone_info request to the bdev.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param start_lba First logical block of a zone.
 * \param num_zones Number of consecutive zones info to retrive.
 * \param info Pointer to array capable to store num_zones elements.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_zdev_get_zone_info(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			    uint64_t start_lba, size_t num_zones, struct spdk_zdev_zone_info *info,
			    spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a zone_open request to the bdev.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param start_lba First logical block of a zone.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_zdev_zone_open(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			uint64_t start_lba, spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a zone_finish request to the bdev.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param start_lba First logical block of a zone.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_zdev_zone_finish(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			  uint64_t start_lba, spdk_bdev_io_completion_cb cb, void *cb_arg);

/**
 * Submit a zone_close request to the bdev.
 *
 * \ingroup bdev_io_submit_functions
 *
 * \param desc Block device descriptor.
 * \param ch I/O channel. Obtained by calling spdk_bdev_get_io_channel().
 * \param start_lba First logical block of a zone.
 * \param cb Called when the request is complete.
 * \param cb_arg Argument passed to cb.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *   * -ENOMEM - spdk_bdev_io buffer cannot be allocated
 */
int spdk_zdev_zone_close(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			 uint64_t start_lba, spdk_bdev_io_completion_cb cb, void *cb_arg);


#endif /* SPDK_ZDEV_H */
