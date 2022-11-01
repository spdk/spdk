/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#ifndef VBDEV_OCF_CTX_H
#define VBDEV_OCF_CTX_H

#include <ocf/ocf.h>
#include "ocf_env.h"
#include "spdk/thread.h"

extern ocf_ctx_t vbdev_ocf_ctx;

#define OCF_WRITE_FLUSH 11

#define SPDK_OBJECT 1

/* Context of cache instance */
struct vbdev_ocf_cache_ctx {
	ocf_queue_t                  mngt_queue;
	ocf_queue_t                  cleaner_queue;
	pthread_mutex_t              lock;
	env_atomic                   refcnt;
};

void vbdev_ocf_cache_ctx_put(struct vbdev_ocf_cache_ctx *ctx);
void vbdev_ocf_cache_ctx_get(struct vbdev_ocf_cache_ctx *ctx);

int vbdev_ocf_ctx_init(void);
void vbdev_ocf_ctx_cleanup(void);

/* Thread safe queue creation and deletion
 * These are wrappers for original ocf_queue_create() and ocf_queue_put() */
int vbdev_ocf_queue_create(ocf_cache_t cache, ocf_queue_t *queue, const struct ocf_queue_ops *ops);
void vbdev_ocf_queue_put(ocf_queue_t queue);

#endif
