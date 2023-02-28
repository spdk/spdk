/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_BDEV_PMEM_H
#define SPDK_BDEV_PMEM_H

#include "spdk/bdev.h"

typedef void (*spdk_delete_pmem_complete)(void *cb_arg, int bdeverrno);

/**
 * Create new pmem bdev.
 *
 * \param pmem_file Pointer to pmem pool file.
 * \param name Bdev name.
 * \param bdev output parameter for bdev when operation is successful.
 * \return 0 on success.
 *         -EIO if pool check failed
 *         -EINVAL if input parameters check failed
 *         -ENOMEM if buffer cannot be allocated
 */
int create_pmem_disk(const char *pmem_file, const char *name, struct spdk_bdev **bdev);

/**
 * Delete pmem bdev.
 *
 * \param name Name of pmem bdev.
 * \param cb_fn Function to call after deletion.
 * \param cb_arg Argument to pass to cb_fn.
 */
void delete_pmem_disk(const char *name, spdk_delete_pmem_complete cb_fn,
		      void *cb_arg);

#endif /* SPDK_BDEV_PMEM_H */
