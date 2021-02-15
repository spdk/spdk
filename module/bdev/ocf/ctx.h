/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
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
