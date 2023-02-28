/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/log.h"

struct sigbus_handler {
	spdk_pci_error_handler func;
	void *ctx;

	TAILQ_ENTRY(sigbus_handler) tailq;
};

static pthread_mutex_t g_sighandler_mutex = PTHREAD_MUTEX_INITIALIZER;
static TAILQ_HEAD(, sigbus_handler) g_sigbus_handler =
	TAILQ_HEAD_INITIALIZER(g_sigbus_handler);

static void
sigbus_fault_sighandler(int signum, siginfo_t *info, void *ctx)
{
	struct sigbus_handler *sigbus_handler;

	pthread_mutex_lock(&g_sighandler_mutex);
	TAILQ_FOREACH(sigbus_handler, &g_sigbus_handler, tailq) {
		sigbus_handler->func(info->si_addr, sigbus_handler->ctx);
	}
	pthread_mutex_unlock(&g_sighandler_mutex);
}

__attribute__((constructor)) static void
device_set_signal(void)
{
	struct sigaction sa;

	sa.sa_sigaction = sigbus_fault_sighandler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGBUS, &sa, NULL);
}

__attribute__((destructor)) static void
device_destroy_signal(void)
{
	struct sigbus_handler *sigbus_handler, *tmp;

	TAILQ_FOREACH_SAFE(sigbus_handler, &g_sigbus_handler, tailq, tmp) {
		free(sigbus_handler);
	}
}

int
spdk_pci_register_error_handler(spdk_pci_error_handler sighandler, void *ctx)
{
	struct sigbus_handler *sigbus_handler;

	if (!sighandler) {
		SPDK_ERRLOG("Error handler is NULL\n");
		return -EINVAL;
	}

	pthread_mutex_lock(&g_sighandler_mutex);
	TAILQ_FOREACH(sigbus_handler, &g_sigbus_handler, tailq) {
		if (sigbus_handler->func == sighandler) {
			pthread_mutex_unlock(&g_sighandler_mutex);
			SPDK_ERRLOG("Error handler has been registered\n");
			return -EINVAL;
		}
	}
	pthread_mutex_unlock(&g_sighandler_mutex);

	sigbus_handler = calloc(1, sizeof(*sigbus_handler));
	if (!sigbus_handler) {
		SPDK_ERRLOG("Failed to allocate sigbus handler\n");
		return -ENOMEM;
	}

	sigbus_handler->func = sighandler;
	sigbus_handler->ctx = ctx;

	pthread_mutex_lock(&g_sighandler_mutex);
	TAILQ_INSERT_TAIL(&g_sigbus_handler, sigbus_handler, tailq);
	pthread_mutex_unlock(&g_sighandler_mutex);

	return 0;
}

void
spdk_pci_unregister_error_handler(spdk_pci_error_handler sighandler)
{
	struct sigbus_handler *sigbus_handler;

	if (!sighandler) {
		return;
	}

	pthread_mutex_lock(&g_sighandler_mutex);
	TAILQ_FOREACH(sigbus_handler, &g_sigbus_handler, tailq) {
		if (sigbus_handler->func == sighandler) {
			TAILQ_REMOVE(&g_sigbus_handler, sigbus_handler, tailq);
			free(sigbus_handler);
			pthread_mutex_unlock(&g_sighandler_mutex);
			return;
		}
	}
	pthread_mutex_unlock(&g_sighandler_mutex);
}
