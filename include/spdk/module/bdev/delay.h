/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

/** \file
 * Delay block device abstraction layer
 */

#ifndef SPDK_MODULE_BDEV_DELAY_H_
#define SPDK_MODULE_BDEV_DELAY_H_

#ifdef __cplusplus
extern "C" {
#endif

enum spdk_bdev_delay_io_type {
	SPDK_BDEV_DELAY_IO_TYPE_NONE,
	SPDK_BDEV_DELAY_IO_TYPE_AVG_READ,
	SPDK_BDEV_DELAY_IO_TYPE_P99_READ,
	SPDK_BDEV_DELAY_IO_TYPE_AVG_WRITE,
	SPDK_BDEV_DELAY_IO_TYPE_P99_WRITE,
};

#ifdef __cplusplus
}
#endif

#endif /* SPDK_MODULE_BDEV_DELAY_H_ */
