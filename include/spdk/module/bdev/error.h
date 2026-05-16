/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Dell Inc, or its subsidiaries.
 */

/** \file
 * Error block device abstraction layer
 */

#ifndef SPDK_MODULE_BDEV_ERROR_H_
#define SPDK_MODULE_BDEV_ERROR_H_

#ifdef __cplusplus
extern "C" {
#endif

enum spdk_bdev_error_type {
	SPDK_BDEV_ERROR_TYPE_NO_ERROR = 0,
	SPDK_BDEV_ERROR_TYPE_FAILURE,
	SPDK_BDEV_ERROR_TYPE_PENDING,
	SPDK_BDEV_ERROR_TYPE_CORRUPT_DATA,
	SPDK_BDEV_ERROR_TYPE_NOMEM,
	SPDK_BDEV_ERROR_TYPE_NVME_FAILURE,
};

#ifdef __cplusplus
}
#endif

#endif /* SPDK_MODULE_BDEV_ERROR_H_ */
