/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2025 Huawei Technologies
 *   All rights reserved.
 */

#ifndef SPDK_VBDEV_OCF_CACHE_H
#define SPDK_VBDEV_OCF_CACHE_H

#include <ocf/ocf.h>
#include "spdk/bdev_module.h"
#include "volume.h"

/* Global list of all caches (started and incomplete). */
extern STAILQ_HEAD(vbdev_ocf_caches_head, vbdev_ocf_cache) g_vbdev_ocf_caches;

/* OCF module interface. */
extern struct spdk_bdev_module ocf_if;

#define vbdev_ocf_foreach_cache(cache) \
	STAILQ_FOREACH(cache, &g_vbdev_ocf_caches, link)

struct vbdev_ocf_cache_init_params {
	char *	bdev_name;
};

struct vbdev_ocf_cache {
	char *					name;
	struct spdk_uuid			uuid;

	struct vbdev_ocf_base			base;

	STAILQ_HEAD(, vbdev_ocf_core)		cores;
	uint16_t				cores_count;

	ocf_cache_t				ocf_cache;
	ocf_queue_t				ocf_cache_mngt_q;
	struct ocf_mngt_cache_config		ocf_cache_cfg;
	struct ocf_mngt_cache_attach_config	ocf_cache_att_cfg;

	struct vbdev_ocf_cache_init_params *	init_params;

	STAILQ_ENTRY(vbdev_ocf_cache)		link;
};

struct vbdev_ocf_cache_mngt_queue_ctx {
	struct spdk_poller *		poller;
	struct spdk_thread *		thread;
	/* Currently kept only for its name used in debug log. */
	struct vbdev_ocf_cache *	cache; // rm ?
};

int vbdev_ocf_cache_create(const char *cache_name, struct vbdev_ocf_cache **out);
void vbdev_ocf_cache_destroy(struct vbdev_ocf_cache *cache);
int vbdev_ocf_cache_set_config(struct vbdev_ocf_cache *cache, const char *cache_mode,
			       const uint8_t cache_line_size);
int vbdev_ocf_cache_base_attach(struct vbdev_ocf_cache *cache, const char *bdev_name);
void vbdev_ocf_cache_base_detach(struct vbdev_ocf_cache *cache);
int vbdev_ocf_cache_add_incomplete(struct vbdev_ocf_cache *cache, const char *bdev_name);
void vbdev_ocf_cache_remove_incomplete(struct vbdev_ocf_cache *cache);
int vbdev_ocf_cache_mngt_queue_create(struct vbdev_ocf_cache *cache);

struct vbdev_ocf_cache *vbdev_ocf_cache_get_by_name(const char *cache_name);
bool vbdev_ocf_cache_is_running(struct vbdev_ocf_cache *cache);
bool vbdev_ocf_cache_is_incomplete(struct vbdev_ocf_cache *cache);

#endif
