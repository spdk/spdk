/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   Copyright (C) 2025 Huawei Technologies
 *   All rights reserved.
 */

#ifndef VBDEV_OCF_CTX_H
#define VBDEV_OCF_CTX_H

#include <ocf/ocf.h>

extern ocf_ctx_t vbdev_ocf_ctx;

#define OCF_WRITE_FLUSH 11

#define SPDK_OBJECT 1

// why?
#define VBDEV_OCF_QUEUE_RUN_MAX 32

int vbdev_ocf_ctx_init(void);
void vbdev_ocf_ctx_cleanup(void);

/* Thread safe queue creation and deletion
 * These are wrappers for original ocf_queue_create() and ocf_queue_put() */
int vbdev_ocf_queue_create(ocf_cache_t cache, ocf_queue_t *queue, const struct ocf_queue_ops *ops);
int vbdev_ocf_queue_create_mngt(ocf_cache_t cache, ocf_queue_t *queue,
				const struct ocf_queue_ops *ops);
void vbdev_ocf_queue_put(ocf_queue_t queue);
int vbdev_ocf_queue_poller(void *ctx);

#endif
