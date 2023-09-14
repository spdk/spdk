/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 */

#include "spdk/queue.h"

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
ftl_property_register(struct spdk_ftl_dev *dev, const char *name)
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
