/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
  */

#ifndef FTL_NV_CACHE_DEVICE_H
#define FTL_NV_CACHE_DEVICE_H

#include "spdk/stdinc.h"
#include "spdk/bdev_module.h"
#include "ftl_layout.h"

struct spdk_ftl_dev;
struct ftl_nv_cache_chunk;
struct ftl_io;
struct ftl_mngt_process;

/**
 * @brief NV Cache device features and capabilities
 */
struct ftl_nv_cache_device_features {
	/*
	 * The placeholder for NV Cache device features. It will be filled in the future.
	 */
};

/**
 * @brief NV Cache device operations interface
 */
struct ftl_nv_cache_device_ops {
	/**
	 * @brief Initialize NV Cache device
	 *
	 * @param dev ftl device
	 *
	 * @return Initialization result
	 */
	int (*init)(struct spdk_ftl_dev *dev);

	/**
	 * @brief Deinitialize NV Cache device
	 *
	 * @param dev ftl device
	 */
	void (*deinit)(struct spdk_ftl_dev *dev);

	/**
	 * @brief Inform NV cache device that chunk is being opened
	 *
	 * @param dev ftl device
	 * @param chunk chunk is being opened
	 */
	void (*on_chunk_open)(struct spdk_ftl_dev *dev, struct ftl_nv_cache_chunk *chunk);

	/**
	 * @brief Inform NV cache device that chunk has been closed
	 *
	 * @param dev ftl device
	 * @param chunk chunk which has been closed
	 */
	void (*on_chunk_closed)(struct spdk_ftl_dev *dev, struct ftl_nv_cache_chunk *chunk);

	/**
	 * @brief Check if block device is valid for NV Cache device
	 *
	 * @param dev ftl device
	 * @param bdev bdev to be checked
	 *
	 * @retval true if bdev is valid for NV Cache device
	 * @retval false if bdev is not valid for NV Cache device
	 */
	bool (*is_bdev_compatible)(struct spdk_ftl_dev *dev, struct spdk_bdev *bdev);

	/**
	 * @brief Check if chunk is active and can be used for NV Cache
	 *
	 * @param dev FTL device
	 * @param chunk_offset chunk start offset to be checked
	 *
	 * @retval true if chunk is active
	 * @retval false if chunk is not active
	 */
	bool (*is_chunk_active)(struct spdk_ftl_dev *dev, uint64_t chunk_offset);

	/**
	 * @brief Write user IO to the NV cache device
	 *
	 * @param io FTL IO
	 *
	 */
	void (*write)(struct ftl_io *io);

	/**
	 * @brief Process NV Cache device
	 *
	 * @param dev ftl device
	 */
	void (*process)(struct spdk_ftl_dev *dev);

	/**
	 * @brief Recover open chunk
	 *
	 * @param dev ftl device
	 * @param mngt FTL management precess handler
	 * @param chunk NV cache chunk to be recovered
	 *
	 * @note When the recovery finished successfully then the procedure
	 * shall invoke ftl_mngt_next_step(mngt). If a failure occurred then
	 * the process shall call ftl_mngt_fail_step(mngt)
	 */
	void (*recover_open_chunk)(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt,
				   struct ftl_nv_cache_chunk *chunk);

	/**
	 * @brief Setup NV Cache device layout
	 *
	 * If the NV cache device requires to setup additional metadata regions,
	 * it can be done here.
	 *
	 * @param dev ftl device
	 *
	 * @retval 0 on success
	 * @retval non-zero on failure
	 */
	int (*setup_layout)(struct spdk_ftl_dev *dev);

	struct ftl_md_layout_ops md_layout_ops;
};

/**
 * @brief NV Cache device type
 */
struct ftl_nv_cache_device_type {
	/**
	 * The name of the NV cache device type
	 */
	const char *name;

	/**
	 * The features list of the NV cache device type
	 *
	 */
	const struct ftl_nv_cache_device_features features;

	/**
	 * The NV cache device operations
	 */
	const struct ftl_nv_cache_device_ops ops;

	/** Internal fields */
	struct {
		/* The queue entry to put this description to a queue */
		TAILQ_ENTRY(ftl_nv_cache_device_type) entry;
	} internal;
};

/**
 * @brief Macro to register NV Cache device type when the module is loaded
 *
 * @param desc NV Cache device type
 */
#define FTL_NV_CACHE_DEVICE_TYPE_REGISTER(desc) \
static void __attribute__((constructor)) ftl_nv_cache_device_register_##desc(void) \
{ \
	ftl_nv_cache_device_register(&desc); \
}

/**
 * @brief Register NV Cache device type
 *
 * @param type NV Cache device type
 */
void ftl_nv_cache_device_register(struct ftl_nv_cache_device_type *type);

/**
 * @brief Get NV Cache device type by bdev
 *
 * @param bdev bdev for which NV Cache device type is requested
 *
 * @return NV Cache device type
 */
const struct ftl_nv_cache_device_type *ftl_nv_cache_device_get_type_by_bdev(
	struct spdk_ftl_dev *dev, struct spdk_bdev *bdev);

#endif /* FTL_NV_CACHE_DEVICE_H */
