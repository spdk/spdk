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

struct spdk_bs_dev;
struct spdk_bdev;
struct spdk_bdev_module;

/**
 * Create a blobstore block device from a bdev.
 *
 * \param bdev_name Name of the bdev to use.
 * \param event_cb Called when the bdev triggers asynchronous event.
 * \param event_ctx Argument passed to function event_cb.
 * \param bs_dev Output parameter for a pointer to the blobstore block device.
 *
 * \return 0 if operation is successful, or suitable errno value otherwise.
 */
int spdk_bdev_create_bs_dev_ext(const char *bdev_name, spdk_bdev_event_cb_t event_cb,
				void *event_ctx, struct spdk_bs_dev **bs_dev);

/**
 * Claim the bdev module for the given blobstore.
 *
 * \param bs_dev Blobstore block device.
 * \param module Bdev module to claim.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_bs_bdev_claim(struct spdk_bs_dev *bs_dev, struct spdk_bdev_module *module);

#ifdef __cplusplus
}
#endif

#endif
