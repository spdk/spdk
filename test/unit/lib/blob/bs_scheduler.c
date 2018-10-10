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
