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
 * Helper library to use spdk_bdev as the backing device for a blobstore
 */

#ifndef SPDK_BLOB_BDEV_H
#define SPDK_BLOB_BDEV_H

#include "spdk/stdinc.h"
#include "spdk/bdev.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Blobstore device */
struct spdk_bs_dev;
/** Block device */
struct spdk_bdev;
/** Block device module */
struct spdk_bdev_module_if;

/**
 * Create a blobstore device for the given block device.
 *
 * \param bdev Block device.
 * \param remove_cb Callback function for hot removal the device.
 * \param remove_ctx Param for hot removal callback function.
 * \return The created blobstore device on success or NULL otherwise.
 */
struct spdk_bs_dev *spdk_bdev_create_bs_dev(struct spdk_bdev *bdev, spdk_bdev_remove_cb_t remove_cb,
		void *remove_ctx);

/**
 * Claim the block device module for the given blobstore device.
 *
 * \param bs_dev Blobstore device.
 * \param module Block device module to claim.
 * \return 0 on success, negated errno on failure.
 */
int spdk_bs_bdev_claim(struct spdk_bs_dev *bs_dev, struct spdk_bdev_module_if *module);

#ifdef __cplusplus
}
#endif

#endif
