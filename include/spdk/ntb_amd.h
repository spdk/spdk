/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Advanced Micro Devices, Inc.
 *   All rights reserved.
 */

/** \file
 * AMD EPYC 4th-Generation Non-Transparent Bridge (NTB) user-space
 * driver public interface.
 */

#ifndef SPDK_NTB_AMD_H
#define SPDK_NTB_AMD_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "spdk/env.h"

/*
 * Device introspection attributes queried via spdk_ntb_amd_attr_get().
 *
 * These are SPDK_-prefixed so the string keys do not leak unprefixed names
 * into consumers' namespaces. Memory-window addressing (base address, bus
 * address and size) is *not* exposed as an attribute; use the typed
 * spdk_ntb_amd_get_peer_mw() accessor instead.
 */
#define SPDK_NTB_QUEUE_SZ_NAME      "queue_size"
#define SPDK_NTB_QUEUE_NUM_NAME     "queue_num"
#define SPDK_NTB_TOPO_NAME          "topo"
#define SPDK_NTB_LINK_STATUS_NAME   "link_status"
#define SPDK_NTB_SPEED_NAME         "speed"
#define SPDK_NTB_WIDTH_NAME         "width"
#define SPDK_NTB_MW_CNT_NAME        "mw_count"
#define SPDK_NTB_DB_CNT_NAME        "db_count"
#define SPDK_NTB_SPAD_CNT_NAME      "spad_count"

/**
 * Opaque handle for a single NTB channel returned by \ref spdk_ntb_amd_probe().
 */
struct spdk_ntb_amd_dev;

struct spdk_memory_domain;

/**
 * Callback for spdk_ntb_amd_probe() enumeration.
 *
 * \param cb_ctx User-specified opaque value corresponding to cb_ctx from spdk_ntb_amd_probe().
 * \param pci_dev PCI device that is being probed.
 *
 * \return true to attach to this device.
 */
typedef bool (*spdk_ntb_amd_probe_cb)(void *cb_ctx, struct spdk_pci_device *pci_dev);

/**
 * Callback for spdk_ntb_amd_probe() to report a device that has been attached to
 * the userspace NTB driver.
 *
 * \param cb_ctx User-specified opaque value corresponding to cb_ctx from spdk_ntb_amd_probe().
 * \param pci_dev PCI device that was attached to the driver.
 * \param ntb NTB channel that was attached to the driver.
 */
typedef void (*spdk_ntb_amd_attach_cb)(void *cb_ctx, struct spdk_pci_device *pci_dev,
				       struct spdk_ntb_amd_dev *ntb);

/*
 * Get current link status.
 */
int spdk_ntb_amd_get_link_status(struct spdk_ntb_amd_dev *ntb);

/*
 * Read a scratchpad register.
 *
 * \return 0 on success (register value stored in \p value), negative errno
 * on invalid arguments.
 */
int spdk_ntb_amd_spad_read(const struct spdk_ntb_amd_dev *ntb, int spad, bool peer,
			   uint32_t *value);

/*
 * Read the local doorbell status register.
 */
uint64_t spdk_ntb_amd_db_read(const struct spdk_ntb_amd_dev *ntb);

/*
 * Clear bits from the local doorbell status register.
 */
int spdk_ntb_amd_db_clear(struct spdk_ntb_amd_dev *ntb, uint64_t db_bits);

/*
 * Ring a doorbell on the peer.
 */
int spdk_ntb_amd_peer_db_set(struct spdk_ntb_amd_dev *ntb, uint8_t db_idx);

/*
 * Get an attribute (see NTB_*_NAME above) by name.
 */
int spdk_ntb_amd_attr_get(struct spdk_ntb_amd_dev *ntb, const char *attr_name,
			  uint64_t *attr_value);

/**
 * Read the cached "peer ready" flag for this channel.
 *
 * The flag is true once the local side has observed the peer-ready
 * doorbell at least once and is intended to gate the per-link-up setup
 * (memory-window translations, mempool buffer publication, etc.) so that
 * the application does not redo it for every duplicate doorbell.
 *
 * The driver itself only reads this flag (it folds it into the value
 * returned for the "link_status" attribute). Updating it is the
 * application's responsibility — see \ref spdk_ntb_amd_peer_ready_set().
 *
 * \param ntb NTB channel handle returned by spdk_ntb_amd_probe().
 * \return true if the peer is currently considered ready, false otherwise
 *         (also returns false if \p ntb is NULL).
 */
bool spdk_ntb_amd_peer_ready_get(const struct spdk_ntb_amd_dev *ntb);

/**
 * Update the cached "peer ready" flag for this channel.
 *
 * Applications call this in their doorbell handler when they observe a
 * peer-ready (set true) or peer-down / peer-ack-down (set false)
 * notification. The flag is consumed by spdk_ntb_amd_attr_get(NTB_LINK_STATUS_NAME).
 *
 * \param ntb NTB channel handle returned by spdk_ntb_amd_probe().
 * \param ready New value for the flag.
 * \return 0 on success, -EINVAL if \p ntb is NULL.
 */
int spdk_ntb_amd_peer_ready_set(struct spdk_ntb_amd_dev *ntb, bool ready);

/**
 * Perform the peer-to-peer NTB handshake.
 *
 * Publishes the memory-window count via scratchpad, programs each
 * memory-window translation to a non-overlapping slice of \p buffer, and
 * rings doorbell 0 to notify the peer.
 *
 * \param ntb NTB channel handle returned by spdk_ntb_amd_probe().
 * \param buffer Local DMA-capable staging buffer that backs *all* exposed
 *               memory windows. The buffer must be physically contiguous
 *               (e.g. allocated via spdk_dma_zmalloc()) and at least
 *               \c sum(mw_size[i]) bytes long, where \c mw_size[i] is the
 *               size of the i'th memory window. Memory window i is
 *               programmed to the slice starting at offset
 *               \c sum(mw_size[0..i-1]).
 * \param buffer_size Size of \p buffer in bytes; rejected with -EINVAL if
 *                    smaller than the sum of memory-window sizes, or if the
 *                    buffer is not physically contiguous across that range.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_ntb_amd_handshake_work(struct spdk_ntb_amd_dev *ntb, void *buffer,
				size_t buffer_size);

/**
 * Finalize device setup once the link is up. Must be called after
 * spdk_ntb_amd_handshake_work() and before any data movement.
 *
 * \param ntb NTB channel handle returned by spdk_ntb_amd_probe().
 * \return 0 on success, negative errno on failure.
 */
int spdk_ntb_amd_dev_start(struct spdk_ntb_amd_dev *ntb);

/**
 * Select which memory window (BAR) is used for the local mapping in spdk_ntb_amd_dev_start().
 *
 * \param ntb NTB device.
 * \param mw_idx Memory window index, less than mw_cnt.
 *
 * \return 0 on success, -EINVAL if arguments are invalid.
 */
int spdk_ntb_amd_set_active_mw(struct spdk_ntb_amd_dev *ntb, uint8_t mw_idx);

/**
 * Describe a peer memory window as a DMA target.
 *
 * Fills \p iov with the window's device (bus) address (\c iov_base) and size
 * (\c iov_len), and \p domain with the NTB device memory domain. The
 * (iovec, memory-domain) pair can be passed directly to
 * spdk_ae4dma_build_copy() as a copy endpoint: because the domain is
 * non-NULL and describes device memory, the engine treats \c iov_base as an
 * already-resolved bus address (no spdk_vtophys()) and as IO memory.
 *
 * For CPU access to the same window (e.g. memcpy), \p vaddr optionally
 * receives the locally-mapped virtual address.
 *
 * \param ntb NTB channel handle returned by spdk_ntb_amd_probe().
 * \param mw_idx Memory window index, less than the memory-window count.
 * \param iov Output iovec describing the window (bus address + size).
 * \param domain Output NTB memory domain for the window. May be NULL.
 * \param vaddr Optional output for the locally-mapped virtual address. May be
 *              NULL if the caller only needs the DMA view.
 *
 * \return 0 on success, -EINVAL on invalid arguments, -ENODEV if the window
 *         is not mapped.
 */
int spdk_ntb_amd_get_peer_mw(struct spdk_ntb_amd_dev *ntb, uint8_t mw_idx,
			     struct iovec *iov, struct spdk_memory_domain **domain,
			     void **vaddr);

/**
 * Enumerate the NTB devices attached to the system and attach the userspace
 * NTB driver to them if desired.
 *
 * If called more than once, only devices that are not already attached to the
 * SPDK NTB driver will be reported.
 *
 * To stop using the controller and release its associated resources, call
 * spdk_ntb_amd_detach() with the ntb_channel instance returned by this function.
 *
 * \param cb_ctx Opaque value which will be passed back in cb_ctx parameter of
 * the callbacks.
 * \param probe_cb will be called once per NTB device found in the system.
 * \param attach_cb will be called for devices for which probe_cb returned true
 * once the NTB controller has been attached to the userspace driver.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_ntb_amd_probe(void *cb_ctx, spdk_ntb_amd_probe_cb probe_cb,
		       spdk_ntb_amd_attach_cb attach_cb);

/**
 * Detach specified device returned by spdk_ntb_amd_probe() from the NTB driver.
 *
 * \param ntb NTB channel to detach from the driver.
  */
void spdk_ntb_amd_detach(struct spdk_ntb_amd_dev *ntb);

#ifdef __cplusplus
}
#endif

#endif
