/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/thread.h"

bool g_scheduler_delay = false;

struct scheduled_ops {
	spdk_msg_fn	fn;
	void		*ctx;

	TAILQ_ENTRY(scheduled_ops)	ops_queue;
};

static TAILQ_HEAD(, scheduled_ops) g_scheduled_ops = TAILQ_HEAD_INITIALIZER(g_scheduled_ops);

void _bs_flush_scheduler(uint32_t);

static void
_bs_send_msg(spdk_msg_fn fn, void *ctx, void *thread_ctx)
{
	if (g_scheduler_delay) {
		struct scheduled_ops *ops = calloc(1, sizeof(*ops));

		SPDK_CU_ASSERT_FATAL(ops != NULL);
		ops->fn = fn;
		ops->ctx = ctx;
		TAILQ_INSERT_TAIL(&g_scheduled_ops, ops, ops_queue);

	} else {
		fn(ctx);
	}
}

static void
_bs_flush_scheduler_single(void)
{
	struct scheduled_ops *op;
	TAILQ_HEAD(, scheduled_ops) ops;
	TAILQ_INIT(&ops);

	TAILQ_SWAP(&g_scheduled_ops, &ops, scheduled_ops, ops_queue);

	while (!TAILQ_EMPTY(&ops)) {
		op = TAILQ_FIRST(&ops);
		TAILQ_REMOVE(&ops, op, ops_queue);

		op->fn(op->ctx);
		free(op);
	}
}

void
_bs_flush_scheduler(uint32_t n)
{
	while (n--) {
		_bs_flush_scheduler_single();
	}
}
