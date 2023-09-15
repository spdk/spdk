/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 */

#include "spdk/stdinc.h"
#include "spdk/queue.h"
#include "spdk/log.h"

#include "ftl_core.h"
#include "ftl_base_dev.h"
#include "utils/ftl_defs.h"

static TAILQ_HEAD(, ftl_base_device_type) g_devs = TAILQ_HEAD_INITIALIZER(g_devs);
static pthread_mutex_t g_devs_mutex = PTHREAD_MUTEX_INITIALIZER;

static const struct ftl_base_device_type *
ftl_base_device_type_get_desc(const char *name)
{
	struct ftl_base_device_type *entry;

	TAILQ_FOREACH(entry, &g_devs, base_devs_entry) {
		if (0 == strcmp(entry->name, name)) {
			return entry;
		}
	}

	return NULL;
}

static bool
ftl_base_device_valid(const struct ftl_base_device_type *type)
{
	return type && type->name && strlen(type->name);
}

void
ftl_base_device_register(struct ftl_base_device_type *type)
{
	if (!ftl_base_device_valid(type)) {
		SPDK_ERRLOG("[FTL] Base device type is invalid\n");
		ftl_abort();
	}

	pthread_mutex_lock(&g_devs_mutex);
	if (!ftl_base_device_type_get_desc(type->name)) {
		TAILQ_INSERT_TAIL(&g_devs, type, base_devs_entry);

		SPDK_NOTICELOG("[FTL] Registered base device, name: %s\n", type->name);
	} else {
		SPDK_ERRLOG("[FTL] Cannot register base device, already exist, name: %s\n", type->name);
		ftl_abort();
	}

	pthread_mutex_unlock(&g_devs_mutex);
}

const struct ftl_base_device_type *
ftl_base_device_get_type_by_bdev(struct spdk_ftl_dev *dev, struct spdk_bdev *bdev)
{
	struct ftl_base_device_type *type;

	pthread_mutex_lock(&g_devs_mutex);

	TAILQ_FOREACH(type, &g_devs, base_devs_entry) {
		if (type->ops.is_bdev_compatible) {
			if (type->ops.is_bdev_compatible(dev, bdev)) {
				break;
			}
		}
	}

	pthread_mutex_unlock(&g_devs_mutex);

	return type;
}
