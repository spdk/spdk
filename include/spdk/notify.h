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

#ifndef SPDK_NOTIFY_H
#define SPDK_NOTIFY_H

#include "spdk/stdinc.h"
#include "spdk/json.h"
#include "spdk/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Opaque event type.
 */
struct spdk_notify_type;

typedef int (*spdk_notify_foreach_type_cb)(const struct spdk_notify_type *type, void *ctx);

#define SPDK_NOTIFY_MAX_NAME_SIZE 128
#define SPDK_NOTIFY_MAX_CTX_SIZE 128

struct spdk_notify_event {
	char type[SPDK_NOTIFY_MAX_NAME_SIZE];
	char ctx[SPDK_NOTIFY_MAX_CTX_SIZE];
};

/**
 * Callback type for event enumeration.
 *
 * \param idx Event index
 * \param event Event data
 * \param ctx User context
 * \return Non zero to break iteration.
 */
typedef int (*spdk_notify_foreach_event_cb)(uint64_t idx, const struct spdk_notify_event *event,
		void *ctx);

/**
 * Register \c type as new notification type.
 *
 * \note This function is thread safe.
 *
 * \param type New notification type to register.
 * \return registered notification type or NULL on failure.
 */
struct spdk_notify_type *spdk_notify_type_register(const char *type);

/**
 * Return name of the notification type.
 *
 * \param type Notification type we are talking about.
 * \return Name of notification type.
 */
const char *spdk_notify_type_get_name(const struct spdk_notify_type *type);

/**
 * Call cb_fn for all event types.
 *
 * \note Whole function call is under lock so user callback should not sleep.
 * \param cb_fn
 * \param ctx
 */
void spdk_notify_foreach_type(spdk_notify_foreach_type_cb cb_fn, void *ctx);

/**
 * Send given notification.
 *
 * \param type Notification type
 * \param ctx Notification context
 *
 * \return Event index.
 */
uint64_t spdk_notify_send(const char *type, const char *ctx);

/**
 * Call cb_fn with events from given range.
 *
 * \note Whole function call is under lock so user callback should not sleep.
 *
 * \param start_idx First event index
 * \param cb_fn User callback function. Return non-zero to break iteration.
 * \param max Maximum number of invocations of user callback function.
 * \param ctx User context
 * \return Number of user callback invocations
 */
uint64_t spdk_notify_foreach_event(uint64_t start_idx, uint64_t max,
				   spdk_notify_foreach_event_cb cb_fn, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_NOTIFY_H */
