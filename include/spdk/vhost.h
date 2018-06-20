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
#include "spdk/json.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Callback function for spdk_vhost_fini().
 */
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
 * \return 0 on success, -1 on failure.
 */
int spdk_vhost_init(void);

/**
 * Clean up the environment of vhost after finishing the vhost application.
 *
 * \param fini_cb Called when the cleanup operation completes.
 */
void spdk_vhost_fini(spdk_vhost_fini_cb fini_cb);


/**
 * Write vhost subsystem configuration into provided JSON context.
 *
 * \param w JSON write context
 * \param done_ev call this event when done.
 */
void spdk_vhost_config_json(struct spdk_json_write_ctx *w, struct spdk_event *done_ev);

/**
 * Deinit vhost application. This is called once by SPDK app layer.
 */
void spdk_vhost_shutdown_cb(void);

/**
 * SPDK vhost target. This is an entity responsible for accepting remote
 * connections and creating vhost devices. Upon any single connection,
 * a separate vhost device will be created. Each device will use its own
 * queues to perform I/O, but all devices will have access to the same
 * storage set up by the target.
 *
 * All vtgt-changing functions operate directly on this object. Note that
 * \c spdk_vhost_tgt cannot be acquired directly. This object is only
 * accessible as a callback parameter via \c spdk_vhost_call_external_event.
 * This ensures that all access to the vtgt is piped through a single,
 * thread-safe API.
 */
struct spdk_vhost_tgt;

/**
 * Synchronized vhost event used for user callbacks.
 *
 * \param vtgt vhost target.
 * \param arg user-provided parameter.
 *
 * \return 0 on success, -1 on failure.
 */
typedef int (*spdk_vhost_event_fn)(struct spdk_vhost_tgt *vtgt, void *arg);

/**
 * Get name of a vhost target.  This is equal to the filename
 * of socket file. The name is constant throughout the lifetime of
 * a vtgt.
 *
 * \param vtgt vhost target.
 *
 * \return name of the vtgt.
 */
const char *spdk_vhost_tgt_get_name(struct spdk_vhost_tgt *vtgt);

/**
 * Get cpuset of a vhost target.  The cpuset is constant throughout the lifetime
 * of a vtgt. It is a subset of the cpumask SPDK was started with.
 *
 * \param vtgt vhost target.
 *
 * \return cpuset of the vtgt.
 */
const struct spdk_cpuset *spdk_vhost_tgt_get_cpumask(struct spdk_vhost_tgt *vtgt);

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
 * \param vtgt vhost target.
 * \param delay_base_us Base delay time in microseconds. If 0, coalescing is disabled.
 * \param iops_threshold IOPS threshold when coalescing is activated.
 */
int spdk_vhost_set_coalescing(struct spdk_vhost_tgt *vtgt, uint32_t delay_base_us,
			      uint32_t iops_threshold);

/**
 * Get coalescing parameters.
 *
 * \see spdk_vhost_set_coalescing
 *
 * \param vtgt vhost target.
 * \param delay_base_us Optional pointer to store base delay time.
 * \param iops_threshold Optional pointer to store IOPS threshold.
 */
void spdk_vhost_get_coalescing(struct spdk_vhost_tgt *vtgt, uint32_t *delay_base_us,
			       uint32_t *iops_threshold);

/**
 * Construct an empty vhost SCSI target.  This will create a Unix domain
 * socket together with a vhost-user slave server waiting for connections
 * on this socket. Creating a vtgt does not start any I/O pollers and does
 * not hog the CPU. I/O processing starts after establishing a connection
 * on the created socket and creating a vhost device. Currently, vhost SCSI
 * targets support only one connection per socket and all subsequent
 * connections will be rejected.
 *
 * All physical devices have to be separately attached to this
 * target via \c spdk_vhost_scsi_tgt_add_tgt().
 *
 * \see spdk_vhost_tgt
 *
 * This function is thread-safe.
 *
 * \param name name of the vhost target. The name will also be used
 * for socket name, which is exactly \c socket_base_dir/name
 * \param cpumask string containing cpumask in hex. The leading *0x*
 * is allowed but not required. The mask itself can be constructed as:
 * ((1 << cpu0) | (1 << cpu1) | ... | (1 << cpuN)).
 *
 * \return 0 on success, negative errno on error.
 */
int spdk_vhost_scsi_tgt_construct(const char *name, const char *cpumask);

/**
 * Construct and attach new SCSI target to a vhost SCSI target
 * on given (unoccupied) slot.  The SCSI target will be created with a single
 * LUN0 associated with the provided SPDK bdev. Currently only one LUN per
 * SCSI target is supported.
 *
 * If the vhost SCSI devices created by this target support \c
 * VIRTIO_SCSI_F_HOTPLUG feature, the new SCSI target should be
 * automatically detected by the device drivers.
 *
 * \param vtgt vhost SCSI target.
 * \param scsi_tgt_num SCSI target slot to attach to.
 * \param bdev_name name of the SPDK bdev to associate with LUN0.
 *
 * \return 0 on success, negative errno on error.
 */
int spdk_vhost_scsi_tgt_add_tgt(struct spdk_vhost_tgt *vtgt, unsigned scsi_tgt_num,
				const char *bdev_name);

/**
 * Get SCSI target from given slot of a vhost SCSI target. Max number of
 * available slots is defined by \c SPDK_VHOST_SCSI_CTRLR_MAX_DEVS.
 *
 * \param vtgt vhost SCSI target.
 * \param num slot id.
 *
 * \return SCSI device on given slot or NULL.
 */
struct spdk_scsi_dev *spdk_vhost_scsi_tgt_get_tgt(struct spdk_vhost_tgt *vtgt, uint8_t num);

/**
 * Detach and destruct SCSI target from a vhost SCSI target.
 *
 * If vhost SCSI target has a device exposed, it is required
 * that the device has negotiated \c VIRTIO_SCSI_F_HOTPLUG feature
 * flag. Otherwise an -ENOTSUP error code is returned. If the flag has
 * been negotiated, the SCSI target will be marked to be deleted. Actual
 * deletion is deferred until after all pending I/O to this vhost target
 * has finished.
 *
 * Once the target has been deleted (whether or not vhost SCSI target
 * is in use) given callback will be called.
 *
 * \param vtgt vhost SCSI target
 * \param scsi_tgt_num slot id to delete from
 * \param cb_fn callback to be fired once the SCSI target has been
 * successfully deleted.
 * \param cb_arg parameter to be passed to *cb_fn*.
 *
 * \return 0 on success, negative errno on error.
 */
int spdk_vhost_scsi_tgt_remove_tgt(struct spdk_vhost_tgt *vtgt, unsigned scsi_tgt_num,
				   spdk_vhost_event_fn cb_fn, void *cb_arg);

/**
 * Construct a vhost block target.  This will create a Unix domain
 * socket together with a vhost-user slave server waiting for connections
 * on this socket. Creating a vtgt does not start any I/O pollers and does
 * not hog the CPU. I/O processing starts after establishing a connection
 * on the created socket and creating a vhost device. Currently, vhost block
 * targets support only one connection per socket and all subsequent
 * connections will be rejected.
 *
 * vhost block target is tightly associated with given SPDK bdev. The
 * associated bdev can not be changed, unless it is hotremoved. This would
 * case all subsequent I/O to fail with \c VIRTIO_BLK_S_IOERR error code.
 *
 * \see spdk_vhost_tgt
 *
 * This function is thread-safe.
 *
 * \param name name of the vhost block target. The name will also be
 * used for socket name, which is exactly \c socket_base_dir/name
 * \param cpumask string containing cpumask in hex. The leading *0x*
 * is allowed but not required. The mask itself can be constructed as:
 * ((1 << cpu0) | (1 << cpu1) | ... | (1 << cpuN)).
 * \param dev_name bdev name to associate with this vhost target
 * \param readonly if set, all writes to the target will fail with
 * \c VIRTIO_BLK_S_IOERR error code.
 *
 * \return 0 on success, negative errno on error.
 */
int spdk_vhost_blk_construct(const char *name, const char *cpumask, const char *dev_name,
			     bool readonly);

/**
 * Remove a vhost target. It must not have any devices currently exposed.
 *
 * \param vtgt vhost target.
 *
 * \return 0 on success, negative errno on error.
 */
int spdk_vhost_tgt_remove(struct spdk_vhost_tgt *vtgt);

/**
 * Get underlying SPDK bdev from vhost block target. The bdev might be NULL, as it
 * could have been hotremoved.
 *
 * \param vtgt vhost block target.
 *
 * \return SPDK bdev associated with given vtgt.
 */
struct spdk_bdev *spdk_vhost_blk_get_dev(struct spdk_vhost_tgt *vtgt);

/**
 * Call function on reactor of given vhost target. If the target is not in use,
 * the event will be called right away on the caller's thread.
 *
 * This function is thread safe.
 *
 * \param vtgt_name name of the vhost target to run this event on.
 * \param fn function to be called. The first parameter of callback function is
 * either actual spdk_vhost_tgt pointer or NULL in case vtgt with given name doesn't
 * exist. The second param is user provided argument *arg*.
 * \param arg parameter to be passed to *fn*.
 */
void spdk_vhost_call_external_event(const char *vtgt_name, spdk_vhost_event_fn fn, void *arg);

/**
 * Call function for each available vhost target on its reactor.  This will call
 * given function in a chain, meaning that each callback will be called after the
 * previous one has finished. After given function has been called for all targets,
 * it will be called once again with first param - vhost target - set to NULL.
 *
 * This function is thread safe.
 *
 * \param fn function to be called for each vtgt. The first param will be
 * either vtgt pointer or NULL. The second param is user provided argument *arg*.
 * \param arg parameter to be passed to *fn*.
 */
void spdk_vhost_call_external_event_foreach(spdk_vhost_event_fn fn, void *arg);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_VHOST_H */
