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

#ifndef SPDK_RPC_CONFIG_H_
#define SPDK_RPC_CONFIG_H_

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

struct spdk_notification;

/**
 * Function signature for notification handlers.
 *
 * \param notification handler to notification
 * \param ctx context
 */
typedef void (*spdk_notification_handler)(struct spdk_notification *notification,
		void *ctx);


struct spdk_notification_type {
	const char *module;
	const char *name;
	spdk_notification_handler fn;
};


/** Notification */
struct spdk_notification {
	struct spdk_notification_type type;
	char *object;
	char *uuid;
};

/**
 * This call provides ability to register notification type
 */
void spdk_add_notification(struct spdk_notification_type *notification_type);

/**
 * Send notification to all registered listeners
 */
void spdk_send_notification(struct spdk_notification *notification);


/**
 * Register for notifications
 */
void spdk_get_notificiation_types(struct spdk_notification **notifications, int *cnt);

/**
 * Register for notifications
 */
void spdk_notification_register(struct spdk_notification_type *notification_type,
				spdk_notification_handler cb,
				void *ctx);

void spdk_notification_register_all(spdk_notification_handler cb,
				    void *ctx);

void spdk_notification_register_module(spdk_notification_handler cb,
				       void *ctx);


#ifdef __cplusplus
}
#endif

#endif
