/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 */

#include "spdk/queue.h"
#include "spdk/json.h"
#include "spdk/jsonrpc.h"

#include "ftl_core.h"
#include "ftl_property.h"

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

	/** Link to put the property to the list */
	LIST_ENTRY(ftl_property) entry;
};

static struct ftl_property *
get_property_item(struct ftl_properties *properties, const char *name)
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
ftl_property_register(struct spdk_ftl_dev *dev, const char *name, void *value, size_t size,
		      ftl_property_dump_fn dump)
{
	struct ftl_properties *properties = dev->properties;

	if (get_property_item(properties, name)) {
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
