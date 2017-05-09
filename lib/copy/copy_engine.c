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

#include "spdk_internal/copy_engine.h"

#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/io_channel.h"

#include "spdk_internal/event.h"

static size_t g_max_copy_module_size = 0;

static struct spdk_copy_engine *hw_copy_engine = NULL;
/* Memcpy engine always exist */
static struct spdk_copy_engine *mem_copy_engine = NULL;

TAILQ_HEAD(, spdk_copy_module_if) spdk_copy_module_list =
	TAILQ_HEAD_INITIALIZER(spdk_copy_module_list);

struct copy_io_channel {
	struct spdk_copy_engine	*engine;
	struct spdk_io_channel	*ch;
};

void
spdk_copy_engine_register(struct spdk_copy_engine *copy_engine)
{
	assert(hw_copy_engine == NULL);
	hw_copy_engine = copy_engine;
}

static void
spdk_memcpy_register(struct spdk_copy_engine *copy_engine)
{
	assert(mem_copy_engine == NULL);
	mem_copy_engine = copy_engine;
}

static void
copy_engine_done(void *ref, int status)
{
	struct spdk_copy_task *req = (struct spdk_copy_task *)ref;

	req->cb(req, status);
}

int64_t
spdk_copy_submit(struct spdk_copy_task *copy_req, struct spdk_io_channel *ch,
		 void *dst, void *src, uint64_t nbytes, spdk_copy_completion_cb cb)
{
	struct spdk_copy_task *req = copy_req;
	struct copy_io_channel *copy_ch = spdk_io_channel_get_ctx(ch);

	req->cb = cb;
	return copy_ch->engine->copy(req->offload_ctx, copy_ch->ch, dst, src, nbytes,
				     copy_engine_done);
}

int64_t
spdk_copy_submit_fill(struct spdk_copy_task *copy_req, struct spdk_io_channel *ch,
		      void *dst, uint8_t fill, uint64_t nbytes, spdk_copy_completion_cb cb)
{
	struct spdk_copy_task *req = copy_req;
	struct copy_io_channel *copy_ch = spdk_io_channel_get_ctx(ch);

	req->cb = cb;
	return copy_ch->engine->fill(req->offload_ctx, copy_ch->ch, dst, fill, nbytes,
				     copy_engine_done);
}

/* memcpy default copy engine */
static int64_t
mem_copy_submit(void *cb_arg, struct spdk_io_channel *ch, void *dst, void *src, uint64_t nbytes,
		spdk_copy_completion_cb cb)
{
	struct spdk_copy_task *copy_req;

	memcpy(dst, src, (size_t)nbytes);

	copy_req = (struct spdk_copy_task *)((uintptr_t)cb_arg -
					     offsetof(struct spdk_copy_task, offload_ctx));
	cb(copy_req, 0);
	return nbytes;
}

static int64_t
mem_copy_fill(void *cb_arg, struct spdk_io_channel *ch, void *dst, uint8_t fill, uint64_t nbytes,
	      spdk_copy_completion_cb cb)
{
	struct spdk_copy_task *copy_req;

	memset(dst, fill, nbytes);
	copy_req = (struct spdk_copy_task *)((uintptr_t)cb_arg -
					     offsetof(struct spdk_copy_task, offload_ctx));
	cb(copy_req, 0);

	return nbytes;
}

static struct spdk_io_channel *mem_get_io_channel(uint32_t priority);

static struct spdk_copy_engine memcpy_copy_engine = {
	.copy		= mem_copy_submit,
	.fill		= mem_copy_fill,
	.get_io_channel	= mem_get_io_channel,
};

static int
memcpy_create_cb(void *io_device, uint32_t priority, void *ctx_buf, void *unique_ctx)
{
	return 0;
}

static void
memcpy_destroy_cb(void *io_device, void *ctx_buf)
{
}

static struct spdk_io_channel *mem_get_io_channel(uint32_t priority)
{
	return spdk_get_io_channel(&memcpy_copy_engine, priority, false, NULL);
}

static size_t
copy_engine_mem_get_ctx_size(void)
{
	return sizeof(struct spdk_copy_task);
}

size_t
spdk_copy_task_size(void)
{
	return g_max_copy_module_size;
}

void spdk_copy_module_list_add(struct spdk_copy_module_if *copy_module)
{
	TAILQ_INSERT_TAIL(&spdk_copy_module_list, copy_module, tailq);
	if (copy_module->get_ctx_size && copy_module->get_ctx_size() > g_max_copy_module_size) {
		g_max_copy_module_size = copy_module->get_ctx_size();
	}
}

static int
copy_create_cb(void *io_device, uint32_t priority, void *ctx_buf, void *unique_ctx)
{
	struct copy_io_channel	*copy_ch = ctx_buf;

	if (hw_copy_engine != NULL) {
		copy_ch->ch = hw_copy_engine->get_io_channel(priority);
		if (copy_ch->ch != NULL) {
			copy_ch->engine = hw_copy_engine;
			return 0;
		}
	}

	copy_ch->ch = mem_copy_engine->get_io_channel(priority);
	assert(copy_ch->ch != NULL);
	copy_ch->engine = mem_copy_engine;
	return 0;
}

static void
copy_destroy_cb(void *io_device, void *ctx_buf)
{
	struct copy_io_channel	*copy_ch = ctx_buf;

	spdk_put_io_channel(copy_ch->ch);
}

struct spdk_io_channel *
spdk_copy_engine_get_io_channel(uint32_t priority)
{
	return spdk_get_io_channel(&spdk_copy_module_list, priority, false, NULL);
}

static int
copy_engine_mem_init(void)
{
	spdk_memcpy_register(&memcpy_copy_engine);
	spdk_io_device_register(&memcpy_copy_engine, memcpy_create_cb, memcpy_destroy_cb, 0);

	return 0;
}

static void
spdk_copy_engine_module_initialize(void)
{
	struct spdk_copy_module_if *copy_engine_module;

	TAILQ_FOREACH(copy_engine_module, &spdk_copy_module_list, tailq) {
		copy_engine_module->module_init();
	}
}

static void
spdk_copy_engine_module_finish(void)
{
	struct spdk_copy_module_if *copy_engine_module;

	TAILQ_FOREACH(copy_engine_module, &spdk_copy_module_list, tailq) {
		if (copy_engine_module->module_fini)
			copy_engine_module->module_fini();
	}
}

static int
spdk_copy_engine_initialize(void)
{
	spdk_copy_engine_module_initialize();
	/*
	 * We need a unique identifier for the copy engine framework, so use the
	 *  spdk_copy_module_list address for this purpose.
	 */
	spdk_io_device_register(&spdk_copy_module_list, copy_create_cb, copy_destroy_cb,
				sizeof(struct copy_io_channel));
	return 0;
}

static int
spdk_copy_engine_finish(void)
{
	spdk_copy_engine_module_finish();
	return 0;
}

SPDK_COPY_MODULE_REGISTER(copy_engine_mem_init, NULL, NULL, copy_engine_mem_get_ctx_size)
SPDK_SUBSYSTEM_REGISTER(copy, spdk_copy_engine_initialize, spdk_copy_engine_finish, NULL)
