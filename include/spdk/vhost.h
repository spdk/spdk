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

/**
 *  \file
 *  SPDK vhost
 */

#ifndef SPDK_VHOST_H
#define SPDK_VHOST_H

#include "spdk/stdinc.h"

#include "spdk/event.h"

int spdk_vhost_init(void);
int spdk_vhost_fini(void);

/**
 * Init vhost application. This is called once by SPDK app layer.
 * \param arg1 optional path to directory where sockets will
 * be created
 * \param arg2 unused
 */
void spdk_vhost_startup(void *arg1, void *arg2);

/**
 * Deinit vhost application. This is called once by SPDK app layer.
 */
void spdk_vhost_shutdown_cb(void);

/**
 * SPDK vhost controller.
 *
 * All controller-changing functions operate directly on this object.
 * Note that \c spdk_vhost_dev cannot be accquired. This object is
 * only accessible as a callback parameter via \c
 * spdk_vhost_call_external_event and it's derivatives. This ensures
 * that all access to the controller is piped through a single,
 * thread-safe API.
 */
struct spdk_vhost_dev;

/**
 * Synchronized vhost event used for user callbacks.
 *
 * \param vdev vhost controller
 * \param arg user-provided parameter
 * \return 0 on success, -1 on failure
 */
typedef int (*spdk_vhost_event_fn)(struct spdk_vhost_dev *vdev, void *arg);

/**
 * Get name of the vhost controller. This is equal to the filename
 * of socket file. The name is constant throughout the lifetime of
 * a controller.
 *
 * \param ctrlr vhost controller
 * \return name of the controller
 */
const char *spdk_vhost_dev_get_name(struct spdk_vhost_dev *ctrl);

/**
 * Get cpumask of the vhost controller. The mask is constant
 * throughout the lifetime of a controller. It must be a subset
 * of SPDK cpumask vhost was started with.
 *
 * \param ctrlr vhost controller
 * \return cpumask of the controller. This is a number greater or
 * equal than 0 and lesser than number of logical CPU cores.
 */
uint64_t spdk_vhost_dev_get_cpumask(struct spdk_vhost_dev *ctrl);

/**
 * Try to convert cpumask string into valid mask number. Given mask
 * must be a subset of SPDK cpumask vhost was started with.
 *
 * \param mask string containing cpumask in either hex or base 10
 * to be parsed. The format will be detected automatically. In case
 * of hex format the leading *0x* is allowed but not required.
 * \param cpumask pointer where parsed mask will be put. In case
 * this function returns -1, the value under this pointer is
 * undefined.
 * \return 0 on success, -1 on failure.
 */
int spdk_vhost_parse_core_mask(const char *mask, uint64_t *cpumask);

/**
 * Construct an empty vhost SCSI controller. This will create a
 * Unix domain socket together with a vhost-user slave server waiting
 * for a connection on this socket. Creating the controller does not
 * start any I/O pollers and does not hog the CPU. I/O processing
 * starts after receiving proper message on the created socket.
 * All physical devices have to be separately attached to this
 * controller via \c spdk_vhost_scsi_dev_add_dev().
 *
 * This function is thread-safe.
 *
 * \param name name of the controller. The name will also be used
 * for socket name, which is exactly \c socket_base_dir/name
 * \param mask string containing cpumask in either hex or base 10
 * to be parsed. The format will be detected automatically. In case
 * of hex the leading *0x* is allowed but not required.
 * \return 0 on success, negative errno on error.
 */
int spdk_vhost_scsi_dev_construct(const char *name, uint64_t cpumask);

/**
 * Remove an empty vhost SCSI controller. The controller must not
 * have any SCSI devices attached nor have any open connection on
 * it's socket.
 *
 * \param vdev vhost SCSI controller
 * \return 0 on success, negative errno on error.
 */
int spdk_vhost_scsi_dev_remove(struct spdk_vhost_dev *vdev);

/**
 * Construct and attach new SCSI device to the vhost SCSI controller
 * on given (unoccupied) slot. The device will be created with a single
 * LUN0 associated with given SPDK bdev. Currently only one LUN per
 * device is supported.
 *
 * If vhost SCSI controller has an active socket connection, it is
 * required that it has negotiated \c VIRTIO_SCSI_F_HOTPLUG feature
 * flag. Otherwise an -ENOTSUP error code is returned.
 *
 * \param vdev vhost SCSI controller
 * \param scsi_dev_num slot to attach to
 * \param lun_name name of the SPDK bdev to associate with SCSI LUN0
 * \return 0 on success, negative errno on error.
 */
int spdk_vhost_scsi_dev_add_dev(struct spdk_vhost_dev *vdev, unsigned scsi_dev_num,
				const char *lun_name);

/**
 * Get SCSI device from vhost SCSI controller on given slot. Max
 * number of available slots is defined by
 * \c SPDK_VHOST_SCSI_CTRLR_MAX_DEVS.
 *
 * \param ctrl vhost SCSI controller
 * \param num slot id
 * \return SCSI device on given slot or NULL
 */
struct spdk_scsi_dev *spdk_vhost_scsi_dev_get_dev(struct spdk_vhost_dev *ctrl,
		uint8_t num);

/**
 * Detach and destruct SCSI device from vhost SCSI controller.
 *
 * If vhost SCSI controller has an active socket connection, it is
 * required that it has negotiated \c VIRTIO_SCSI_F_HOTPLUG feature
 * flag.Otherwise an -ENOTSUP error code is returned. If the flag has
 * been negotiated, the device will be marked to be deleted. Actual
 * deletion is deferred until after all pending I/O to this device
 * has finished.
 *
 * Once the device has been deleted (whether or not vhost SCSI
 * controller is in use) given callback will be called.
 *
 * \param vdev vhost SCSI controller
 * \param scsi_dev_num slot id to delete device from
 * \param cb_fn callback to be fired once device has been successfully
 * deleted. The first parameter of callback function is the vhost SCSI
 * controller, the second is user provided argument *cb_arg*.
 * \param cb_arg parameter to be passed to *cb_fn*.
 * \return 0 on success, negative errno on error.
 */
int spdk_vhost_scsi_dev_remove_dev(struct spdk_vhost_dev *vdev, unsigned scsi_dev_num,
				   spdk_vhost_event_fn cb_fn, void *cb_arg);

/**
 * Construct a vhost blk controller. This will create a Unix domain
 * socket together with a vhost-user slave server waiting for a
 * connection on this socket. Creating the controller does not start
 * any I/O pollers and does not hog the CPU. I/O processing starts
 * after receiving proper message on the created socket. Vhost blk
 * controller is tightly associated with given SPDK bdev. Given
 * bdev can not be changed, unless it has been hotremoved. This
 * would result in all I/O failing with virtio \c VIRTIO_BLK_S_IOERR
 * error code.
 *
 * This function is thread-safe.
 *
 * \param name name of the controller. The name will also be used
 * for socket name, which is exactly \c socket_base_dir/name
 * \param mask string containing cpumask in either hex or base 10
 * to be parsed. The format will be detected automatically. In case
 * of hex the leading *0x* is allowed but not required.
 * \return 0 on success, negative errno on error.
 */
int spdk_vhost_blk_construct(const char *name, uint64_t cpumask, const char *dev_name,
			     bool readonly);

/**
 * Remove a vhost blk controller. The controller must not have any
 * open connection on it's socket.
 *
 * \param vdev vhost blk controller
 * \return 0 on success, negative errno on error.
 */
int spdk_vhost_blk_destroy(struct spdk_vhost_dev *dev);

/**
 * Get underlying SPDK bdev from vhost blk controller.
 * The bdev might be NULL, as it could have been hotremoved.
 *
 * \param ctrl vhost blk controller
 * \return SPDK bdev associated with given controller
 */
struct spdk_bdev *spdk_vhost_blk_get_dev(struct spdk_vhost_dev *ctrlr);

/**
 * Call function on reactor of given vhost controller.
 * If controller is not in use, the event will be called
 * right away on the caller's thread.
 *
 * This function is thread safe.
 *
 * \param ctrlr_name name of the vhost controller to run
 * this event on
 * \param fn function to be called. The first parameter
 * of callback function is either actual spdk_vhost_dev
 * pointer or NULL in case vdev with given name doesn't
 * exist. The second param is user provided argument *arg*.
 * \param arg parameter to be passed to *fn*
 */
void spdk_vhost_call_external_event(const char *ctrlr_name, spdk_vhost_event_fn fn, void *arg);

/**
 * Call function for each available vhost controller on
 * it's reactor. This will call given function in a chain,
 * meaning that each callback will be called after the
 * previous one has finished. After given function has
 * been called for all controllers, it will be called
 * once again with first param - vhost controller - set
 * to NULL.
 *
 * This function is thread safe.
 *
 * \param fn function to be called
 * \param arg parameter to be passed to *fn*
 */
void spdk_vhost_call_external_event_foreach(spdk_vhost_event_fn fn, void *arg);

#endif /* SPDK_VHOST_H */
