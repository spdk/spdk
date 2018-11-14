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

#ifndef SPDK_NOTIFY_CONFIG_H_
#define SPDK_NOTIFY_CONFIG_H_

#include "spdk/stdinc.h"
#include "event.h"
#include "spdk/json.h"

#ifdef __cplusplus
extern "C" {
#endif

struct spdk_notify;
struct spdk_notify_type;

typedef void (*spdk_notify_handler)(struct spdk_notify *notify,
				    void *ctx);

typedef void (*spdk_notify_info)(struct spdk_json_write_ctx *w,
				 struct spdk_notify *notify, void *ctx);

typedef void (*spdk_notify_type_info)(struct spdk_json_write_ctx *w,
				      struct spdk_notify_type *type, void *ctx);

extern struct spdk_notify_type_list g_notify_types;


void spdk_notify_type_register(const char *name, spdk_notify_type_info write_type_cb,
			       spdk_notify_info write_info_cb);

struct spdk_notify_type *spdk_notify_type_first(void);
struct spdk_notify_type *spdk_notify_type_next(struct spdk_notify_type *prev);
struct spdk_notify *spdk_notify_alloc(struct spdk_notify_type *type);
void spdk_notify_get(struct spdk_notify *notify);
void spdk_notify_put(struct spdk_notify *notify);
void spdk_notify_send(struct spdk_notify *notify);
void spdk_notify_write_json(struct spdk_json_write_ctx *w, struct spdk_notify *notify);
struct spdk_notify_type *spdk_notify_first(void);
struct spdk_notify_type *spdk_notify_next(struct spdk_notify_type *prev);
int spdk_notify_listen(spdk_notify_handler cb, void *ctx);
int spdk_notify_unlisten(spdk_notify_handler cb, void *ctx);

#define SPDK_NOTIFY_REGISTER(name, type, write_type_cb, write_info_cb) \
struct spdk_notify_type type { \
	.name = name, \
	.write_info_cb = write_info_cb \
};	\
static void __attribute__((constructor)) notify_register_##write_type_cb(void) \
{ \
	spdk_notify_type_register(name, write_type_cb, write_info_cb); \
}

#ifdef __cplusplus
}
#endif

#endif
