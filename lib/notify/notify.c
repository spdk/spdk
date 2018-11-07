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
#include "spdk/json.h"

struct spdk_notify_client {
	spdk_notify_handler cb;
	void *ctx;
	TAILQ_ENTRY(spdk_notify_client) tailq;
};
TAILQ_HEAD(spdk_notify_client_list, spdk_notify_client);

struct spdk_notify_type {
	char *name;
	void *ctx;
	spdk_notify_info write_info_cb;
	spdk_notify_type_info write_type_cb;
	TAILQ_ENTRY(spdk_notify_type) tailq;
};

struct spdk_notify {
	const struct spdk_notify_type *type;
	spdk_notify_info write_info_cb;
	void *ctx;
	uint64_t refcnt;
};


TAILQ_HEAD(spdk_notify_type_list, spdk_notify_type);

struct spdk_notify_type_list g_notify_types = TAILQ_HEAD_INITIALIZER(
			g_notify_types);

struct spdk_notify_client_list g_notify_clients = TAILQ_HEAD_INITIALIZER(
			g_notify_clients);

void
spdk_notify_type_register(const char *name, spdk_notify_type_info write_type_cb,
			  spdk_notify_info write_info_cb)
{
	struct spdk_notify_type *type;
	type = calloc(1, sizeof(struct spdk_notify_type));
	assert(type != NULL);

	type->name = strdup(name);
	type->write_info_cb = write_info_cb;
	type->write_type_cb = write_type_cb;
	TAILQ_INSERT_TAIL(&g_notify_types, type, tailq);
}

struct spdk_notify_type *
spdk_notify_type_first(void)
{
	struct spdk_notify_type *type;

	type = TAILQ_FIRST(&g_notify_types);

	return type;
}

struct spdk_notify_type *
spdk_notify_type_next(struct spdk_notify_type *prev)
{
	struct spdk_notify_type *type;

	type = TAILQ_NEXT(prev, tailq);

	return type;
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

int
spdk_notify_unlisten(spdk_notify_handler cb,
		     void *ctx)
{
	struct spdk_notify_client *client;

	TAILQ_FOREACH(client, &g_notify_clients, tailq) {
		if (client->cb == cb && client->ctx == ctx) {
			TAILQ_REMOVE(&g_notify_clients, client, tailq);
			free(client);
			return 0;
		}

	}
	return -ENOENT;
}

void
spdk_notify_send(struct spdk_notify *notify)
{
	struct spdk_notify_client *client;

	TAILQ_FOREACH(client, &g_notify_clients, tailq) {
		client->cb(notify, client->ctx);
	}
	spdk_notify_put(notify);
}

struct spdk_notify *
spdk_notify_alloc(struct spdk_notify_type *type)
{
	struct spdk_notify *notify;

	notify = calloc(1, sizeof(*notify));
	notify->type = type;
	spdk_notify_get(notify);

	return notify;
}

void
spdk_notify_get(struct spdk_notify *notify)
{
	notify->refcnt++;
}

void
spdk_notify_put(struct spdk_notify *notify)
{
	assert(notify != NULL);
	assert(notify->refcnt > 0);

	if (!(--notify->refcnt)) {
		free(notify);
	}
}

void spdk_notify_write_json(struct spdk_json_write_ctx *w, struct spdk_notify *notify)
{
	notify->write_info_cb(w, notify, notify->ctx);
}
