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

#ifndef VBDEV_OCF_UTILS_H
#define VBDEV_OCF_UTILS_H

#include <ocf/ocf.h>

ocf_cache_mode_t ocf_get_cache_mode(const char *cache_mode);
const char *ocf_get_cache_modename(ocf_cache_mode_t mode);

/* Poller that supports continuations */
struct spdk_cont_poller;

/* Common continuation type for asynchronous OCF procedures */
typedef int (*cont_poller_fn)(struct spdk_cont_poller *, void *);

struct spdk_cont_poller *spdk_cont_poller_noop(void);
struct spdk_cont_poller *spdk_cont_poller_register(cont_poller_fn fn, void *ctx, int period);
int spdk_cont_poller_append(struct spdk_cont_poller *poller, cont_poller_fn fn, void *ctx);
int spdk_cont_poller_append_poller(struct spdk_cont_poller *poller, cont_poller_fn fn, void *ctx,
				   int period);

/* Common SPDK callback function that is not composable */
typedef void (*spdk_callback_fn)(int, void *);
int spdk_cont_poller_append_finish(struct spdk_cont_poller *poller, spdk_callback_fn cb, void *ctx);

/* Prevents poller from unregistering after function was called */
int spdk_cont_poller_repeat(struct spdk_cont_poller *poller);

/* Status returned by previus poller */
int spdk_cont_poller_parent_status(struct spdk_cont_poller *current);

#endif
