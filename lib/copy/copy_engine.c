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

#include "spdk/copy_engine.h"

#include <stdio.h>
#include <errno.h>
#include <rte_config.h>
#include <rte_debug.h>
#include <rte_malloc.h>
#include <rte_memcpy.h>
#include <rte_lcore.h>

#include "spdk/log.h"
#include "spdk/event.h"

struct mem_request {
	struct mem_request	*next;
	copy_completion_cb	cb;
};

struct mem_request *copy_engine_req_head[RTE_MAX_LCORE];

static struct spdk_copy_engine *hw_copy_engine = NULL;
/* Memcpy engine always exist */
static struct spdk_copy_engine *mem_copy_engine = NULL;

TAILQ_HEAD(, spdk_copy_module_if) spdk_copy_module_list =
	TAILQ_HEAD_INITIALIZER(spdk_copy_module_list);

void
spdk_copy_engine_register(struct spdk_copy_engine *copy_engine)
{
	RTE_VERIFY(hw_copy_engine == NULL);
	hw_copy_engine = copy_engine;
}

static void
spdk_memcpy_register(struct spdk_copy_engine *copy_engine)
{
	RTE_VERIFY(mem_copy_engine == NULL);
	mem_copy_engine = copy_engine;
}

static int
spdk_has_copy_engine(void)
{
	return (hw_copy_engine == NULL) ? 0 : 1;
}

int
spdk_copy_check_io(void)
{
	if (spdk_has_copy_engine())
		hw_copy_engine->check_io();
	else
		mem_copy_engine->check_io();

	return 0;
}

static void
copy_engine_done(void *ref, int status)
{
	struct copy_task *req = (struct copy_task *)ref;

	req->cb(req, status);
}

int64_t
spdk_copy_submit(struct copy_task *copy_req, void *dst, void *src,
		 uint64_t nbytes, copy_completion_cb cb)
{
	struct copy_task *req = copy_req;

	req->cb = cb;

	if (spdk_has_copy_engine())
		return hw_copy_engine->copy(req->offload_ctx, dst, src, nbytes,
					    copy_engine_done);

	return mem_copy_engine->copy(req->offload_ctx, dst, src, nbytes,
				     copy_engine_done);
}

int64_t
spdk_copy_submit_fill(struct copy_task *copy_req, void *dst, uint8_t fill,
		      uint64_t nbytes, copy_completion_cb cb)
{
	struct copy_task *req = copy_req;

	req->cb = cb;

	if (hw_copy_engine && hw_copy_engine->fill) {
		return hw_copy_engine->fill(req->offload_ctx, dst, fill, nbytes,
					    copy_engine_done);
	}

	return mem_copy_engine->fill(req->offload_ctx, dst, fill, nbytes,
				     copy_engine_done);
}

/* memcpy default copy engine */
static void
mem_copy_check_io(void)
{
	struct mem_request **req_head = &copy_engine_req_head[rte_lcore_id()];
	struct mem_request *req = *req_head;
	struct mem_request *req_next;
	struct copy_task *copy_req;

	*req_head = NULL;

	while (req != NULL) {
		req_next = req->next;
		copy_req = (struct copy_task *)((uintptr_t)req -
						offsetof(struct copy_task, offload_ctx));
		req->cb((void *)copy_req, 0);
		req = req_next;
	}

}

static int64_t
mem_copy_submit(void *cb_arg, void *dst, void *src, uint64_t nbytes,
		copy_completion_cb cb)
{
	struct mem_request **req_head = &copy_engine_req_head[rte_lcore_id()];
	struct mem_request *req = (struct mem_request *)cb_arg;

	req->next = *req_head;
	*req_head = req;
	req->cb = cb;

	rte_memcpy(dst, src, (size_t)nbytes);

	return nbytes;
}

static int64_t
mem_copy_fill(void *cb_arg, void *dst, uint8_t fill, uint64_t nbytes,
	      copy_completion_cb cb)
{
	struct mem_request **req_head = &copy_engine_req_head[rte_lcore_id()];
	struct mem_request *req = (struct mem_request *)cb_arg;

	req->next = *req_head;
	*req_head = req;
	req->cb = cb;

	memset(dst, fill, nbytes);

	return nbytes;
}

static struct spdk_copy_engine memcpy_copy_engine = {
	.copy		= mem_copy_submit,
	.fill		= mem_copy_fill,
	.check_io	= mem_copy_check_io,
};

static int
copy_engine_mem_get_ctx_size(void)
{
	return sizeof(struct mem_request) + sizeof(struct copy_task);
}

int spdk_copy_module_get_max_ctx_size(void)
{
	struct spdk_copy_module_if *copy_engine;
	int max_copy_module_size = 0;

	TAILQ_FOREACH(copy_engine, &spdk_copy_module_list, tailq) {
		if (copy_engine->get_ctx_size && copy_engine->get_ctx_size() > max_copy_module_size) {
			max_copy_module_size = copy_engine->get_ctx_size();
		}
	}
	return max_copy_module_size;
}

void spdk_copy_module_list_add(struct spdk_copy_module_if *copy_module)
{
	TAILQ_INSERT_TAIL(&spdk_copy_module_list, copy_module, tailq);
}

static int
copy_engine_mem_init(void)
{
	int i;

	for (i = 0; i < RTE_MAX_LCORE; i++) {
		copy_engine_req_head[i] = NULL;
	}

	/* Anyway, We will register memcpy engine */
	spdk_memcpy_register(&memcpy_copy_engine);

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
