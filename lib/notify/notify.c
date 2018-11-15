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

void
spdk_register_notify_type(const char *type, spdk_notify_handler cb)
{
	struct spdk_notify_type *ntype;
	ntype = calloc(1, sizeof(struct spdk_notify_type));
	assert(ntype != NULL);

	ntype->name = strdup(type);
	assert(ntype->name != NULL);

	TAILQ_INIT(&ntype->clients);
	TAILQ_INSERT_TAIL(&g_notify_types, ntype, tailq);
	g_notify_types_count++;
}

int
spdk_get_notificiation_types(const char **notifys, size_t *count)
{

	struct spdk_notify_type *ntype;
	size_t i = 0;

	if (notifys == NULL) {
		*count = g_notify_types_count;
		return -ENOMEM;
	}

	TAILQ_FOREACH(ntype, &g_notify_types, tailq) {
		if (i >= *count) {
			return -ENOMEM;
		}
		notifys[i++] = ntype->name;
	}
	return 0;
}

void
spdk_send_notify(const char *name, void *ctx)
{
	struct spdk_notify_client *client;
	struct spdk_notify_type *ntype;

	TAILQ_FOREACH(ntype, &g_notify_types, tailq) {
		if (!strcmp(ntype->name, name)) {
			client = TAILQ_FIRST(&ntype->clients);
			if (client) {
				client->cb(ntype, client->ctx);
				TAILQ_REMOVE(&ntype->clients, client, tailq);
			}
		}
	}
}

int
spdk_notify_listen(const char *name,
		   spdk_notify_handler cb,
		   void *ctx)
{
	struct spdk_notify_client *client;
	struct spdk_notify_type *ntype;
	TAILQ_FOREACH(ntype, &g_notify_types, tailq) {
		if (name == NULL || !strcmp(ntype->name, name)) {
			client = calloc(1, sizeof(*client));
			if (client == NULL) {
				return -ENOMEM;
			}
			client->cb = cb;
			client->ctx = ctx;
			TAILQ_INSERT_TAIL(&ntype->clients, client, tailq);
			return 0;
		}
	}
	return -ENOENT;
}
