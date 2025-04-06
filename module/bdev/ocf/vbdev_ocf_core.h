/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2025 Huawei Technologies
 *   All rights reserved.
 */

#ifndef SPDK_VBDEV_OCF_CORE_H
#define SPDK_VBDEV_OCF_CORE_H

#include <ocf/ocf.h>
#include "spdk/bdev_module.h"
#include "volume.h"

/* Global list of all incomplete cores (not added to
 * the cache yet due to lack of cache or base device. */
extern STAILQ_HEAD(vbdev_ocf_incomplete_cores_head, vbdev_ocf_core) g_vbdev_ocf_incomplete_cores;

/* OCF module interface. */
extern struct spdk_bdev_module ocf_if;

/* Function table of exposed OCF vbdev. */
extern struct spdk_bdev_fn_table vbdev_ocf_fn_table;

#define vbdev_ocf_foreach_core_incomplete(core) \
	STAILQ_FOREACH(core, &g_vbdev_ocf_incomplete_cores, link)

#define vbdev_ocf_foreach_core_in_cache(core, cache) \
	STAILQ_FOREACH(core, &cache->cores, link)

struct vbdev_ocf_core_init_params {
	char *	bdev_name;
	char *	cache_name;
};

struct vbdev_ocf_core {
	char *					name;
	struct spdk_uuid			uuid;

	struct vbdev_ocf_base			base;

	struct vbdev_ocf_cache *		cache;

	ocf_core_t				ocf_core;
	struct ocf_mngt_core_config		ocf_core_cfg;

	/* Exposed OCF vbdev; the one which is registered in bdev layer for usage */
	struct spdk_bdev			ocf_vbdev;

	struct vbdev_ocf_core_init_params *	init_params;

	STAILQ_ENTRY(vbdev_ocf_core)		link;
};

struct vbdev_ocf_core_io_channel_ctx {
	ocf_queue_t			queue;
	struct spdk_io_channel *	cache_ch;
	struct spdk_io_channel *	core_ch;
	struct spdk_poller *		poller;
	struct spdk_thread *		thread;
	/* Currently kept only for its name used in debug log. */
	struct vbdev_ocf_core *		core; // rm ?
};

int vbdev_ocf_core_create(const char *core_name, struct vbdev_ocf_core **out);
void vbdev_ocf_core_destroy(struct vbdev_ocf_core *core);
int vbdev_ocf_core_set_config(struct vbdev_ocf_core *core);
int vbdev_ocf_core_base_attach(struct vbdev_ocf_core *core, const char *bdev_name);
void vbdev_ocf_core_base_detach(struct vbdev_ocf_core *core);
int vbdev_ocf_core_add_incomplete(struct vbdev_ocf_core *core, const char *bdev_name,
				  const char *cache_name);
void vbdev_ocf_core_remove_incomplete(struct vbdev_ocf_core *core);
void vbdev_ocf_core_add_to_cache(struct vbdev_ocf_core *core, struct vbdev_ocf_cache *cache);
void vbdev_ocf_core_remove_from_cache(struct vbdev_ocf_core *core);
int vbdev_ocf_core_register(struct vbdev_ocf_core *core);
int vbdev_ocf_core_unregister(struct vbdev_ocf_core *core, spdk_bdev_unregister_cb cb_fn, void *cb_arg);

struct vbdev_ocf_cache *vbdev_ocf_core_get_cache(struct vbdev_ocf_core *core);
struct vbdev_ocf_core *vbdev_ocf_core_get_by_name(const char *core_name);
bool vbdev_ocf_core_cache_is_started(struct vbdev_ocf_core *core);
bool vbdev_ocf_core_is_incomplete(struct vbdev_ocf_core *core);

#endif
