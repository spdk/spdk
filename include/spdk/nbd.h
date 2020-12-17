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
