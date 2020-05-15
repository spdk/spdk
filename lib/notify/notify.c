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

pthread_mutex_t g_events_lock = PTHREAD_MUTEX_INITIALIZER;
static struct spdk_notify_event g_events[SPDK_NOTIFY_MAX_EVENTS];
static uint64_t g_events_head;

static TAILQ_HEAD(, spdk_notify_type) g_notify_types = TAILQ_HEAD_INITIALIZER(g_notify_types);

struct spdk_notify_type *
spdk_notify_type_register(const char *type)
{
	struct spdk_notify_type *it = NULL;

	if (!type) {
		SPDK_ERRLOG("Invalid notification type %p\n", type);
		return NULL;
	} else if (!type[0] || strlen(type) >= SPDK_NOTIFY_MAX_NAME_SIZE) {
		SPDK_ERRLOG("Notification type '%s' too short or too long\n", type);
		return NULL;
	}

	pthread_mutex_lock(&g_events_lock);
	TAILQ_FOREACH(it, &g_notify_types, tailq) {
		if (strcmp(type, it->name) == 0) {
			SPDK_NOTICELOG("Notification type '%s' already registered.\n", type);
			goto out;
		}
	}

	it = calloc(1, sizeof(*it));
	if (it == NULL) {
		goto out;
	}

	snprintf(it->name, sizeof(it->name), "%s", type);
	TAILQ_INSERT_TAIL(&g_notify_types, it, tailq);

out:
	pthread_mutex_unlock(&g_events_lock);
	return it;
}

const char *
spdk_notify_type_get_name(const struct spdk_notify_type *type)
{
	return type->name;
}


void
spdk_notify_foreach_type(spdk_notify_foreach_type_cb cb, void *ctx)
{
	struct spdk_notify_type *it;

	pthread_mutex_lock(&g_events_lock);
	TAILQ_FOREACH(it, &g_notify_types, tailq) {
		if (cb(it, ctx)) {
			break;
		}
	}
	pthread_mutex_unlock(&g_events_lock);
}

uint64_t
spdk_notify_send(const char *type, const char *ctx)
{
	uint64_t head;
	struct spdk_notify_event *ev;

	pthread_mutex_lock(&g_events_lock);
	head = g_events_head;
	g_events_head++;

	ev = &g_events[head % SPDK_NOTIFY_MAX_EVENTS];
	spdk_strcpy_pad(ev->type, type, sizeof(ev->type), '\0');
	spdk_strcpy_pad(ev->ctx, ctx, sizeof(ev->ctx), '\0');
	pthread_mutex_unlock(&g_events_lock);

	return head;
}

uint64_t
spdk_notify_foreach_event(uint64_t start_idx, uint64_t max,
			  spdk_notify_foreach_event_cb cb_fn, void *ctx)
{
	uint64_t i;

	pthread_mutex_lock(&g_events_lock);

	if (g_events_head > SPDK_NOTIFY_MAX_EVENTS && start_idx < g_events_head - SPDK_NOTIFY_MAX_EVENTS) {
		start_idx = g_events_head - SPDK_NOTIFY_MAX_EVENTS;
	}

	for (i = 0; start_idx < g_events_head && i < max; start_idx++, i++) {
		if (cb_fn(start_idx, &g_events[start_idx % SPDK_NOTIFY_MAX_EVENTS], ctx)) {
			break;
		}
	}
	pthread_mutex_unlock(&g_events_lock);

	return i;
}
