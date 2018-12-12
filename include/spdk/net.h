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

/** \file
 * Net framework abstraction layer
 */

#ifndef SPDK_NET_H
#define SPDK_NET_H

#include "spdk/stdinc.h"

#include "spdk/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

struct spdk_sock;

struct spdk_net_framework {
	const char *name;

	void (*init)(void);
	void (*fini)(void);

	STAILQ_ENTRY(spdk_net_framework) link;
};

/**
 * Register a net framework.
 *
 * \param frame Net framework to register.
 */
void spdk_net_framework_register(struct spdk_net_framework *frame);

#define SPDK_NET_FRAMEWORK_REGISTER(name, frame) \
static void __attribute__((constructor)) net_framework_register_##name(void) \
{ \
	spdk_net_framework_register(frame); \
}

/**
 * Initialize the network interfaces by getting information through netlink socket.
 *
 * \return 0 on success, 1 on failure.
 */
int spdk_interface_init(void);

/**
 * Destroy the network interfaces.
 */
void spdk_interface_destroy(void);

/**
 * Net framework initialization callback.
 *
 * \param cb_arg Callback argument.
 * \param rc 0 if net framework initialized successfully or negative errno if it failed.
 */
typedef void (*spdk_net_init_cb)(void *cb_arg, int rc);

/**
 * Net framework finish callback.
 *
 * \param cb_arg Callback argument.
 */
typedef void (*spdk_net_fini_cb)(void *cb_arg);

void spdk_net_framework_init_next(int rc);

/**
 * Start all registered frameworks.
 *
 * \return 0 on success.
 */
void spdk_net_framework_start(spdk_net_init_cb cb_fn, void *cb_arg);

void spdk_net_framework_fini_next(void);

/**
 * Stop all registered frameworks.
 */
void spdk_net_framework_fini(spdk_net_fini_cb cb_fn, void *cb_arg);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_NET_H */
