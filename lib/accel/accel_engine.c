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

#include "spdk_internal/accel_engine.h"

#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/thread.h"

static size_t g_max_accel_module_size = 0;

static struct spdk_accel_engine *g_hw_accel_engine = NULL;
/* Software memcpy engine always exists */
static struct spdk_accel_engine *g_sw_accel_engine = NULL;

static TAILQ_HEAD(, spdk_accel_module_if) spdk_accel_module_list =
	TAILQ_HEAD_INITIALIZER(spdk_accel_module_list);

struct accel_io_channel {
	struct spdk_accel_engine	*engine;
	struct spdk_io_channel		*ch;
};

static struct spdk_accel_module_if *g_accel_engine_module = NULL;
static spdk_accel_fini_cb g_fini_cb_fn = NULL;
static void *g_fini_cb_arg = NULL;

void
spdk_accel_engine_register(struct spdk_accel_engine *accel_engine)
{
	assert(g_hw_accel_engine == NULL);
	g_hw_accel_engine = accel_engine;
}

static void
spdk_sw_accel_register(struct spdk_accel_engine *accel_engine)
{
	assert(g_sw_accel_engine == NULL);
	g_sw_accel_engine = accel_engine;
}

static void
spdk_sw_accel_unregister(void)
{
	g_sw_accel_engine = NULL;
}

static void
accel_engine_done(void *ref, int status)
{
	struct spdk_accel_task *req = (struct spdk_accel_task *)ref;

	req->cb(req, status);
}

int
spdk_accel_submit_copy(struct spdk_accel_task *accel_req, struct spdk_io_channel *ch,
		       void *dst, void *src, uint64_t nbytes, spdk_accel_completion_cb cb)
{
	struct spdk_accel_task *req = accel_req;
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);

	req->cb = cb;
	return accel_ch->engine->copy(req->offload_ctx, accel_ch->ch, dst, src, nbytes,
				      accel_engine_done);
}

int
spdk_accel_submit_fill(struct spdk_accel_task *accel_req, struct spdk_io_channel *ch,
		       void *dst, uint8_t fill, uint64_t nbytes, spdk_accel_completion_cb cb)
{
	struct spdk_accel_task *req = accel_req;
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);

	req->cb = cb;
	return accel_ch->engine->fill(req->offload_ctx, accel_ch->ch, dst, fill, nbytes,
				      accel_engine_done);
}

/* Software memcpy default accel engine */
static int
sw_accel_submit_copy(void *cb_arg, struct spdk_io_channel *ch, void *dst, void *src,
		     uint64_t nbytes,
		     spdk_accel_completion_cb cb)
{
	struct spdk_accel_task *accel_req;

	memcpy(dst, src, (size_t)nbytes);

	accel_req = (struct spdk_accel_task *)((uintptr_t)cb_arg -
					       offsetof(struct spdk_accel_task, offload_ctx));
	cb(accel_req, 0);
	return 0;
}

static int
sw_accel_submit_fill(void *cb_arg, struct spdk_io_channel *ch, void *dst, uint8_t fill,
		     uint64_t nbytes,
		     spdk_accel_completion_cb cb)
{
	struct spdk_accel_task *accel_req;

	memset(dst, fill, nbytes);
	accel_req = (struct spdk_accel_task *)((uintptr_t)cb_arg -
					       offsetof(struct spdk_accel_task, offload_ctx));
	cb(accel_req, 0);

	return 0;
}

static struct spdk_io_channel *sw_accel_get_io_channel(void);

static struct spdk_accel_engine sw_accel_engine = {
	.copy		= sw_accel_submit_copy,
	.fill		= sw_accel_submit_fill,
	.get_io_channel	= sw_accel_get_io_channel,
};

static int
sw_accel_create_cb(void *io_device, void *ctx_buf)
{
	return 0;
}

static void
sw_accel_destroy_cb(void *io_device, void *ctx_buf)
{
}

static struct spdk_io_channel *sw_accel_get_io_channel(void)
{
	return spdk_get_io_channel(&sw_accel_engine);
}

static size_t
accel_engine_sw_get_ctx_size(void)
{
	return sizeof(struct spdk_accel_task);
}

size_t
spdk_accel_task_size(void)
{
	return g_max_accel_module_size;
}

void spdk_accel_module_list_add(struct spdk_accel_module_if *accel_module)
{
	TAILQ_INSERT_TAIL(&spdk_accel_module_list, accel_module, tailq);
	if (accel_module->get_ctx_size && accel_module->get_ctx_size() > g_max_accel_module_size) {
		g_max_accel_module_size = accel_module->get_ctx_size();
	}
}

static int
accel_engine_create_cb(void *io_device, void *ctx_buf)
{
	struct accel_io_channel	*accel_ch = ctx_buf;

	if (g_hw_accel_engine != NULL) {
		accel_ch->ch = g_hw_accel_engine->get_io_channel();
		if (accel_ch->ch != NULL) {
			accel_ch->engine = g_hw_accel_engine;
			return 0;
		}
	}

	accel_ch->ch = g_sw_accel_engine->get_io_channel();
	assert(accel_ch->ch != NULL);
	accel_ch->engine = g_sw_accel_engine;
	return 0;
}

static void
accel_engine_destroy_cb(void *io_device, void *ctx_buf)
{
	struct accel_io_channel	*accel_ch = ctx_buf;

	spdk_put_io_channel(accel_ch->ch);
}

struct spdk_io_channel *
spdk_accel_engine_get_io_channel(void)
{
	return spdk_get_io_channel(&spdk_accel_module_list);
}

static int
accel_engine_sw_accel_init(void)
{
	spdk_sw_accel_register(&sw_accel_engine);
	spdk_io_device_register(&sw_accel_engine, sw_accel_create_cb, sw_accel_destroy_cb, 0,
				"sw_accel_engine");

	return 0;
}

static void
accel_engine_sw_accel_fini(void *ctxt)
{
	spdk_io_device_unregister(&sw_accel_engine, NULL);
	spdk_sw_accel_unregister();

	spdk_accel_engine_module_finish();
}

static void
spdk_accel_engine_module_initialize(void)
{
	struct spdk_accel_module_if *accel_engine_module;

	TAILQ_FOREACH(accel_engine_module, &spdk_accel_module_list, tailq) {
		accel_engine_module->module_init();
	}
}

int
spdk_accel_engine_initialize(void)
{
	spdk_accel_engine_module_initialize();
	/*
	 * We need a unique identifier for the accel engine framework, so use the
	 *  spdk_accel_module_list address for this purpose.
	 */
	spdk_io_device_register(&spdk_accel_module_list, accel_engine_create_cb, accel_engine_destroy_cb,
				sizeof(struct accel_io_channel), "accel_module");

	return 0;
}

static void
spdk_accel_engine_module_finish_cb(void)
{
	spdk_accel_fini_cb cb_fn = g_fini_cb_fn;

	cb_fn(g_fini_cb_arg);
	g_fini_cb_fn = NULL;
	g_fini_cb_arg = NULL;
}

void
spdk_accel_engine_module_finish(void)
{
	if (!g_accel_engine_module) {
		g_accel_engine_module = TAILQ_FIRST(&spdk_accel_module_list);
	} else {
		g_accel_engine_module = TAILQ_NEXT(g_accel_engine_module, tailq);
	}

	if (!g_accel_engine_module) {
		spdk_accel_engine_module_finish_cb();
		return;
	}

	if (g_accel_engine_module->module_fini) {
		spdk_thread_send_msg(spdk_get_thread(), g_accel_engine_module->module_fini, NULL);
	} else {
		spdk_accel_engine_module_finish();
	}
}

void
spdk_accel_engine_finish(spdk_accel_fini_cb cb_fn, void *cb_arg)
{
	assert(cb_fn != NULL);

	g_fini_cb_fn = cb_fn;
	g_fini_cb_arg = cb_arg;

	spdk_io_device_unregister(&spdk_accel_module_list, NULL);
	spdk_accel_engine_module_finish();
}

void
spdk_accel_engine_config_text(FILE *fp)
{
	struct spdk_accel_module_if *accel_engine_module;

	TAILQ_FOREACH(accel_engine_module, &spdk_accel_module_list, tailq) {
		if (accel_engine_module->config_text) {
			accel_engine_module->config_text(fp);
		}
	}
}

SPDK_ACCEL_MODULE_REGISTER(accel_engine_sw_accel_init, accel_engine_sw_accel_fini,
			   NULL, accel_engine_sw_get_ctx_size)
