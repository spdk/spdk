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
#include "event.h"
#include "spdk/json.h"

#ifdef __cplusplus
extern "C" {
#endif

struct spdk_notify;
struct spdk_notify_type;

/**
 * Callback for writing notification information into JSON write context.
 *
 * \param w JSON write context
 * \param notify SPDK notification we are talking about.
 * \param ctx notification specific data.
 */
typedef void (*spdk_notify_info)(struct spdk_json_write_ctx *w,
				 struct spdk_notify *notify, void *ctx);

typedef void (*spdk_notify_type_info)(struct spdk_json_write_ctx *w,
				      struct spdk_notify_type *type, void *ctx);

struct spdk_notify_type {
	const char *name;
	spdk_notify_info write_info_cb;
	spdk_notify_type_info write_type_cb;

	/* Internal fields */
	TAILQ_ENTRY(spdk_notify_type) tailq;
};

/**
 * Register \c type as new notification type.
 *
 * The \c type must be valid through whole program lifetime (chance being a global variable).
 *
 * \note This function is thread safe.
 *
 * \param type New notification type to register.
 */
void spdk_notify_type_register(struct spdk_notify_type *type);

/**
 * Return first registered notification type.
 *
 * This function might be used to start iterating over all registered notification types.
 *
 * \note This function is not thread safe.
 *
 * \return Pointer to first notification type or NULL if no notifications are registered.
 */
struct spdk_notify_type *spdk_notify_type_first(void);

/**
 * Return next registered notification type.
 *
 * This function might be used continue iterating over all registered notification types.
 *
 * \note This function is not thread safe.
 *
 * \param prev Pointer to the previous notification type.
 * \return Pointer to the next notification type or NULL if no more notification type available.
 */
struct spdk_notify_type *spdk_notify_type_next(struct spdk_notify_type *prev);

/**
 * Write notification type information into provided JSON write context.
 *
 * \param w JSON write context
 * \param type Notification type we are talking about.
 */
void spdk_notify_type_write_json(struct spdk_json_write_ctx *w, struct spdk_notify_type *type);

/**
 * Create new notification object using \c type as its type.
 *
 * When notification object is no longer need it is required to call \c spdk_notify_put to free
 * used resources. Failed to do so will lead to memory leaks.
 *
 * \see spdk_notify_info
 *
 * \param type Notification type
 * \param ctx Optional argument for notification type handler.
 * \return New notification object or NULL on error. In case of error \c errno variable is set.
 */
struct spdk_notify *spdk_notify_alloc(struct spdk_notify_type *type, void *ctx);

/**
 * Increment notification object reference counter.
 *
 * \see spdk_notify_put
 *
 * \param notify Notification object we are talking about.
 * \return reference counter before calling this function.
 */
unsigned spdk_notify_get(struct spdk_notify *notify);

/**
 * Decrement and possibly free notification object.
 *
 * \param notify
 * \return reference counter after calling this function. 0 means that notification
 *  object was deleted.
 */
unsigned spdk_notify_put(struct spdk_notify *notify);

/**
 * Send given notification to all clients.
 *
 * \see spdk_notify_listen
 *
 * \param notify Notification object
 */
void spdk_notify_send(struct spdk_notify *notify);

/**
 * Write notification data into JSON write context
 *
 * For documentation about data what is written see documentation of notification type.
 *
 * \param w JSON write context
 * \param notify Notification object
 */
void spdk_notify_write_json(struct spdk_json_write_ctx *w, struct spdk_notify *notify);

/**
 * Client notification callback.
 *
 * Whenever new notification is send each registered client's callback is called to inform it
 * about it. After returning from this function notification object might be destroyed. If notficiation
 * object need to be used after returning from callback function the \c spdk_notify_get function must be
 * used to increment reference counter. Each call to \c spdk_notify_get must be followed by \c spdk_notify_put
 * when notification object is no longer needed.
 *
 *
 * \param notify New notification that was sent.
 * \param ctx Callback parameter.
 */
typedef void (*spdk_notify_handler)(struct spdk_notify *notify,
				    void *ctx);

/**
 * Add client to listen for notifications.
 *
 * Pair of \c cb and \c ctx is called notification client. When notification is send each
 * registered client is walked and callback with provided context and notification is
 * called.
 *
 * \see spdk_notify_type
 *
 * \param cb Client callback function.
 * \param ctx Optional callback function parameter.
 * \return 0 on success or negative error code:
 *  -EINVAL - callback is NULL
 *  -ENOMEM - out of memory
 *  -EEXIST - pair of \c cb and \c ctx is already registered.
 */
int spdk_notify_listen(spdk_notify_handler cb, void *ctx);

/**
 * Remove registered notification client.
 *
 * After this call client (\c cb, \c ctx) won't be notified about new notifications.
 *
 * \see spdk_notify_listen
 *
 * \param cb Notification callback function.
 * \param ctx Optional notification callback function parameter.
 *  -ENOENT - pair of \c cb and \c ctx is not registered.
 */
int spdk_notify_unlisten(spdk_notify_handler cb, void *ctx);

#define SPDK_NOTIFY_TYPE_REGISTER(_type) \
static void __attribute__((constructor)) notify_type_register##_type(void) \
{ \
	struct spdk_notify_type *_ptype = &(_type); \
	rc = spdk_notify_type_register(_ptype); \
}

#ifdef __cplusplus
}
#endif

#endif /* SPDK_NOTIFY_H */
