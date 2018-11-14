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

#define SPDK_NOTIFY_MAX_NAME_SIZE 128
#define SPDK_NOTIFY_MAX_CTX_SIZE 128

struct spdk_notify_event {
	char type[SPDK_NOTIFY_MAX_NAME_SIZE];
	char ctx[SPDK_NOTIFY_MAX_CTX_SIZE];
};

/**
 * Register \c type as new notification type.
 *
 * The \c type must be valid through whole program lifetime (chance being a global variable).
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
const char *spdk_notify_type_get_name(struct spdk_notify_type *type);

/**
 * Return first registered notification type.
 *
 * This function might be used to start iterating over all registered notification types.
 *
 * \return Pointer to first notification type or NULL if no notifications are registered.
 */
struct spdk_notify_type *spdk_notify_type_first(void);

/**
 * Return next registered notification type.
 *
 * This function might be used continue iterating over all registered notification types.
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
 * \return Event index.
 */
uint64_t spdk_notify_send(const char *type, const char *ctx);

/**
 * Return first notification in buffer.
 * \return
 */
const struct spdk_notify_event *spdk_notify_get_event(uint64_t idx);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_NOTIFY_H */
