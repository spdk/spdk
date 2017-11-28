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
 * Virtio-SCSI device.  This structure describes a SCSI controller used by
 * the bdev_virtio module.
 */
struct spdk_virtio_scsi_dev;

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
 * Note that this function does not create any bdevs. See \c bdev_virtio_scsi_dev_scan.
 *
 * \param name name for the virtio device. It will be later inherited by all
 * bdevs exposed by this virtio device.
 * \param path path to the socket
 * \param num_queues max number of request virtqueues (I/O queues) to use.
 * If given value exceeds a hard limit of the physical (host) device, this
 * call will return with error.
 * \param queue_size depth of all queues
 * \return virtio device or NULL
 */
struct virtio_dev *bdev_virtio_scsi_dev_create(const char *name, const char *path,
		unsigned num_queues, unsigned queue_size);

/**
 * Scan a Virtio SCSI device.  This might multiple Virtio SCSI bdevs - one
 * for each detected LUN on each target.
 *
 * \param vdev virtio device
 * \param cb_fn function to be called after scanning all targets on the
 * virtio device. Optional, can be NULL. See \c bdev_virtio_create_cb.
 * \param cb_arg argument for the `cb_fn`
 * \return 0 on success (device scan is started) or negative error code.
 * In case of error the \c cb_fn is not called.
 */
int bdev_virtio_scsi_dev_scan(struct virtio_dev *vdev, bdev_virtio_create_cb cb_fn,
			      void *cb_arg);

/**
 * Remove a Virtio SCSI device.  The device must not have been scanned yet.
 * This function must be called from the same thread that created the device.
 *
 * \param vdev virtio device
 */
void bdev_virtio_scsi_dev_remove(struct virtio_dev *vdev);

#endif /* SPDK_BDEV_VIRTIO_H */
