/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
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
