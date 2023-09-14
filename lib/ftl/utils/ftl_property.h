/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 */

#ifndef FTL_PROPERTY_H
#define FTL_PROPERTY_H

#include "spdk/stdinc.h"

struct spdk_ftl_dev;

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
 * @brief Register a FTL property
 *
 * @param dev FTL device
 * @param name the FTL property name
 */
void ftl_property_register(struct spdk_ftl_dev *dev, const char *name);

/**
 * @brief Dump FTL properties to the JSON request
 *
 * @param dev FTL device
 * @param request The JSON request where to store the FTL properties
 */
void ftl_property_dump(struct spdk_ftl_dev *dev, struct spdk_jsonrpc_request *request);

#endif
