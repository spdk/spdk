/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 */

#include "spdk/queue.h"
#include "spdk/json.h"
#include "spdk/jsonrpc.h"

#include "ftl_core.h"
#include "ftl_property.h"
#include "mngt/ftl_mngt.h"

struct ftl_properties {
	LIST_HEAD(, ftl_property) list;
};

/**
 * @brief FTL property descriptor
 */
struct ftl_property {
	/** Name of the property */
	const char *name;

	/* Pointer to the value of property */
	void *value;

	/* The value size of the property */
	size_t size;

	/* The function to dump the value of property into the specified JSON RPC request */
	ftl_property_dump_fn dump;

	/* Decode property value and store it in output */
	ftl_property_decode_fn decode;

	/* Set the FTL property */
	ftl_property_set_fn set;

	/** Link to put the property to the list */
	LIST_ENTRY(ftl_property) entry;
};

static struct ftl_property *
get_property(struct ftl_properties *properties, const char *name)
{
	struct ftl_property *entry;

	LIST_FOREACH(entry, &properties->list, entry) {
		/* TODO think about strncmp */
		if (0 == strcmp(entry->name, name)) {
			return entry;
		}
	}

	return NULL;
}

void
ftl_property_register(struct spdk_ftl_dev *dev,
		      const char *name, void *value, size_t size,
		      ftl_property_dump_fn dump,
		      ftl_property_decode_fn decode,
		      ftl_property_set_fn set)
{
	struct ftl_properties *properties = dev->properties;

	if (get_property(properties, name)) {
		FTL_ERRLOG(dev, "FTL property registration ERROR, already exist, name %s\n", name);
		ftl_abort();
	} else {
		struct ftl_property *prop = calloc(1, sizeof(*prop));
		if (NULL == prop) {
			FTL_ERRLOG(dev, "FTL property registration ERROR, out of memory, name %s\n", name);
			ftl_abort();
		}

		prop->name = name;
		prop->value = value;
		prop->size = size;
		prop->dump = dump;
		prop->decode = decode;
		prop->set = set;
		LIST_INSERT_HEAD(&properties->list, prop, entry);
	}
}

int
ftl_properties_init(struct spdk_ftl_dev *dev)
{
	dev->properties = calloc(1, sizeof(*dev->properties));
	if (!dev->properties) {
		return -ENOMEM;
	}

	LIST_INIT(&dev->properties->list);
	return 0;
}

void
ftl_properties_deinit(struct spdk_ftl_dev *dev)
{
	struct ftl_properties *properties = dev->properties;
	struct ftl_property *prop;

	if (!properties) {
		return;
	}

	while (!LIST_EMPTY(&properties->list)) {
		prop = LIST_FIRST(&properties->list);
		LIST_REMOVE(prop, entry);
		free(prop);
	}

	free(dev->properties);
}

void
ftl_property_dump(struct spdk_ftl_dev *dev, struct spdk_jsonrpc_request *request)
{
	struct ftl_properties *properties = dev->properties;
	struct ftl_property *prop;
	struct spdk_json_write_ctx *w;

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "name", dev->conf.name);

	spdk_json_write_named_object_begin(w, "properties");
	LIST_FOREACH(prop, &properties->list, entry) {
		prop->dump(prop, w);
	}
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);
}

void
ftl_property_dump_bool(const struct ftl_property *property,
		       struct spdk_json_write_ctx *w)
{
	bool *value = property->value;

	assert(property->size == sizeof(*value));
	spdk_json_write_named_bool(w, property->name, *value);
}

void
ftl_property_dump_uint64(const struct ftl_property *property,
			 struct spdk_json_write_ctx *w)
{
	uint64_t *value = property->value;

	assert(property->size == sizeof(*value));
	spdk_json_write_named_uint64(w, property->name, *value);
}

void
ftl_property_dump_uint32(const struct ftl_property *property,
			 struct spdk_json_write_ctx *w)
{
	uint32_t *value = property->value;

	assert(property->size == sizeof(*value));
	spdk_json_write_named_uint32(w, property->name, *value);
}

int
ftl_property_decode(struct spdk_ftl_dev *dev, const char *name, const char *value,
		    size_t value_size, void **output, size_t *output_size)
{
	struct ftl_properties *properties = dev->properties;
	struct ftl_property *prop = get_property(properties, name);
	int rc;

	if (!prop) {
		FTL_ERRLOG(dev, "Property doesn't exist, name %s\n", name);
		return -ENOENT;
	}

	if (!prop->decode) {
		FTL_ERRLOG(dev, "Property is read only, name %s\n", name);
		return -EACCES;
	}

	assert(prop->size);
	assert(NULL == *output);

	/* Allocate buffer for the new value of the property */
	*output = calloc(1, prop->size);
	if (NULL == *output) {
		FTL_ERRLOG(dev, "Property allocation memory error, name %s\n", name);
		return -EACCES;
	}
	*output_size = prop->size;

	rc = prop->decode(dev, prop, value, value_size, *output, *output_size);
	if (rc) {
		FTL_ERRLOG(dev, "Property decode error, name %s\n", name);
		free(*output);
		*output = NULL;
		return rc;
	}

	return 0;
}

int
ftl_property_set(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt,
		 const char *name, void *value, size_t value_size)
{
	struct ftl_properties *properties = dev->properties;
	struct ftl_property *prop = get_property(properties, name);

	if (!prop) {
		FTL_ERRLOG(dev, "Property doesn't exist, name %s\n", name);
		return -ENOENT;
	}

	if (!prop->set) {
		FTL_ERRLOG(dev, "Property is read only, name %s\n", name);
		return -EACCES;
	}

	prop->set(dev, mngt, prop, value, value_size);
	return 0;
}

void
ftl_property_set_generic(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt,
			 const struct ftl_property *property, void *new_value, size_t new_value_size)
{
	ftl_bug(property->size != new_value_size);
	memcpy(property->value, new_value, property->size);
	ftl_mngt_next_step(mngt);
}

int
ftl_property_decode_bool(struct spdk_ftl_dev *dev, struct ftl_property *property,
			 const char *value, size_t value_size, void *output, size_t output_size)
{
	bool *out = output;

	if (sizeof(bool) != output_size) {
		return -ENOBUFS;
	}

	if (strnlen(value, value_size) == value_size) {
		return -EINVAL;
	}

	if (0 == strncmp(value, "true", strlen("true"))) {
		*out = true;
		return 0;
	}

	if (0 == strncmp(value, "false", strlen("false"))) {
		*out = false;
		return 0;
	}

	return -EINVAL;
}
