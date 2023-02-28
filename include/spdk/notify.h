/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
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
