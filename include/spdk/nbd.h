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
struct spdk_event;

/**
 * Initialize the network block device layer.
 *
 * \return 0 on success.
 */
int spdk_nbd_init(void);

/**
 * Stop and close all the running network block devices.
 */
void spdk_nbd_fini(void);

/**
 * Start a network block device backed by the bdev.
 *
 * \param bdev_name Name of bdev exposed as a network block device.
 * \param nbd_path Path to the registered network block device.
 *
 * \return a pointer to the configuration of the registered network block device
 * on success, or NULL on failure.
 */
struct spdk_nbd_disk *spdk_nbd_start(const char *bdev_name, const char *nbd_path);

/**
 * Stop the running network block device safely.
 *
 * \param nbd A pointer to the network block device to stop.
 */
void spdk_nbd_stop(struct spdk_nbd_disk *nbd);

/**
 * Write NBD subsystem configuration into provided JSON context.
 *
 * \param w JSON write context
 * \param done_ev call this event when done.
 */
void spdk_nbd_write_config_json(struct spdk_json_write_ctx *w, struct spdk_event *done_ev);

#ifdef __cplusplus
}
#endif

#endif
