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

#define SPDK_NOTIFY_MAX_EVENTS		1024
/* Data queue is larger to store also events that are being processed */
#define SPDK_NOTIFY_MAX_EVENTS_DATA	(SPDK_NOTIFY_MAX_EVENTS * 2)

struct spdk_notify_type {
	char name[SPDK_NOTIFY_MAX_NAME_SIZE];
	TAILQ_ENTRY(spdk_notify_type) tailq;
};

pthread_mutex_t g_events_lock = PTHREAD_MUTEX_INITIALIZER;

static struct event_queue {
	/* Data queue */
	volatile int64_t in_tail_alloc;
	volatile int64_t in_tail_pending;
	struct spdk_notify_event in[SPDK_NOTIFY_MAX_EVENTS_DATA];
	/* Event queue */
	volatile int64_t out_tail_alloc;
	volatile int64_t out_tail;
	struct spdk_notify_event *out[SPDK_NOTIFY_MAX_EVENTS];
} g_queue;

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
			SPDK_ERRLOG("Notification type '%s' already registered.\n", type);
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

	uint64_t in_tail_pending = __sync_add_and_fetch(&g_queue.in_tail_pending, 1);
	if (in_tail_pending > SPDK_NOTIFY_MAX_EVENTS_DATA - SPDK_NOTIFY_MAX_EVENTS) {
		/* If number of actually processed events is larger than the
		 * data queue back buffer, let producer to retry later or drop event
		 */
		__sync_sub_and_fetch(&g_queue.in_tail_pending, 1);
		return 0; /* -EAGAIN ? */
	}

	/* Allocate space for data and copy it to the data queue */
	uint64_t in_tail_alloc = __sync_fetch_and_add(&g_queue.in_tail_alloc, 1);
	ev = &g_queue.in[in_tail_alloc % SPDK_NOTIFY_MAX_EVENTS_DATA];
	spdk_strcpy_pad(ev->type, type, sizeof(ev->type), '\0');
	spdk_strcpy_pad(ev->ctx, ctx, sizeof(ev->ctx), '\0');

	/* Now put the pointer from data queue to the output queue */
	head = __sync_fetch_and_add(&g_queue.out_tail_alloc, 1);
	g_queue.out[head % SPDK_NOTIFY_MAX_EVENTS] = ev;

	/* Now we know that all data is valid */
	__sync_fetch_and_add(&g_queue.out_tail, 1);
	__sync_sub_and_fetch(&g_queue.in_tail_pending, 1);

	return head;
}

static int
_spdk_event_get(struct spdk_notify_event *msg, int64_t index)
{
	struct spdk_notify_event *m;
	int64_t out_tail;

	out_tail = __sync_fetch_and_add(&g_queue.out_tail, 0);
	if (index >= out_tail) {
		/* Message is not available yet, so try another time */
		return -EAGAIN;
	}
	if (index < out_tail - SPDK_NOTIFY_MAX_EVENTS - 1) {
		/* Message is not available anymore */
		return -1;
	}

	/* Get the pointer to data */
	m = __sync_fetch_and_add(&g_queue.out[index % SPDK_NOTIFY_MAX_EVENTS], 0);
	if (m == NULL) {
		/* This should never happen */
		return -1;
	}
	/* We need to copy event here to prevent data corruption */
	memcpy(msg, m, sizeof(struct spdk_notify_event));

	/* Revalidate data to make sure it wasn't overwritten meantime */
	out_tail = __sync_fetch_and_add(&g_queue.out_tail, 0);
	if (index < out_tail - SPDK_NOTIFY_MAX_EVENTS - 1) {
		return -1;
	}

	return 0;
}

uint64_t
spdk_notify_foreach_event(uint64_t start_idx, uint64_t max,
			  spdk_notify_foreach_event_cb cb_fn, void *ctx)
{
	uint64_t i;
	struct spdk_notify_event ev;

	for (i = 0; i < max; start_idx++, i++) {

		if (_spdk_event_get(&ev, start_idx)) {
			break;
		}

		if (cb_fn(start_idx, &ev, ctx)) {
			break;
		}
	}

	return i;
}
