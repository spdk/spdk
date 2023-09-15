/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
  */

#ifndef FTL_BASE_DEVICE_H
#define FTL_BASE_DEVICE_H

#include "spdk/stdinc.h"
#include "spdk/bdev_module.h"
#include "spdk/queue.h"
#include "ftl_layout.h"

struct spdk_ftl_dev;
struct ftl_mngt_process;

/**
 * @brief Base device operations interface
 */
struct ftl_base_device_ops {
	/**
	 * @brief Check if block device is valid for base device
	 *
	 * @param dev ftl device
	 * @param bdev bdev to be checked
	 *
	 * @retval true if bdev is valid for base device
	 * @retval false if bdev is not valid for base device
	 */
	bool (*is_bdev_compatible)(struct spdk_ftl_dev *dev, struct spdk_bdev *bdev);

	struct ftl_md_layout_ops md_layout_ops;
};

/**
 * @brief Base device type
 */
struct ftl_base_device_type {
	/**
	 * The name of the base device type
	 */
	const char *name;

	/**
	 * The base device operations
	 */
	const struct ftl_base_device_ops ops;

	/**
	 * Queue entry for FTL base devices
	 */
	TAILQ_ENTRY(ftl_base_device_type) base_devs_entry;
};

/**
 * @brief Macro to register base device type when the module is loaded
 *
 * @param desc Base device type descriptor
 */
#define FTL_BASE_DEVICE_TYPE_REGISTER(desc) \
static void __attribute__((constructor)) ftl_base_device_register_##desc(void) \
{ \
	ftl_base_device_register(&desc); \
}

/**
 * @brief Register base device type
 *
 * @param desc Base device type type
 */
void ftl_base_device_register(struct ftl_base_device_type *desc);

/**
 * @brief Get base device type by bdev
 *
 * @param bdev bdev for which base device type is requested
 *
 * @return Base device type descriptor
 */
const struct ftl_base_device_type *ftl_base_device_get_type_by_bdev(
	struct spdk_ftl_dev *dev, struct spdk_bdev *bdev);

#endif /* FTL_BASE_DEVICE_H */
