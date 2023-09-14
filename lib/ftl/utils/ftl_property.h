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
void ftl_property_dump_bool(struct spdk_ftl_dev *dev, const struct ftl_property *property,
			    struct spdk_json_write_ctx *w);

/**
 * @brief A function to dump the FTL property which type is uint64
 */
void ftl_property_dump_uint64(struct spdk_ftl_dev *dev, const struct ftl_property *property,
			      struct spdk_json_write_ctx *w);

/**
 * @brief A function to dump the FTL property which type is uint32
 */
void ftl_property_dump_uint32(struct spdk_ftl_dev *dev, const struct ftl_property *property,
			      struct spdk_json_write_ctx *w);

/**
 * @brief Dump the value of property into the specified JSON RPC request
 *
 * @param dev FTL device
 * @param property The property to dump to the JSON RPC request
 * @param[out] w JSON RPC request
 */
typedef void (*ftl_property_dump_fn)(struct spdk_ftl_dev *dev, const struct ftl_property *property,
				     struct spdk_json_write_ctx *w);

/**
 * @brief Decode property value and store it in output
 *
 * @param dev FTL device
 * @param property The property
 * @param value The new property value
 * @param value_size The size of the value buffer
 * @param output The output where to store new value
 * @param output_size The decoded value output size
 */
typedef int (*ftl_property_decode_fn)(struct spdk_ftl_dev *dev, struct ftl_property *property,
				      const char *value, size_t value_size, void *output, size_t output_size);

/**
 * @brief Set the FTL property
 *
 * @param dev FTL device
 * @param mngt FTL management process handle
 * @param property The property
 * @param new_value The new property value to be set
 * @param new_value_size The size of the new property value
 */
typedef void (*ftl_property_set_fn)(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt,
				    const struct ftl_property *property, void *new_value, size_t new_value_size);

/**
 * @brief Register a FTL property
 *
 * @param dev FTL device
 * @param name the FTL property name
 * @param value Pointer to the value of property
 * @param size The value size of the property
 * @param unit The unit of the property value
 * @param desc The property description for user help
 * @param dump The function to dump the property to the JSON RPC request
 * @param decode The function to decode a new value of the property
 * @param set The function to execute the property setting procedure
 * @param verbose_mode The property is available in verbose mode only
 */
void ftl_property_register(struct spdk_ftl_dev *dev,
			   const char *name, void *value, size_t size,
			   const char *unit, const char *desc,
			   ftl_property_dump_fn dump,
			   ftl_property_decode_fn decode,
			   ftl_property_set_fn set,
			   bool verbose_mode);

/**
 * @brief Dump FTL properties to the JSON request
 *
 * @param dev FTL device
 * @param request The JSON request where to store the FTL properties
 */
void ftl_property_dump(struct spdk_ftl_dev *dev, struct spdk_jsonrpc_request *request);

/**
 * @brief Decode property value and store it in output
 *
 * @param dev FTL device
 * @param name The property name to be decoded
 * @param value The new property value
 * @param value_size The new property value buffer size
 * @param output The output where to store new value
 * @param output_size The decoded value output size
 */
int ftl_property_decode(struct spdk_ftl_dev *dev, const char *name, const char *value,
			size_t value_size, void **output, size_t *output_size);

/**
 * @brief The property bool decoder
 */
int ftl_property_decode_bool(struct spdk_ftl_dev *dev, struct ftl_property *property,
			     const char *value, size_t value_size, void *output, size_t output_size);

/**
 * @brief Set FTL property
 *
 * @param dev FTL device
 * @param mngt FTL management process handle
 * @param name The property name to be set
 * @param value The new property decoded value
 * @param output The size of the new property decoded value
 */
int ftl_property_set(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt,
		     const char *name, void *value, size_t value_size);

/**
 * @brief Generic setter of the property
 *
 * @note This setter does binary copy and finishes always call the next management step
 */
void ftl_property_set_generic(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt,
			      const struct ftl_property *property,
			      void *new_value, size_t new_value_size);

/**
 * @brief The wrapper function to register mutable boolean property
 *
 * @param dev FTL device
 * @param name The property name
 * @param value The pointer to the boolean value of the property
 * @param unit The property unit
 * @param desc The property description
 * @param verbose_mode The verbose mode flag
 */
static inline void
ftl_property_register_bool_rw(struct spdk_ftl_dev *dev, const char *name, bool *value,
			      const char *unit, const char *desc, bool verbose_mode)
{
	ftl_property_register(dev, name, value, sizeof(*value), unit, desc, ftl_property_dump_bool,
			      ftl_property_decode_bool, ftl_property_set_generic, verbose_mode);
}

#endif
