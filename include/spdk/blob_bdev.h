/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

struct spdk_bdev_bs_dev_opts {
	/**
	 * The size of this structure according to the caller of this library is used for ABI
	 * compatibility. The library uses this field to know how many fields in this structure are
	 * valid. And the library will populate any remaining fields with default values.
	 * New added fields should be put at the end of the struct.
	 */
	size_t opts_size;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_bdev_bs_dev_opts) == 8, "Incorrect size");

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
 * Create a blobstore block device from a bdev.
 *
 * \param bdev_name The bdev to use.
 * \param write If true, open device read-write, else open read-only.
 * \param opts Additonal options; none currently supported.
 * \param opts_size Size of structure referenced by opts.
 * \param event_cb Called when the bdev triggers asynchronous event.
 * \param event_ctx Argument passed to function event_cb.
 * \param bs_dev Output parameter for a pointer to the blobstore block device.
 * \return 0 if operation is successful, or suitable errno value otherwise.
 */
int spdk_bdev_create_bs_dev(const char *bdev_name, bool write,
			    struct spdk_bdev_bs_dev_opts *opts, size_t opts_size,
			    spdk_bdev_event_cb_t event_cb, void *event_ctx,
			    struct spdk_bs_dev **bs_dev);

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
