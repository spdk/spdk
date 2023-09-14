/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 */

#ifndef FTL_PROPERTY_H
#define FTL_PROPERTY_H

#include "spdk/stdinc.h"

struct spdk_ftl_dev;
struct ftl_property;

/**
 * @brief Init the FTL properties system
 *
 * @retval 0 Success
 * @retval Non-zero a Failure
 */
int ftl_properties_init(struct spdk_ftl_dev *dev);

/**
 * @brief Deinit the FTL properties system
 */
void ftl_properties_deinit(struct spdk_ftl_dev *dev);

/**
 * @brief A function to dump the FTL property which type is bool
 */
void ftl_property_dump_bool(const struct ftl_property *property, struct spdk_json_write_ctx *w);

/**
 * @brief A function to dump the FTL property which type is uint64
 */
void ftl_property_dump_uint64(const struct ftl_property *property, struct spdk_json_write_ctx *w);

/**
 * @brief A function to dump the FTL property which type is uint32
 */
void ftl_property_dump_uint32(const struct ftl_property *property, struct spdk_json_write_ctx *w);

/**
 * @brief Dump the value of property into the specified JSON RPC request
 *
 * @param property The property to dump to the JSON RPC request
 * @param[out] w JSON RPC request
 */
typedef void (*ftl_property_dump_fn)(const struct ftl_property *property,
				     struct spdk_json_write_ctx *w);

/**
 * @brief Register a FTL property
 *
 * @param dev FTL device
 * @param name the FTL property name
 * @param value Pointer to the value of property
 * @param size The value size of the property
 * @param dump The function to dump the property to the JSON RPC request
 */
void ftl_property_register(struct spdk_ftl_dev *dev, const char *name, void *value, size_t size,
			   ftl_property_dump_fn dump);

/**
 * @brief Dump FTL properties to the JSON request
 *
 * @param dev FTL device
 * @param request The JSON request where to store the FTL properties
 */
void ftl_property_dump(struct spdk_ftl_dev *dev, struct spdk_jsonrpc_request *request);

#endif
