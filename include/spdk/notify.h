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

#ifndef SPDK_RPC_CONFIG_H_
#define SPDK_RPC_CONFIG_H_

#include "spdk/stdinc.h"
#include "event.h"

#ifdef __cplusplus
extern "C" {
#endif

struct spdk_notify;
struct spdk_notify_type;

typedef void (*spdk_notify_handler)(struct spdk_notify *notify,
				    void *ctx);

typedef const char *(*spdk_notify_get_info)(struct spdk_notify_type *notify,
		void *ctx);

struct spdk_notify_client {
	spdk_notify_handler cb;
	void *ctx;
	TAILQ_ENTRY(spdk_notify_client) tailq;
};
TAILQ_HEAD(spdk_notify_client_list, spdk_notify_client);

struct spdk_notify_type {
	const char *name;
	spdk_notify_get_info get_object_cb;
	spdk_notify_get_info get_uuid_cb;
	struct spdk_notify_client_list clients;
	TAILQ_ENTRY(spdk_notify_type) tailq;
};
TAILQ_HEAD(spdk_notify_type_list, spdk_notify_type);
extern struct spdk_notify_type_list g_notify_types;


/** notify */
struct spdk_notify {
	struct spdk_notify_type *type;
	char *object_name;
	char *uuid;
};

/**
 * This call provides ability to register notify type
 */
void spdk_notify_register_type(const char *name,
			       spdk_notify_get_info get_object_cb, spdk_notify_get_info get_uuid_cb);

/**
 * Send notify to all registered listeners
 */
void spdk_notify_send(struct spdk_notify *notify, void *ctx);

/**
 * Get notify types
 */
int spdk_notify_get_types(const char **notify_names, size_t *count);

/**
 * Register for notifys
 */
int spdk_notify_listen(spdk_notify_handler cb,
		       void *ctx);

const char *spdk_notify_get_name(struct spdk_notify *notify);
const char *spdk_notify_get_object(struct spdk_notify *notify);
const char *spdk_notify_get_uuid(struct spdk_notify *notify);

#define SPDK_NOTIFY_REGISTER(name, get_object_cb, get_uuid_cb) \
static void __attribute__((constructor)) notify_register_##get_object_cb(void) \
{ \
	spdk_notify_register_type(name, get_object_cb, get_uuid_cb); \
}

#ifdef __cplusplus
}
#endif

#endif
