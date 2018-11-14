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

#define SPDK_NOTIFY_MAX_CTX_SIZE 128

struct spdk_notify_type {
	const char *name;

	/* Internal fields */
	TAILQ_ENTRY(spdk_notify_type) tailq;
};

struct spdk_notify_event {
	struct spdk_notify_type *type;
	char ctx[SPDK_NOTIFY_MAX_CTX_SIZE];
};

/**
 * Initialize notifications library.
 */
void spdk_notify_initialize(void);

/**
 * Finish notifications library.
 */
void spdk_notify_finish(void);

/**
 * Register \c type as new notification type.
 *
 * The \c type must be valid through whole program lifetime (chance being a global variable).
 *
 * \note This function is thread safe.
 *
 * \param type New notification type to register.
 * \return 0 on success, negative errno code on failure.
 */
int spdk_notify_type_register(struct spdk_notify_type *type);

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
 * Send given notification to all clients.
 *
 * \see spdk_notify_listen
 *
 * \param type Notification type
 * \param ctx Notification context
 *
 * \return 0 on success, negative errno code on failure:
 *  -EAGAIN - can't send event  as library is not initialised yet.
 */
int spdk_notify_send(struct spdk_notify_type *type, const char *ctx);

#define SPDK_NOTIFY_TYPE_REGISTER(_type) \
static void __attribute__((constructor)) notify_type_register##_type(void) \
{ \
	struct spdk_notify_type *_ptype = &(_type); \
	spdk_notify_type_register(_ptype); \
}

/**
 * Return first notification in buffer.
 * \return
 */
struct spdk_notify_event *spdk_notify_event_first(void);

/**
 * Return pointer to the next notification or NULL if no more notifications available.
 *
 * \param prev
 * \return Next event type or NULL if this was last one.
 */
struct spdk_notify_event *spdk_notify_event_next(struct spdk_notify_event *prev);


#ifdef __cplusplus
}
#endif

#endif /* SPDK_NOTIFY_H */
