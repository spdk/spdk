/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.
 *   All rights reserved.
 */

#ifndef EXTERNAL_NVME_H
#define EXTERNAL_NVME_H

#include "spdk/env.h"
#include "spdk/nvme_spec.h"

struct nvme_ctrlr;

/**
 * Callback for nvme_probe() to report a device that has been attached to
 * the userspace NVMe driver.
 *
 * \param cb_ctx Opaque value passed to nvme_attach_cb().
 * \param addr The PCI address of the NVMe controller.
 * \param ctrlr Opaque handle to NVMe controller.
 */
typedef void (*nvme_attach_cb)(void *cb_ctx, const struct spdk_pci_addr *addr,
			       struct nvme_ctrlr *ctrlr);

/**
 * Enumerate PCIe bus and attach all NVMe devices found to the driver.
 *
 * This function is not thread safe and should only be called from one thread at
 * a time while no other threads are actively using any NVMe devices.
 *
 * \param cb_ctx Opaque value which will be passed back in cb_ctx parameter of
 * the callbacks.
 * \param attach_cb will be called for each NVMe device found
 *
 * \return 0 on success, negative errno on failure.
 */
int nvme_probe(nvme_attach_cb attach_cb, void *ctx);

/**
 * Connect the NVMe driver to the device located at the given transport ID.
 *
 * This function is not thread safe and should only be called from one thread at
 * a time while no other threads are actively using this NVMe device.
 *
 * \param addr The PCI address of the NVMe controller to connect.
 *
 * \return pointer to the connected NVMe controller or NULL if there is any failure.
 */
struct nvme_ctrlr *nvme_connect(struct spdk_pci_addr *addr);

/**
 * Detach specified device returned by nvme_probe()'s attach_cb. After returning
 * the nvme_ctrlr handle is no longer valid.
 *
 * This function should be called from a single thread while no other threads
 * are actively using the NVMe device.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 */
void nvme_detach(struct nvme_ctrlr *ctrlr);

/**
 * Get the identify controller data as defined by the NVMe specification.
 *
 * This function is thread safe and can be called at any point while the controller
 * is attached to the SPDK NVMe driver.
 *
 * \param ctrlr Opaque handle to NVMe controller.
 *
 * \return pointer to the identify controller data.
 */
const struct spdk_nvme_ctrlr_data *nvme_ctrlr_get_data(struct nvme_ctrlr *ctrlr);

#endif /* EXTERNAL_NVME_H */
