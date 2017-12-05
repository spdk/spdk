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

#ifndef SPDK_BDEV_VIRTIO_H
#define SPDK_BDEV_VIRTIO_H

#include "spdk/bdev.h"

/**
 * Callback for creating virtio bdevs.
 *
 * \param ctx opaque context set by the user
 * \param errnum error code. 0 on success, negative errno on error.
 * \param bdevs contiguous array of created bdevs
 * \param bdev_cnt number of bdevs in the `bdevs` array
 */
typedef void (*bdev_virtio_create_cb)(void *ctx, int errnum,
				      struct spdk_bdev **bdevs, size_t bdev_cnt);

/**
 * Connect to a vhost-user Unix domain socket and create a Virtio SCSI device.
 * If the connection is successful, the device will be automatically scanned.
 * The scan consists of probing the targets on the device and will result in
 * creating possibly multiple Virtio SCSI bdevs - one for each target. Currently
 * only one LUN per target is detected - LUN0. Note that the bdev creation is
 * run asynchronously in the background. After it's finished, the `cb_fn`
 * callback is called.
 *
 * \param name name for the virtio device. It will be inherited by all created
 * bdevs, which are named in the following format: <name>t<target_id>
 * \param path path to the socket
 * \param num_queues max number of request virtqueues (I/O queues) to use.
 * If given value exceeds a hard limit of the physical (host) device, this
 * function will return with error.
 * \param queue_size depth of each queue
 * \param cb_fn function to be called after scanning all targets on the virtio
 * device. It's optional, can be NULL. See \c bdev_virtio_create_cb.
 * \param cb_arg argument for the `cb_fn`
 * \return zero on success (device scan is started) or negative error code.
 * In case of error the \c cb_fn is not called.
 */
int bdev_virtio_scsi_dev_create(const char *name, const char *path,
				unsigned num_queues, unsigned queue_size,
				bdev_virtio_create_cb cb_fn, void *cb_arg);

#endif /* SPDK_BDEV_VIRTIO_H */
