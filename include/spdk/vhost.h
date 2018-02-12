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

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*spdk_vhost_fini_cb)(void);

int spdk_vhost_init(void);
void spdk_vhost_fini(spdk_vhost_fini_cb fini_cb);

/**
 * Init vhost application.  This is called once by SPDK app layer.
 * \param arg1 optional path to directory where sockets will
 * be created
 * \param arg2 unused
 */
void spdk_vhost_startup(void *arg1, void *arg2);

/**
 * Deinit vhost application.  This is called once by SPDK app layer.
 */
void spdk_vhost_shutdown_cb(void);

/**
 * SPDK vhost device (vdev).  An equivalent of Virtio device.
 * Both virtio-blk and virtio-scsi devices are represented by this
 * struct. For virtio-scsi a single vhost device (also called SCSI
 * controller) may contain multiple SCSI targets (devices), each of
 * which may contain multiple logical units (SCSI LUNs). For now
 * only one LUN per target is available.
 *
 * All vdev-changing functions operate directly on this object.
 * Note that \c spdk_vhost_dev cannot be acquired. This object is
 * only accessible as a callback parameter via \c
 * spdk_vhost_call_external_event and it's derivatives. This ensures
 * that all access to the vdev is piped through a single,
 * thread-safe API.
 */
struct spdk_vhost_dev;

/**
 * Synchronized vhost event used for user callbacks.
 *
 * \param vdev vhost device
 * \param arg user-provided parameter
 * \return 0 on success, -1 on failure
 */
typedef int (*spdk_vhost_event_fn)(struct spdk_vhost_dev *vdev, void *arg);

/**
 * Get name of the vhost device.  This is equal to the filename
 * of socket file. The name is constant throughout the lifetime of
 * a vdev.
 *
 * \param vdev vhost device
 * \return name of the vdev
 */
const char *spdk_vhost_dev_get_name(struct spdk_vhost_dev *vdev);

/**
 * Get cpuset of the vhost device.  The cpuset is constant
 * throughout the lifetime of a vdev. It is be a subset
 * of SPDK app cpuset vhost was started with.
 *
 * \param dev vhost device
 * \return cpuset of the vdev.
 */
const struct spdk_cpuset *spdk_vhost_dev_get_cpumask(struct spdk_vhost_dev *vdev);

/**
 * By default, events are generated when asked, but for high queue depth and
 * high IOPS this prove to be inefficient both for guest kernel that have to
 * handle a lot more IO completions and for SPDK vhost that need to make more
 * syscalls. If enabled, limit amount of events (IRQs) sent to initiator by SPDK
 * vhost effectively coalescing couple of completions. This of cource introduce
 * IO latency penalty proportional to event delay time.
 *
 * Actual events delay time when is calculated according to below formula:
 * if (delay_base == 0 || IOPS < iops_threshold) {
 *   delay = 0;
 * } else if (IOPS < iops_threshold) {
 *   delay = delay_base * (iops - iops_threshold) / iops_threshold;
 * }
 *
 * \param vdev vhost device
 * \param delay_base_us Base delay time in microseconds. If 0, coalescing is disabled.
 * \param iops_threshold IOPS threshold when coalescing is activated
 */
int spdk_vhost_set_coalescing(struct spdk_vhost_dev *vdev, uint32_t delay_base_us,
			      uint32_t iops_threshold);

/**
 * Construct an empty vhost SCSI device.  This will create a
 * Unix domain socket together with a vhost-user slave server waiting
 * for a connection on this socket. Creating the vdev does not
 * start any I/O pollers and does not hog the CPU. I/O processing
 * starts after receiving proper message on the created socket.
 * See QEMU's vhost-user documentation for details.
 * All physical devices have to be separately attached to this
 * vdev via \c spdk_vhost_scsi_dev_add_tgt().
 *
 * This function is thread-safe.
 *
 * \param name name of the vhost device. The name will also be used
 * for socket name, which is exactly \c socket_base_dir/name
 * \param mask string containing cpumask in hex. The leading *0x*
 * is allowed but not required. The mask itself can be constructed as:
 * ((1 << cpu0) | (1 << cpu1) | ... | (1 << cpuN)).
 * \return 0 on success, negative errno on error.
 */
int spdk_vhost_scsi_dev_construct(const char *name, const char *cpumask);

/**
 * Construct and attach new SCSI target to the vhost SCSI device
 * on given (unoccupied) slot.  The device will be created with a single
 * LUN0 associated with given SPDK bdev. Currently only one LUN per
 * device is supported.
 *
 * If vhost SCSI device has an active socket connection, it is
 * required that it has negotiated \c VIRTIO_SCSI_F_HOTPLUG feature
 * flag. Otherwise an -ENOTSUP error code is returned.
 *
 * \param vdev vhost SCSI device
 * \param scsi_tgt_num slot to attach to
 * \param bdev_name name of the SPDK bdev to associate with SCSI LUN0
 * \return 0 on success, negative errno on error.
 */
int spdk_vhost_scsi_dev_add_tgt(struct spdk_vhost_dev *vdev, unsigned scsi_tgt_num,
				const char *bdev_name);

/**
 * Get SCSI target from vhost SCSI device on given slot. Max
 * number of available slots is defined by
 * \c SPDK_VHOST_SCSI_CTRLR_MAX_DEVS.
 *
 * \param vdev vhost SCSI device
 * \param num slot id
 * \return SCSI device on given slot or NULL
 */
struct spdk_scsi_dev *spdk_vhost_scsi_dev_get_tgt(struct spdk_vhost_dev *vdev, uint8_t num);

/**
 * Detach and destruct SCSI target from a vhost SCSI device.
 *
 * If vhost SCSI device has an active socket connection, it is
 * required that it has negotiated \c VIRTIO_SCSI_F_HOTPLUG feature
 * flag.Otherwise an -ENOTSUP error code is returned. If the flag has
 * been negotiated, the device will be marked to be deleted. Actual
 * deletion is deferred until after all pending I/O to this device
 * has finished.
 *
 * Once the target has been deleted (whether or not vhost SCSI
 * device is in use) given callback will be called.
 *
 * \param vdev vhost SCSI device
 * \param scsi_tgt_num slot id to delete target from
 * \param cb_fn callback to be fired once target has been successfully
 * deleted. The first parameter of callback function is the vhost SCSI
 * device, the second is user provided argument *cb_arg*.
 * \param cb_arg parameter to be passed to *cb_fn*.
 * \return 0 on success, negative errno on error.
 */
int spdk_vhost_scsi_dev_remove_tgt(struct spdk_vhost_dev *vdev, unsigned scsi_tgt_num,
				   spdk_vhost_event_fn cb_fn, void *cb_arg);

/**
 * Construct a vhost blk device.  This will create a Unix domain
 * socket together with a vhost-user slave server waiting for a
 * connection on this socket. Creating the vdev does not start
 * any I/O pollers and does not hog the CPU. I/O processing starts
 * after receiving proper message on the created socket.
 * See QEMU's vhost-user documentation for details. Vhost blk
 * device is tightly associated with given SPDK bdev. Given
 * bdev can not be changed, unless it has been hotremoved. This
 * would result in all I/O failing with virtio \c VIRTIO_BLK_S_IOERR
 * error code.
 *
 * This function is thread-safe.
 *
 * \param name name of the vhost blk device. The name will also be
 * used for socket name, which is exactly \c socket_base_dir/name
 * \param mask string containing cpumask in hex. The leading *0x*
 * is allowed but not required. The mask itself can be constructed as:
 * ((1 << cpu0) | (1 << cpu1) | ... | (1 << cpuN)).
 * \param dev_name bdev name to associate with this vhost device
 * \param readonly if set, all writes to the device will fail with
 * \c VIRTIO_BLK_S_IOERR error code.
 * \return 0 on success, negative errno on error.
 */
int spdk_vhost_blk_construct(const char *name, const char *cpumask, const char *dev_name,
			     bool readonly);

/**
 * Remove a vhost device. The device must not have any open
 * connections on it's socket.
 *
 * \param vdev vhost blk device
 * \return 0 on success, negative errno on error.
 */
int spdk_vhost_dev_remove(struct spdk_vhost_dev *vdev);

/**
 * Get underlying SPDK bdev from vhost blk device.  The
 * bdev might be NULL, as it could have been hotremoved.
 *
 * \param ctrl vhost blk device
 * \return SPDK bdev associated with given vdev
 */
struct spdk_bdev *spdk_vhost_blk_get_dev(struct spdk_vhost_dev *ctrlr);

/**
 * Call function on reactor of given vhost device.  If
 * device is not in use, the event will be called
 * right away on the caller's thread.
 *
 * This function is thread safe.
 *
 * \param vdev_name name of the vhost device to run
 * this event on
 * \param fn function to be called. The first parameter
 * of callback function is either actual spdk_vhost_dev
 * pointer or NULL in case vdev with given name doesn't
 * exist. The second param is user provided argument *arg*.
 * \param arg parameter to be passed to *fn*.
 */
void spdk_vhost_call_external_event(const char *vdev_name, spdk_vhost_event_fn fn, void *arg);

/**
 * Call function for each available vhost device on
 * it's reactor.  This will call given function in a chain,
 * meaning that each callback will be called after the
 * previous one has finished. After given function has
 * been called for all vdevs, it will be called once
 * again with first param - vhost device- set to NULL.
 *
 * This function is thread safe.
 *
 * \param fn function to be called for each vdev.
 * The first param will be either vdev pointer or NULL.
 * The second param is user provided argument *arg*.
 * \param arg parameter to be passed to *fn*.
 */
void spdk_vhost_call_external_event_foreach(spdk_vhost_event_fn fn, void *arg);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_VHOST_H */
