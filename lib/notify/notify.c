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

#include <sys/file.h>

#include "spdk/stdinc.h"
#include "spdk/notify.h"
#include "spdk/util.h"
#include "spdk/queue.h"

static size_t g_notify_types_count = 0;
struct spdk_notify_type_list g_notify_types = TAILQ_HEAD_INITIALIZER(
			g_notify_types);

struct spdk_notify_client_list g_notify_clients = TAILQ_HEAD_INITIALIZER(
			g_notify_clients);

void
spdk_notify_register_type(const char *name,
			  spdk_notify_get_info get_object_cb, spdk_notify_get_info get_uuid_cb)
{
	struct spdk_notify_type *type;
	type = calloc(1, sizeof(struct spdk_notify_type));
	assert(type != NULL);

	type->name = strdup(name);
	assert(type->name != NULL);

	TAILQ_INSERT_TAIL(&g_notify_types, type, tailq);
	g_notify_types_count++;
}

struct spdk_notify_type *
spdk_notify_first(void)
{
	struct spdk_notify_type *type;

	type = TAILQ_FIRST(&g_notify_types);

	return type;
}

struct spdk_notify_type *
spdk_notify_next(struct spdk_notify_type *prev)
{
	struct spdk_notify_type *type;

	type = TAILQ_NEXT(prev, tailq);

	return type;
}

void
spdk_notify_send(struct spdk_notify *notify, void *ctx)
{
	struct spdk_notify_client *client;

	client = TAILQ_FIRST(&g_notify_clients);
	if (client) {
		client->cb(notify, client->ctx);
		TAILQ_REMOVE(&g_notify_clients, client, tailq);
		free(client);
	}
}

int
spdk_notify_listen(spdk_notify_handler cb,
		   void *ctx)
{
	struct spdk_notify_client *client;

	client = calloc(1, sizeof(*client));
	if (client == NULL) {
		return -ENOMEM;
	}

	client->cb = cb;
	client->ctx = ctx;
	TAILQ_INSERT_TAIL(&g_notify_clients, client, tailq);
	return 0;
}

const char *
spdk_notify_get_name(struct spdk_notify *notify)
{
	return notify->type->name;
}

const char *spdk_notify_get_object(struct spdk_notify *notify)
{
	return notify->type->get_object_cb(notify->type, notify->ctx);
}

const char *spdk_notify_get_uuid(struct spdk_notify *notify)
{
	return notify->type->get_uuid_cb(notify->type, notify->ctx);
}
