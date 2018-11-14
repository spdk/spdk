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
#include "spdk/log.h"

#include <sys/queue.h>

#define SPDK_NOTIFY_MAX_EVENTS	1024

struct spdk_notify_event_entry {
	struct spdk_notify_event event;
	TAILQ_ENTRY(spdk_notify_event_entry) tailq;
};
struct spdk_notify_event_entry g_events[SPDK_NOTIFY_MAX_EVENTS];

static TAILQ_HEAD(, spdk_notify_type) g_notify_types = TAILQ_HEAD_INITIALIZER(g_notify_types);
static TAILQ_HEAD(spdk_notify_event_entry_list,
		  spdk_notify_event_entry) g_notifications = TAILQ_HEAD_INITIALIZER(g_notifications);

void
spdk_notify_initialize(void)
{
	size_t i;

	assert(TAILQ_EMPTY(&g_notifications));

	for (i = 0; i < SPDK_NOTIFY_MAX_EVENTS; i++) {
		TAILQ_INSERT_HEAD(&g_notifications, &g_events[i], tailq);
	}
}

void
spdk_notify_finish(void)
{
	TAILQ_INIT(&g_notifications);
}


int
spdk_notify_type_register(struct spdk_notify_type *type)
{
	struct spdk_notify_type *it;

	if (!type) {
		SPDK_ERRLOG("Invalid notification type %p\n", type);
		return -EINVAL;
	} else if (!type->name) {
		SPDK_ERRLOG("Invalid notification type (add: %p, name: %s)\n", type,
			    type->name ? type->name : "(null)");
		return -EINVAL;
	}

	TAILQ_FOREACH(it, &g_notify_types, tailq) {
		if (strcmp(type->name, it->name) == 0) {
			SPDK_ERRLOG("Notification type '%s' already registered.\n", type->name);
			return -EEXIST;
		}
	}

	TAILQ_INSERT_TAIL(&g_notify_types, type, tailq);
	return 0;
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
spdk_notify_send(struct spdk_notify_type *type, const char *ctx)
{
	struct spdk_notify_event_entry *ev = TAILQ_LAST(&g_notifications, spdk_notify_event_entry_list);

	if (!ev) {
		return -EAGAIN;
	}

	TAILQ_REMOVE(&g_notifications, ev, tailq);
	snprintf(ev->event.ctx, sizeof(ev->event.ctx), "%s", ctx);
	TAILQ_INSERT_HEAD(&g_notifications, ev, tailq);

	return 0;
}

struct spdk_notify_event *
spdk_notify_event_first(void)
{
	struct spdk_notify_event_entry *ev = TAILQ_FIRST(&g_notifications);
	return &ev->event;
}

struct spdk_notify_event *
spdk_notify_event_next(struct spdk_notify_event *prev)
{
	struct spdk_notify_event_entry *prev_ev, *next_ev;

	if (prev == NULL) {
		return NULL;
	}

	prev_ev = SPDK_CONTAINEROF(prev, struct spdk_notify_event_entry, event);
	next_ev = TAILQ_NEXT(prev_ev, tailq);;

	return next_ev ? &next_ev->event : NULL;
}

