/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2025 Huawei Technologies
 *   All rights reserved.
 */

#ifndef SPDK_VBDEV_OCF_H
#define SPDK_VBDEV_OCF_H

#include "vbdev_ocf_cache.h"
#include "vbdev_ocf_core.h"

typedef void (*vbdev_ocf_cache_start_cb)(struct vbdev_ocf_cache *vbdev_cache, void *cb_arg, int error);
typedef void (*vbdev_ocf_cache_stop_cb)(void *cb_arg, int error);
typedef void (*vbdev_ocf_core_add_cb)(struct vbdev_ocf_core *vbdev_core, void *cb_arg, int error);
typedef void (*vbdev_ocf_core_remove_cb)(void *cb_arg, int error);
typedef void (*vbdev_ocf_get_bdevs_cb)(void *cb_arg1, void *cb_arg2);

// merge into one for all start/stop/add/remove ?
struct vbdev_ocf_cache_start_ctx {
	struct vbdev_ocf_cache *	cache;
	vbdev_ocf_cache_start_cb	rpc_cb_fn;
	void *				rpc_cb_arg;
};

struct vbdev_ocf_cache_stop_ctx {
	struct vbdev_ocf_cache *	cache;
	vbdev_ocf_cache_stop_cb		rpc_cb_fn;
	void *				rpc_cb_arg;
};

struct vbdev_ocf_core_add_ctx {
	struct vbdev_ocf_cache *	cache;
	struct vbdev_ocf_core *		core;
	vbdev_ocf_core_add_cb		rpc_cb_fn;
	void *				rpc_cb_arg;
};

struct vbdev_ocf_core_remove_ctx {
	struct vbdev_ocf_core *		core;
	vbdev_ocf_core_remove_cb	rpc_cb_fn;
	void *				rpc_cb_arg;
};

/* RPC entry point. */
void vbdev_ocf_cache_start(const char *cache_name,
			   const char *bdev_name,
			   const char *cache_mode,
			   const uint8_t cache_line_size,
			   vbdev_ocf_cache_start_cb cb_fn,
			   void *cb_arg);

/* RPC entry point. */
void vbdev_ocf_cache_stop(const char *cache_name,
			  vbdev_ocf_cache_stop_cb cb_fn,
			  void *cb_arg);

/* RPC entry point. */
void vbdev_ocf_core_add(const char *core_name,
			const char *bdev_name,
			const char *cache_name,
			vbdev_ocf_core_add_cb cb_fn,
			void *cb_arg);

/* RPC entry point. */
void vbdev_ocf_core_remove(const char *core_name,
			   vbdev_ocf_core_remove_cb cb_fn,
			   void *cb_arg);

/* RPC entry point. */
void vbdev_ocf_get_bdevs(const char *name,
			 vbdev_ocf_get_bdevs_cb cb_fn,
			 void *cb_arg1,
			 void *cb_arg2);

#endif
