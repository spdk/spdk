/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

#ifndef __NVME_CUSE_H__
#define __NVME_CUSE_H__

#include "spdk/nvme.h"

int nvme_cuse_register(struct spdk_nvme_ctrlr *ctrlr, const char *dev_path);
void nvme_cuse_unregister(struct spdk_nvme_ctrlr *ctrlr);

#endif /* __NVME_CUSE_H__ */
