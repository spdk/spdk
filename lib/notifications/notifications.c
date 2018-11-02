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
#include "spdk/notifications.h"
#include "spdk/util.h"
#include "spdk/queue.h"

static size_t g_notification_types_count = 0;
struct spdk_notification_type_list g_notification_types = TAILQ_HEAD_INITIALIZER(
			g_notification_types);

void
spdk_register_notification_type(struct spdk_notification_type *ntype)
{
	TAILQ_INIT(&ntype->clients);
	TAILQ_INSERT_TAIL(&g_notification_types, ntype, tailq);
	g_notification_types_count++;
}

int
spdk_get_notificiation_types(const char **notifications, size_t *count)
{

	struct spdk_notification_type *ntype;
	size_t i = 0;

	if (notifications == NULL) {
		*count = g_notification_types_count;
		return -ENOMEM;
	}

	TAILQ_FOREACH(ntype, &g_notification_types, tailq) {
		if (i >= *count) {
			return -ENOMEM;
		}
		notifications[i++] = ntype->name;
	}
	return 0;
}

void
spdk_send_notification(struct spdk_notification_type *ntype, void *ctx)
{
	struct spdk_notification_client *client, *tmp;
	struct spdk_notification notification;

	notification.type = ntype;

	TAILQ_FOREACH_SAFE(client, &ntype->clients, tailq, tmp) {
		client->cb(&notification, client->ctx);
	}
}

int
spdk_notification_listen(const char *name,
			 spdk_notification_handler cb,
			 void *ctx)
{
	struct spdk_notification_client *client;
	struct spdk_notification_type *ntype;

	client = calloc(1, sizeof(*client));
	if (client == NULL) {
		return -ENOMEM;
	}
	client->cb = cb;
	client->ctx = ctx;

	TAILQ_FOREACH(ntype, &g_notification_types, tailq) {
		if (!strcmp(ntype->name, name)) {
			TAILQ_INSERT_TAIL(&ntype->clients, client, tailq);
			return 0;
		}
	}
	return -ENOENT;
}

int
spdk_notification_stop(const char *name,
		       spdk_notification_handler cb)
{
	struct spdk_notification_client *client, *tmp;
	struct spdk_notification_type *ntype;

	TAILQ_FOREACH(ntype, &g_notification_types, tailq) {
		if (!strcmp(ntype->name, name)) {
			TAILQ_FOREACH_SAFE(client, &ntype->clients, tailq, tmp) {
				if (client->cb == cb) {
					TAILQ_REMOVE(&ntype->clients, client, tailq);
					free(client);
					return 0;
				}
			}
		}
	}
	return -ENOENT;
}
