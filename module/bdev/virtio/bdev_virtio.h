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
#include "spdk/env.h"

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
 * Callback for removing virtio devices.
 *
 * \param ctx opaque context set by the user
 * \param errnum error code. 0 on success, negative errno on error.
 */
typedef void (*bdev_virtio_remove_cb)(void *ctx, int errnum);

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
 * \param num_queues max number of request virtqueues to use. `vdev` will be
 * started successfully even if the host device supports less queues than requested.
 * \param queue_size depth of each queue
 * \param cb_fn function to be called after scanning all targets on the virtio
 * device. It's optional, can be NULL. See \c bdev_virtio_create_cb.
 * \param cb_arg argument for the `cb_fn`
 * \return zero on success (device scan is started) or negative error code.
 * In case of error the \c cb_fn is not called.
 */
int bdev_virtio_user_scsi_dev_create(const char *name, const char *path,
				     unsigned num_queues, unsigned queue_size,
				     bdev_virtio_create_cb cb_fn, void *cb_arg);

/**
 * Attach virtio-pci device. This creates a Virtio SCSI device with the same
 * capabilities as the vhost-user equivalent. The device will be automatically
 * scanned for exposed SCSI targets. This will result in creating possibly multiple
 * Virtio SCSI bdevs - one for each target. Currently only one LUN per target is
 * detected - LUN0. Note that the bdev creation is run asynchronously in the
 * background. After it's finished, the `cb_fn` callback is called.
 *
 * \param name name for the virtio device. It will be inherited by all created
 * bdevs, which are named in the following format: <name>t<target_id>
 * \param pci_addr PCI address of the device to attach
 * \param cb_fn function to be called after scanning all targets on the virtio
 * device. It's optional, can be NULL. See \c bdev_virtio_create_cb.
 * \param cb_arg argument for the `cb_fn`
 * \return zero on success (device scan is started) or negative error code.
 * In case of error the \c cb_fn is not called.
 */
int bdev_virtio_pci_scsi_dev_create(const char *name, struct spdk_pci_addr *pci_addr,
				    bdev_virtio_create_cb cb_fn, void *cb_arg);

/**
 * Remove a Virtio device with given name. This will destroy all bdevs exposed
 * by this device.
 *
 * \param name virtio device name
 * \param cb_fn function to be called after scanning all targets on the virtio
 * device. It's optional, can be NULL. See \c bdev_virtio_create_cb. Possible
 * error codes are:
 *  * ENODEV - couldn't find device with given name
 *  * EBUSY - device is already being removed
 * \param cb_arg argument for the `cb_fn`
 * \return zero on success or -ENODEV if scsi dev does not exist
 */
int bdev_virtio_scsi_dev_remove(const char *name,
				bdev_virtio_remove_cb cb_fn, void *cb_arg);

/**
 * Remove a Virtio device with given name.
 *
 * \param bdev virtio blk device bdev
 * \param cb_fn function to be called after removing bdev
 * \param cb_arg argument for the `cb_fn`
 * \return zero on success, -ENODEV if bdev with 'name' does not exist or
 * -EINVAL if bdev with 'name' is not a virtio blk device.
 */
int bdev_virtio_blk_dev_remove(const char *name,
			       bdev_virtio_remove_cb cb_fn, void *cb_arg);

/**
 * List all created Virtio-SCSI devices.
 *
 * \param write_ctx JSON context to write into
 */
void bdev_virtio_scsi_dev_list(struct spdk_json_write_ctx *write_ctx);

/**
 * Connect to a vhost-user Unix domain socket and create a Virtio BLK bdev.
 *
 * \param name name for the virtio bdev
 * \param path path to the socket
 * \param num_queues max number of request virtqueues to use. `vdev` will be
 * started successfully even if the host device supports less queues than requested.
 * \param queue_size depth of each queue
 * \return virtio-blk bdev or NULL
 */
struct spdk_bdev *bdev_virtio_user_blk_dev_create(const char *name, const char *path,
		unsigned num_queues, unsigned queue_size);

/**
 * Attach virtio-pci device. This creates a Virtio BLK device with the same
 * capabilities as the vhost-user equivalent.
 *
 * \param name name for the virtio device. It will be inherited by all created
 * bdevs, which are named in the following format: <name>t<target_id>
 * \param pci_addr PCI address of the device to attach
 * \return virtio-blk bdev or NULL
 */
struct spdk_bdev *bdev_virtio_pci_blk_dev_create(const char *name,
		struct spdk_pci_addr *pci_addr);

/**
 * Enable/Disable the virtio blk hotplug monitor or
 * change the monitor period time
 *
 * \param enabled True means to enable the hotplug monitor and the monitor
 * period time is period_us. False means to disable the hotplug monitor
 * \param period_us The period time of the hotplug monitor in us
 * \return 0 for success otherwise failure
 */
int
bdev_virtio_pci_blk_set_hotplug(bool enabled, uint64_t period_us);

#endif /* SPDK_BDEV_VIRTIO_H */
