/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation. All rights reserved.
 */

#ifndef _SPDK_VFIO_USER_PCI_H
#define _SPDK_VFIO_USER_PCI_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

struct vfio_device;

int spdk_vfio_user_pci_bar_access(struct vfio_device *dev, uint32_t index,
				  uint64_t offset, size_t len, void *buf,
				  bool is_write);

void *spdk_vfio_user_get_bar_addr(struct vfio_device *dev, uint32_t index,
				  uint64_t offset, uint32_t len);

struct vfio_device *spdk_vfio_user_setup(const char *path);

void spdk_vfio_user_release(struct vfio_device *dev);

#ifdef __cplusplus
}
#endif

#endif
