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

#include "spdk/stdinc.h"
#include "spdk/thread.h"

#include "utils.h"

static char *cache_modes[ocf_cache_mode_max] = {
	[ocf_cache_mode_wt] = "wt",
	[ocf_cache_mode_wb] = "wb",
	[ocf_cache_mode_wa] = "wa",
	[ocf_cache_mode_pt] = "pt",
	[ocf_cache_mode_wi] = "wi"
};

ocf_cache_mode_t
ocf_get_cache_mode(const char *cache_mode)
{
	int i;

	for (i = 0; i < ocf_cache_mode_max; i++) {
		if (strcmp(cache_mode, cache_modes[i]) == 0) {
			return i;
		}
	}

	return ocf_cache_mode_none;
}

const char *
ocf_get_cache_modename(ocf_cache_mode_t mode)
{
	if (mode > ocf_cache_mode_none && mode < ocf_cache_mode_max) {
		return cache_modes[mode];
	} else {
		return NULL;
	}
}

struct spdk_cont_poller {
	struct spdk_poller            *poller;
	int                            period;
	bool                           done;
	cont_poller_fn                 fn;
	bool                           is_poller;
	int                            status;
	void                          *ctx;

	/* This structure is a tree, meaning that
	 * every poller have list of pollers, and
	 * it iself an element of such list. */
	TAILQ_HEAD(, spdk_cont_poller) continuations;
	TAILQ_ENTRY(spdk_cont_poller)  tailq;

	struct spdk_cont_poller       *parent;
};

static void poller_done_iter(struct spdk_cont_poller *arg);

int
spdk_cont_poller_parent_status(struct spdk_cont_poller *current)
{
	if (current->parent) {
		return current->parent->status;
	} else {
		return 0;
	}
}

static int
cont_poller_poll(void *opaque)
{
	struct spdk_cont_poller *arg = opaque;

	arg->done = true;
	arg->status = arg->fn(arg, arg->ctx);

	if (arg->done) {
		spdk_poller_unregister(&arg->poller);
		poller_done_iter(arg);
		return 1;
	}

	return 0;
}

int
spdk_cont_poller_repeat(struct spdk_cont_poller *poller)
{
	assert(poller->is_poller);
	if (poller->is_poller) {
		poller->done = false;
	}
	return 0;
}

static void
procedure_callback(void *ctx)
{
	struct spdk_cont_poller *current = ctx;

	if (current->fn) {
		current->status = current->fn(current, current->ctx);
	}

	poller_done_iter(current);
}

static void
start(struct spdk_cont_poller *current)
{
	if (current->is_poller) {
		current->poller = spdk_poller_register(cont_poller_poll, current, current->period);
		if (current->poller == NULL) {
			SPDK_ERRLOG("Could not register a poller\n");
			poller_done_iter(current);
			return;
		}
	} else {
		spdk_thread_send_msg(spdk_get_thread(), procedure_callback, current);
	}
}

static void
poller_done_iter(struct spdk_cont_poller *arg)
{
	struct spdk_cont_poller *current = TAILQ_FIRST(&arg->continuations);
	if (current) {
		TAILQ_REMOVE(&arg->continuations, current, tailq);
		start(current);
	} else {
		if (arg->parent) {
			poller_done_iter(arg->parent);
		}
		free(arg);
	}
}

static struct spdk_cont_poller *
init(struct spdk_cont_poller *parent, bool is_poller, cont_poller_fn fn, void *ctx, int period)
{
	struct spdk_cont_poller *ret;

	ret = malloc(sizeof(*ret));
	if (ret == NULL) {
		return NULL;
	}

	ret->fn            = fn;
	ret->period        = period;
	ret->is_poller     = is_poller;
	ret->ctx           = ctx;
	ret->parent        = parent;
	ret->status        = 0;
	TAILQ_INIT(&ret->continuations);

	if (parent) {
		TAILQ_INSERT_TAIL(&parent->continuations, ret, tailq);
	}

	return ret;
}

struct spdk_cont_poller *
spdk_cont_poller_register(cont_poller_fn fn, void *ctx, int period)
{
	struct spdk_cont_poller *arg;

	if (fn == NULL || period < 0) {
		return NULL;
	}

	arg = init(NULL, true, fn, ctx, period);
	start(arg);
	return arg;
}

struct spdk_cont_poller *
spdk_cont_poller_noop(void)
{
	struct spdk_cont_poller *ret = init(NULL, false, NULL, NULL, 0);

	if (ret == NULL) {
		SPDK_ERRLOG("Poller memory allocation failed\n");
		return NULL;
	}

	start(ret);
	return ret;
}

int spdk_cont_poller_append(struct spdk_cont_poller *parent, cont_poller_fn fn, void *ctx)
{
	struct spdk_cont_poller *ret;

	if (fn == NULL || parent == NULL) {
		return -EFAULT;
	}

	ret = init(parent, false, fn, ctx, 0);
	if (ret == NULL) {
		SPDK_ERRLOG("Poller memory allocation failed\n");
		return -ENOMEM;
	}

	return 0;
}

int spdk_cont_poller_append_poller(struct spdk_cont_poller *parent, cont_poller_fn fn, void *ctx,
				   int period)
{
	struct spdk_cont_poller *ret;

	if (fn == NULL || parent == NULL) {
		return -EFAULT;
	}

	ret = init(parent, true, fn, ctx, period);
	if (ret == NULL) {
		SPDK_ERRLOG("Poller memory allocation failed\n");
		return -ENOMEM;
	}

	return 0;
}


struct common_ctx {
	spdk_callback_fn cb;
	void            *ctx;
};

static int common_cb(struct spdk_cont_poller *actx, void *ctx)
{
	struct common_ctx *cctx = ctx;

	cctx->cb(spdk_cont_poller_parent_status(actx), cctx->ctx);
	free(cctx);
	return 0;
}

int spdk_cont_poller_append_finish(struct spdk_cont_poller *parent, spdk_callback_fn cb, void *ctx)
{
	struct spdk_cont_poller *ret;
	struct common_ctx *cctx;

	if (cb == NULL || parent == NULL) {
		return -EFAULT;
	}

	cctx = malloc(sizeof(*cctx));
	if (cctx == NULL) {
		SPDK_ERRLOG("Poller memory allocation failed\n");
		return -ENOMEM;
	}

	cctx->cb  = cb;
	cctx->ctx = ctx;

	ret = init(parent, false, common_cb, cctx, 0);
	if (ret == NULL) {
		free(cctx);
		SPDK_ERRLOG("Poller memory allocation failed\n");
		return -ENOMEM;
	}

	return 0;
}
