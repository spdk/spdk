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

#include "spdk/cpuset.h"
#include "spdk/json.h"
#include "spdk/thread.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Callback for spdk_vhost_init().
 *
 * \param rc 0 on success, negative errno on failure
 */
typedef void (*spdk_vhost_init_cb)(int rc);

/** Callback for spdk_vhost_fini(). */
typedef void (*spdk_vhost_fini_cb)(void);

/**
 * Set the path to the directory where vhost sockets will be created.
 *
 * This function must be called before spdk_vhost_init().
 *
 * \param basename Path to vhost socket directory
 *
 * \return 0 on success, negative errno on error.
 */
int spdk_vhost_set_socket_path(const char *basename);

/**
 * Init vhost environment.
 *
 * \param init_cb Function to be called when the initialization is complete.
 */
void spdk_vhost_init(spdk_vhost_init_cb init_cb);

/**
 * Clean up the environment of vhost.
 *
 * \param fini_cb Function to be called when the cleanup is complete.
 */
void spdk_vhost_fini(spdk_vhost_fini_cb fini_cb);


/**
 * Write vhost subsystem configuration into provided JSON context.
 *
 * \param w JSON write context
 */
void spdk_vhost_config_json(struct spdk_json_write_ctx *w);

/**
 * Deinit vhost application. This is called once by SPDK app layer.
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
 * Lock the global vhost mutex synchronizing all the vhost device accesses.
 */
void spdk_vhost_lock(void);

/**
 * Lock the global vhost mutex synchronizing all the vhost device accesses.
 *
 * \return 0 if the mutex could be locked immediately, negative errno otherwise.
 */
int spdk_vhost_trylock(void);

/**
 * Unlock the global vhost mutex.
 */
void spdk_vhost_unlock(void);

/**
 * Find a vhost device by name.
 *
 * \return vhost device or NULL
 */
struct spdk_vhost_dev *spdk_vhost_dev_find(const char *name);

/**
 * Get the next vhost device. If there's no more devices to iterate
 * through, NULL will be returned.
 *
 * \param vdev vhost device. If NULL, this function will return the
 * very first device.
 * \return vdev vhost device or NULL
 */
struct spdk_vhost_dev *spdk_vhost_dev_next(struct spdk_vhost_dev *vdev);

/**
 * Synchronized vhost event used for user callbacks.
 *
 * \param vdev vhost device.
 * \param arg user-provided parameter.
 *
 * \return 0 on success, -1 on failure.
 */
typedef int (*spdk_vhost_event_fn)(struct spdk_vhost_dev *vdev, void *arg);

/**
 * Get the name of the vhost device.  This is equal to the filename
 * of socket file. The name is constant throughout the lifetime of
 * a vdev.
 *
 * \param vdev vhost device.
 *
 * \return name of the vdev.
 */
const char *spdk_vhost_dev_get_name(struct spdk_vhost_dev *vdev);

/**
 * Get cpuset of the vhost device.  The cpuset is constant throughout the lifetime
 * of a vdev. It is a subset of SPDK app cpuset vhost was started with.
 *
 * \param vdev vhost device.
 *
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
 * \param vdev vhost device.
 * \param delay_base_us Base delay time in microseconds. If 0, coalescing is disabled.
 * \param iops_threshold IOPS threshold when coalescing is activated.
 */
int spdk_vhost_set_coalescing(struct spdk_vhost_dev *vdev, uint32_t delay_base_us,
			      uint32_t iops_threshold);

/**
 * Get coalescing parameters.
 *
 * \see spdk_vhost_set_coalescing
 *
 * \param vdev vhost device.
 * \param delay_base_us Optional pointer to store base delay time.
 * \param iops_threshold Optional pointer to store IOPS threshold.
 */
void spdk_vhost_get_coalescing(struct spdk_vhost_dev *vdev, uint32_t *delay_base_us,
			       uint32_t *iops_threshold);

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
 * \param cpumask string containing cpumask in hex. The leading *0x*
 * is allowed but not required. The mask itself can be constructed as:
 * ((1 << cpu0) | (1 << cpu1) | ... | (1 << cpuN)).
 *
 * \return 0 on success, negative errno on error.
 */
int spdk_vhost_scsi_dev_construct(const char *name, const char *cpumask);

/**
 * Construct and attach new SCSI target to the vhost SCSI device
 * on given (unoccupied) slot.  The device will be created with a single
 * LUN0 associated with given SPDK bdev. Currently only one LUN per
 * device is supported.
 *
 * If the vhost SCSI device has an active connection and has negotiated
 * \c VIRTIO_SCSI_F_HOTPLUG feature,  the new SCSI target should be
 * automatically detected by the other side.
 *
 * \param vdev vhost SCSI device.
 * \param scsi_tgt_num slot to attach to or negative value to use first free.
 * \param bdev_name name of the SPDK bdev to associate with SCSI LUN0.
 *
 * \return value >= 0 on success - the SCSI target ID, negative errno code:
 * -EINVAL - one of the arguments is invalid:
 *   - vdev is not vhost SCSI device
 *   - SCSI target ID is out of range
 *   - bdev name is NULL
 *   - can't create SCSI LUN because of other errors e.g.: bdev does not exist
 * -ENOSPC - scsi_tgt_num is -1 and maximum targets in vhost SCSI device reached
 * -EEXIST - SCSI target ID already exists
 */
int spdk_vhost_scsi_dev_add_tgt(struct spdk_vhost_dev *vdev, int scsi_tgt_num,
				const char *bdev_name);

/**
 * Get SCSI target from vhost SCSI device on given slot. Max
 * number of available slots is defined by.
 * \c SPDK_VHOST_SCSI_CTRLR_MAX_DEVS.
 *
 * \param vdev vhost SCSI device.
 * \param num slot id.
 *
 * \return SCSI device on given slot or NULL.
 */
struct spdk_scsi_dev *spdk_vhost_scsi_dev_get_tgt(struct spdk_vhost_dev *vdev, uint8_t num);

/**
 * Detach and destruct SCSI target from a vhost SCSI device.
 *
 * The device will be deleted after all pending I/O is finished.
 * If the driver supports VIRTIO_SCSI_F_HOTPLUG, then a hotremove
 * notification will be sent.
 *
 * \param vdev vhost SCSI device
 * \param scsi_tgt_num slot id to delete target from
 * \param cb_fn callback to be fired once target has been successfully
 * deleted. The first parameter of callback function is the vhost SCSI
 * device, the second is user provided argument *cb_arg*.
 * \param cb_arg parameter to be passed to *cb_fn*.
 *
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
 * \param cpumask string containing cpumask in hex. The leading *0x*
 * is allowed but not required. The mask itself can be constructed as:
 * ((1 << cpu0) | (1 << cpu1) | ... | (1 << cpuN)).
 * \param dev_name bdev name to associate with this vhost device
 * \param readonly if set, all writes to the device will fail with
 * \c VIRTIO_BLK_S_IOERR error code.
 * \param packed_ring this controller supports packed ring if set.
 *
 * \return 0 on success, negative errno on error.
 */
int spdk_vhost_blk_construct(const char *name, const char *cpumask, const char *dev_name,
			     bool readonly, bool packed_ring);

/**
 * Remove a vhost device. The device must not have any open connections on it's socket.
 *
 * \param vdev vhost blk device.
 *
 * \return 0 on success, negative errno on error.
 */
int spdk_vhost_dev_remove(struct spdk_vhost_dev *vdev);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_VHOST_H */
