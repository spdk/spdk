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

#include <sys/queue.h>

#include "spdk/stdinc.h"
#include "spdk/util.h"
#include "spdk/queue.h"
#include "spdk/string.h"
#include "spdk/log.h"

#include "spdk/notify.h"

#define SPDK_NOTIFY_MAX_EVENTS	1024

struct spdk_notify_type {
	char name[SPDK_NOTIFY_MAX_NAME_SIZE];
	TAILQ_ENTRY(spdk_notify_type) tailq;
};

static struct spdk_notify_event g_events[SPDK_NOTIFY_MAX_EVENTS];
static uint64_t g_events_head;

static TAILQ_HEAD(, spdk_notify_type) g_notify_types = TAILQ_HEAD_INITIALIZER(g_notify_types);

struct spdk_notify_type *
spdk_notify_type_register(const char *type)
{
	struct spdk_notify_type *it;

	if (!type) {
		SPDK_ERRLOG("Invalid notification type %p\n", type);
		return NULL;
	} else if (!type || strlen(type) >= SPDK_NOTIFY_MAX_NAME_SIZE) {
		SPDK_ERRLOG("Invalid notification type (add: %p, name: %s)\n", type,
			    type ? type : "(null)");
		return NULL;
	}

	TAILQ_FOREACH(it, &g_notify_types, tailq) {
		if (strcmp(type, it->name) == 0) {
			SPDK_ERRLOG("Notification type '%s' already registered.\n", type);
			return NULL;
		}
	}

	it = calloc(1, sizeof(*it));
	if (it == NULL) {
		return NULL;
	}

	snprintf(it->name, sizeof(it->name), "%s", type);
	TAILQ_INSERT_TAIL(&g_notify_types, it, tailq);
	return it;
}

const char *
spdk_notify_type_get_name(struct spdk_notify_type *type)
{
	return type->name;
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

int
spdk_notify_send(const char *type, const char *ctx)
{
	int head = __sync_fetch_and_add(&g_events_head, 1);
	struct spdk_notify_event *ev = &g_events[head % SPDK_NOTIFY_MAX_EVENTS];

	/* TODO: thread safty */
	spdk_strcpy_pad(ev->type, type, sizeof(ev->type), '\0');
	spdk_strcpy_pad(ev->ctx, ctx, sizeof(ev->ctx), '\0');

	return head;
}

const struct spdk_notify_event *
spdk_notify_get_event(uint64_t idx)
{
	/* TODO: thread safty */
	if (idx >= g_events_head) {
		return NULL;
	}

	return &g_events[idx % SPDK_NOTIFY_MAX_EVENTS];
}
