/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

/** \file
 * Network block device layer
 */

#ifndef SPDK_NBD_H_
#define SPDK_NBD_H_

#ifdef __cplusplus
extern "C" {
#endif

struct spdk_bdev;
struct spdk_nbd_disk;
struct spdk_json_write_ctx;
typedef void (*spdk_nbd_fini_cb)(void *arg);

/**
 * Initialize the network block device layer.
 *
 * \return 0 on success.
 */
int spdk_nbd_init(void);

/**
 * Stop and close all the running network block devices.
 *
 * \param cb_fn Callback to be always called.
 * \param cb_arg Passed to cb_fn.
 */
void spdk_nbd_fini(spdk_nbd_fini_cb cb_fn, void *cb_arg);

/**
 * Called when an NBD device has been started.
 * On success, rc is assigned 0; On failure, rc is assigned negated errno.
 */
typedef void (*spdk_nbd_start_cb)(void *cb_arg, struct spdk_nbd_disk *nbd,
				  int rc);

/**
 * Start a network block device backed by the bdev.
 *
 * \param bdev_name Name of bdev exposed as a network block device.
 * \param nbd_path Path to the registered network block device.
 * \param cb_fn Callback to be always called.
 * \param cb_arg Passed to cb_fn.
 */
void spdk_nbd_start(const char *bdev_name, const char *nbd_path,
		    spdk_nbd_start_cb cb_fn, void *cb_arg);

/**
 * Stop the running network block device safely.
 *
 * \param nbd A pointer to the network block device to stop.
 *
 * \return 0 on success.
 */
int spdk_nbd_stop(struct spdk_nbd_disk *nbd);

/**
 * Get the local filesystem path used for the network block device.
 */
const char *spdk_nbd_get_path(struct spdk_nbd_disk *nbd);

/**
 * Write NBD subsystem configuration into provided JSON context.
 *
 * \param w JSON write context
 */
void spdk_nbd_write_config_json(struct spdk_json_write_ctx *w);

#ifdef __cplusplus
}
#endif

#endif
