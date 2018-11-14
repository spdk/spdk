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
#include "spdk/string.h"

struct spdk_notify {
	const struct spdk_notify_type *type;
	void *ctx;

	int refcnt;
};

struct spdk_notify_client {
	spdk_notify_handler cb;
	void *ctx;

	bool unregistered;
	bool in_call;
	TAILQ_ENTRY(spdk_notify_client) tailq;
};

static TAILQ_HEAD(, spdk_notify_type) g_notify_types = TAILQ_HEAD_INITIALIZER(g_notify_types);
static TAILQ_HEAD(, spdk_notify_client) g_notify_clients = TAILQ_HEAD_INITIALIZER(g_notify_clients);

/*
 * Protects lists of notifications and clients
 */
pthread_mutex_t g_notify_mutex = PTHREAD_MUTEX_INITIALIZER;

static int
spdk_notify_type_register_unsafe(struct spdk_notify_type *type)
{
	struct spdk_notify_type *it;
	TAILQ_FOREACH(it, &g_notify_types, tailq) {
		if (strcmp(type->name, it->name) == 0) {
			SPDK_ERRLOG("Notification type '%s' already registered.\n", type->name);
			return -EEXIST;
		}
	}

	TAILQ_INSERT_TAIL(&g_notify_types, type, tailq);
	return 0;
}

void
spdk_notify_type_register(struct spdk_notify_type *type)
{
	int rc;

	if (!type) {
		SPDK_ERRLOG("Invalid notification type %p\n", type);
		assert(type != NULL);
	} else if (!type->name || !type->write_info_cb || !type->write_type_cb) {
		SPDK_ERRLOG("Invalid notification type (add: %p, name: %s)\n", type,
			    type->name ? type->name : "(null)");
		assert(type->name != NULL);
		assert(type->write_info_cb != NULL);
		assert(type->write_type_cb != NULL);
	}

	pthread_mutex_lock(&g_notify_mutex);
	rc = spdk_notify_type_register_unsafe(type);
	pthread_mutex_unlock(&g_notify_mutex);
	if (rc) {
		SPDK_ERRLOG("Failed to register notification type (%d): %s\n", rc, spdk_strerror(-rc));
		assert(rc == 0);
	}
}

struct spdk_notify_type *
spdk_notify_type_first(void)
{
	return TAILQ_FIRST(&g_notify_types);
}

struct spdk_notify_type *
spdk_notify_type_next(struct spdk_notify_type *prev)
{
	return TAILQ_NEXT(prev, tailq);
}

void
spdk_notify_type_write_json(struct spdk_json_write_ctx *w, struct spdk_notify_type *type)
{
	spdk_json_write_string(w, type->name);
}

static int
spdk_notify_listen_unsafe(spdk_notify_handler cb,
			  void *ctx)
{
	struct spdk_notify_client *client;

	if (cb == NULL) {
		return -EINVAL;
	}

	TAILQ_FOREACH(client, &g_notify_clients, tailq) {
		if (client->cb == cb && client->ctx == ctx) {
			SPDK_ERRLOG("Notification client (cb: %p, ctx: %p) already registered.\n", cb, ctx);
			return -EEXIST;
		}
	}


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
spdk_notify_listen(spdk_notify_handler cb,
		   void *ctx)
{
	int rc = 0;

	pthread_mutex_lock(&g_notify_mutex);
	rc = spdk_notify_listen_unsafe(cb, ctx);
	pthread_mutex_unlock(&g_notify_mutex);

	return rc;
}

static int
spdk_notify_unlisten_unsafe(spdk_notify_handler cb, void *ctx)
{
	struct spdk_notify_client *client;

	TAILQ_FOREACH(client, &g_notify_clients, tailq) {
		if (client->cb == cb && client->ctx == ctx) {
			TAILQ_REMOVE(&g_notify_clients, client, tailq);
			free(client);
			return 0;
		}

	}

	SPDK_ERRLOG("Notification client (cb: %p, ctx: %p) not registered.\n", cb, ctx);
	return -ENOENT;
}

int
spdk_notify_unlisten(spdk_notify_handler cb, void *ctx)
{
	int rc;

	pthread_mutex_lock(&g_notify_mutex);
	rc = spdk_notify_unlisten_unsafe(cb, ctx);
	pthread_mutex_unlock(&g_notify_mutex);

	return rc;
}


void
spdk_notify_send(struct spdk_notify *notify)
{
	struct spdk_notify_client *client;

	pthread_mutex_lock(&g_notify_mutex);
	TAILQ_FOREACH(client, &g_notify_clients, tailq) {
		client->in_call = true;
		pthread_mutex_unlock(&g_notify_mutex);

		client->cb(notify, client->ctx);

		pthread_mutex_lock(&g_notify_mutex);
		client->in_call = false;


	}
	pthread_mutex_unlock(&g_notify_mutex);

	spdk_notify_put(notify);
}

struct spdk_notify *
spdk_notify_alloc(struct spdk_notify_type *type, void *ctx)
{
	struct spdk_notify *notify;

	notify = calloc(1, sizeof(*notify));
	if (notify == NULL) {
		/* TODO: log error */
		return NULL;
	}

	notify->type = type;
	notify->ctx = ctx;

	/* Bump refcnt */
	spdk_notify_get(notify);
	return notify;
}

unsigned
spdk_notify_get(struct spdk_notify *notify)
{
	int ret = __sync_fetch_and_add(&notify->refcnt, 1);

	assert(ret >= 0 && ret != INT_MAX);
	return ret;

}

unsigned
spdk_notify_put(struct spdk_notify *notify)
{
	int ret;

	assert(notify != NULL);
	ret = __sync_sub_and_fetch(&notify->refcnt, 1);

	assert(ret >= 0);
	if (ret == 0) {
		free(notify);
	}
	return ret;
}

void spdk_notify_write_json(struct spdk_json_write_ctx *w, struct spdk_notify *notify)
{
	notify->type->write_info_cb(w, notify, notify->ctx);
}
