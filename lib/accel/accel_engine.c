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
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/json.h"
#include "spdk/crc32.h"

/* Accelerator Engine Framework: The following provides a top level
 * generic API for the accelerator functions defined here. Modules,
 * such as the one in /module/accel/ioat, supply the implemention of
 * with the exception of the pure software implemention contained
 * later in this file.
 */

#define ALIGN_4K		0x1000
#define SPDK_ACCEL_NUM_TASKS	0x4000

static struct spdk_mempool *g_accel_task_pool;

/* Largest context size for all accel modules */
static size_t g_max_accel_module_size = 0;

static struct spdk_accel_engine *g_hw_accel_engine = NULL;
static struct spdk_accel_engine *g_sw_accel_engine = NULL;
static struct spdk_accel_module_if *g_accel_engine_module = NULL;
static spdk_accel_fini_cb g_fini_cb_fn = NULL;
static void *g_fini_cb_arg = NULL;

/* Global list of registered accelerator modules */
static TAILQ_HEAD(, spdk_accel_module_if) spdk_accel_module_list =
	TAILQ_HEAD_INITIALIZER(spdk_accel_module_list);

struct accel_io_channel {
	struct spdk_accel_engine	*engine;
	struct spdk_io_channel		*ch;
};

/* Forward declarations of software implementations used when an
 * engine has not implemented the capability.
 */
static int sw_accel_submit_dualcast(struct spdk_io_channel *ch, void *dst1, void *dst2, void *src,
				    uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg);
static int sw_accel_submit_copy(struct spdk_io_channel *ch, void *dst, void *src,
				uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg);
static int sw_accel_submit_compare(struct spdk_io_channel *ch, void *src1, void *src2,
				   uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg);
static int sw_accel_submit_fill(struct spdk_io_channel *ch, void *dst, uint8_t fill,
				uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg);
static int sw_accel_submit_crc32c(struct spdk_io_channel *ch, uint32_t *dst, void *src,
				  uint32_t seed, uint64_t nbytes, spdk_accel_completion_cb cb_fn,
				  void *cb_arg);

/* Registration of hw modules (currently supports only 1 at a time) */
void
spdk_accel_hw_engine_register(struct spdk_accel_engine *accel_engine)
{
	if (g_hw_accel_engine == NULL) {
		g_hw_accel_engine = accel_engine;
	} else {
		SPDK_NOTICELOG("Hardware offload engine already enabled\n");
	}
}

/* Registration of sw modules (currently supports only 1) */
static void
accel_sw_register(struct spdk_accel_engine *accel_engine)
{
	assert(g_sw_accel_engine == NULL);
	g_sw_accel_engine = accel_engine;
}

static void
accel_sw_unregister(void)
{
	g_sw_accel_engine = NULL;
}

/* Common completion routine, called only by the accel framework */
static void
_accel_engine_done(void *ref, int status)
{
	struct spdk_accel_task *req = (struct spdk_accel_task *)ref;

	req->cb(req->cb_arg, status);
	spdk_mempool_put(g_accel_task_pool, req);
}

uint64_t
spdk_accel_get_capabilities(struct spdk_io_channel *ch)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);

	/* All engines are required to implement this API. */
	return accel_ch->engine->get_capabilities();
}

/* Accel framework public API for copy function */
int
spdk_accel_submit_copy(struct spdk_io_channel *ch, void *dst, void *src, uint64_t nbytes,
		       spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_req = spdk_mempool_get(g_accel_task_pool);

	if (accel_req == NULL) {
		SPDK_ERRLOG("Unable to get an accel task.\n");
		return -ENOMEM;
	}

	accel_req->cb = cb_fn;
	accel_req->cb_arg = cb_arg;

	/* If the engine does not support it, fallback to the sw implementation. */
	if (accel_ch->engine->copy) {
		return accel_ch->engine->copy(accel_ch->ch, dst, src, nbytes,
					      _accel_engine_done, accel_req->offload_ctx);
	} else {
		return sw_accel_submit_copy(accel_ch->ch, dst, src, nbytes,
					    _accel_engine_done, accel_req->offload_ctx);
	}
}

/* Accel framework public API for dual cast copy function */
int
spdk_accel_submit_dualcast(struct spdk_io_channel *ch, void *dst1, void *dst2, void *src,
			   uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_req = spdk_mempool_get(g_accel_task_pool);

	if (accel_req == NULL) {
		SPDK_ERRLOG("Unable to get an accel task.\n");
		return -ENOMEM;
	}

	if ((uintptr_t)dst1 & (ALIGN_4K - 1) || (uintptr_t)dst2 & (ALIGN_4K - 1)) {
		SPDK_ERRLOG("Dualcast requires 4K alignment on dst addresses\n");
		return -EINVAL;
	}

	accel_req->cb = cb_fn;
	accel_req->cb_arg = cb_arg;

	/* If the engine does not support it, fallback to the sw implementation. */
	if (accel_ch->engine->dualcast) {
		return accel_ch->engine->dualcast(accel_ch->ch, dst1, dst2, src, nbytes,
						  _accel_engine_done, accel_req->offload_ctx);
	} else {
		return sw_accel_submit_dualcast(accel_ch->ch, dst1, dst2, src, nbytes,
						_accel_engine_done, accel_req->offload_ctx);
	}
}

/* Accel framework public API for batch_create function. All engines are
 * required to implement this API.
 */
struct spdk_accel_batch *
spdk_accel_batch_create(struct spdk_io_channel *ch)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);

	return accel_ch->engine->batch_create(accel_ch->ch);
}

/* Accel framework public API for batch_submit function. All engines are
 * required to implement this API.
 */
int
spdk_accel_batch_submit(struct spdk_io_channel *ch, struct spdk_accel_batch *batch,
			spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_req = spdk_mempool_get(g_accel_task_pool);

	if (accel_req == NULL) {
		SPDK_ERRLOG("Unable to get an accel task.\n");
		return -ENOMEM;
	}

	accel_req->cb = cb_fn;
	accel_req->cb_arg = cb_arg;

	return accel_ch->engine->batch_submit(accel_ch->ch, batch, _accel_engine_done,
					      accel_req->offload_ctx);
}

/* Accel framework public API for getting max batch. All engines are
 * required to implement this API.
 */
uint32_t
spdk_accel_batch_get_max(struct spdk_io_channel *ch)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);

	return accel_ch->engine->batch_get_max();
}

/* Accel framework public API for for when an app is unable to complete a batch sequence,
 * it cancels with this API.
 */
int
spdk_accel_batch_cancel(struct spdk_io_channel *ch, struct spdk_accel_batch *batch)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);

	return accel_ch->engine->batch_cancel(accel_ch->ch, batch);
}

/* Accel framework public API for batch prep_copy function. All engines are
 * required to implement this API.
 */
int
spdk_accel_batch_prep_copy(struct spdk_io_channel *ch, struct spdk_accel_batch *batch, void *dst,
			   void *src, uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_req = spdk_mempool_get(g_accel_task_pool);

	if (accel_req == NULL) {
		SPDK_ERRLOG("Unable to get an accel task.\n");
		return -ENOMEM;
	}

	accel_req->cb = cb_fn;
	accel_req->cb_arg = cb_arg;

	return accel_ch->engine->batch_prep_copy(accel_ch->ch, batch, dst, src, nbytes,
			_accel_engine_done, accel_req->offload_ctx);
}

/* Accel framework public API for batch prep_dualcast function.  All engines are
 * required to implement this API.
 */
int
spdk_accel_batch_prep_dualcast(struct spdk_io_channel *ch, struct spdk_accel_batch *batch,
			       void *dst1, void *dst2, void *src, uint64_t nbytes,
			       spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_req = spdk_mempool_get(g_accel_task_pool);

	if (accel_req == NULL) {
		SPDK_ERRLOG("Unable to get an accel task.\n");
		return -ENOMEM;
	}

	if ((uintptr_t)dst1 & (ALIGN_4K - 1) || (uintptr_t)dst2 & (ALIGN_4K - 1)) {
		SPDK_ERRLOG("Dualcast requires 4K alignment on dst addresses\n");
		return -EINVAL;
	}

	accel_req->cb = cb_fn;
	accel_req->cb_arg = cb_arg;

	return accel_ch->engine->batch_prep_dualcast(accel_ch->ch, batch, dst1, dst2, src,
			nbytes, _accel_engine_done, accel_req->offload_ctx);
}

/* Accel framework public API for batch prep_compare function.  All engines are
 * required to implement this API.
 */
int
spdk_accel_batch_prep_compare(struct spdk_io_channel *ch, struct spdk_accel_batch *batch,
			      void *src1, void *src2, uint64_t nbytes, spdk_accel_completion_cb cb_fn,
			      void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_req = spdk_mempool_get(g_accel_task_pool);

	if (accel_req == NULL) {
		SPDK_ERRLOG("Unable to get an accel task.\n");
		return -ENOMEM;
	}

	accel_req->cb = cb_fn;
	accel_req->cb_arg = cb_arg;

	return accel_ch->engine->batch_prep_compare(accel_ch->ch, batch, src1, src2, nbytes,
			_accel_engine_done, accel_req->offload_ctx);
}

/* Accel framework public API for batch prep_fill function.  All engines are
 * required to implement this API.
 */
int
spdk_accel_batch_prep_fill(struct spdk_io_channel *ch, struct spdk_accel_batch *batch, void *dst,
			   uint8_t fill, uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_req = spdk_mempool_get(g_accel_task_pool);

	if (accel_req == NULL) {
		SPDK_ERRLOG("Unable to get an accel task.\n");
		return -ENOMEM;
	}

	accel_req->cb = cb_fn;
	accel_req->cb_arg = cb_arg;

	return accel_ch->engine->batch_prep_fill(accel_ch->ch, batch, dst, fill, nbytes,
			_accel_engine_done, accel_req->offload_ctx);
}

/* Accel framework public API for batch prep_crc32c function.  All engines are
 * required to implement this API.
 */
int
spdk_accel_batch_prep_crc32c(struct spdk_io_channel *ch, struct spdk_accel_batch *batch,
			     uint32_t *dst, void *src, uint32_t seed, uint64_t nbytes,
			     spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_req = spdk_mempool_get(g_accel_task_pool);

	if (accel_req == NULL) {
		SPDK_ERRLOG("Unable to get an accel task.\n");
		return -ENOMEM;
	}

	accel_req->cb = cb_fn;
	accel_req->cb_arg = cb_arg;

	return accel_ch->engine->batch_prep_crc32c(accel_ch->ch, batch, dst, src, seed, nbytes,
			_accel_engine_done, accel_req->offload_ctx);
}

/* Accel framework public API for compare function */
int
spdk_accel_submit_compare(struct spdk_io_channel *ch, void *src1, void *src2, uint64_t nbytes,
			  spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_req = spdk_mempool_get(g_accel_task_pool);

	if (accel_req == NULL) {
		SPDK_ERRLOG("Unable to get an accel task.\n");
		return -ENOMEM;
	}

	accel_req->cb = cb_fn;
	accel_req->cb_arg = cb_arg;

	/* If the engine does not support it, fallback to the sw implementation. */
	if (accel_ch->engine->compare) {
		return accel_ch->engine->compare(accel_ch->ch, src1, src2, nbytes,
						 _accel_engine_done, accel_req->offload_ctx);
	} else {
		return sw_accel_submit_compare(accel_ch->ch, src1, src2, nbytes,
					       _accel_engine_done, accel_req->offload_ctx);
	}
}

/* Accel framework public API for fill function */
int
spdk_accel_submit_fill(struct spdk_io_channel *ch, void *dst, uint8_t fill, uint64_t nbytes,
		       spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_req = spdk_mempool_get(g_accel_task_pool);

	if (accel_req == NULL) {
		SPDK_ERRLOG("Unable to get an accel task.\n");
		return -ENOMEM;
	}

	accel_req->cb = cb_fn;
	accel_req->cb_arg = cb_arg;

	/* If the engine does not support it, fallback to the sw implementation. */
	if (accel_ch->engine->fill) {
		return accel_ch->engine->fill(accel_ch->ch, dst, fill, nbytes,
					      _accel_engine_done, accel_req->offload_ctx);
	} else {
		return sw_accel_submit_fill(accel_ch->ch, dst, fill, nbytes,
					    _accel_engine_done, accel_req->offload_ctx);
	}
}

/* Accel framework public API for CRC-32C function */
int
spdk_accel_submit_crc32c(struct spdk_io_channel *ch, uint32_t *dst, void *src, uint32_t seed,
			 uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_req = spdk_mempool_get(g_accel_task_pool);

	if (accel_req == NULL) {
		SPDK_ERRLOG("Unable to get an accel task.\n");
		return -ENOMEM;
	}

	accel_req->cb = cb_fn;
	accel_req->cb_arg = cb_arg;

	/* If the engine does not support it, fallback to the sw implementation. */
	if (accel_ch->engine->crc32c) {
		return accel_ch->engine->crc32c(accel_ch->ch, dst, src,	seed, nbytes,
						_accel_engine_done, accel_req->offload_ctx);
	} else {
		return sw_accel_submit_crc32c(accel_ch->ch, dst, src, seed, nbytes,
					      _accel_engine_done, accel_req->offload_ctx);
	}
}

/* Helper function when when accel modules register with the framework. */
void spdk_accel_module_list_add(struct spdk_accel_module_if *accel_module)
{
	TAILQ_INSERT_TAIL(&spdk_accel_module_list, accel_module, tailq);
	if (accel_module->get_ctx_size && accel_module->get_ctx_size() > g_max_accel_module_size) {
		g_max_accel_module_size = accel_module->get_ctx_size();
	}
}

/* Framework level channel create callback. */
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

	/* No hw engine enabled, use sw. */
	accel_ch->ch = g_sw_accel_engine->get_io_channel();
	assert(accel_ch->ch != NULL);
	accel_ch->engine = g_sw_accel_engine;
	return 0;
}

/* Framework level channel destroy callback. */
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

static void
accel_engine_module_initialize(void)
{
	struct spdk_accel_module_if *accel_engine_module;
	char task_pool_name[30];

	TAILQ_FOREACH(accel_engine_module, &spdk_accel_module_list, tailq) {
		accel_engine_module->module_init();
	}

	snprintf(task_pool_name, sizeof(task_pool_name), "accel_task_pool");
	g_accel_task_pool = spdk_mempool_create(task_pool_name,
						SPDK_ACCEL_NUM_TASKS,
						g_max_accel_module_size,
						SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
						SPDK_ENV_SOCKET_ID_ANY);
	assert(g_accel_task_pool);

}

int
spdk_accel_engine_initialize(void)
{
	SPDK_NOTICELOG("Accel engine initialized to use software engine.\n");
	accel_engine_module_initialize();

	/*
	 * We need a unique identifier for the accel engine framework, so use the
	 *  spdk_accel_module_list address for this purpose.
	 */
	spdk_io_device_register(&spdk_accel_module_list, accel_engine_create_cb, accel_engine_destroy_cb,
				sizeof(struct accel_io_channel), "accel_module");

	return 0;
}

static void
accel_engine_module_finish_cb(void)
{
	spdk_accel_fini_cb cb_fn = g_fini_cb_fn;

	cb_fn(g_fini_cb_arg);
	g_fini_cb_fn = NULL;
	g_fini_cb_arg = NULL;
}

void
spdk_accel_write_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_accel_module_if *accel_engine_module;

	/*
	 * The accel engine has no config, there may be some in
	 * the modules though.
	 */
	spdk_json_write_array_begin(w);
	TAILQ_FOREACH(accel_engine_module, &spdk_accel_module_list, tailq) {
		if (accel_engine_module->write_config_json) {
			accel_engine_module->write_config_json(w);
		}
	}
	spdk_json_write_array_end(w);
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
		accel_engine_module_finish_cb();
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
	spdk_mempool_free(g_accel_task_pool);
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

/*
 * The SW Accelerator module is "built in" here (rest of file)
 */

#define SW_ACCEL_BATCH_SIZE 2048

enum sw_accel_opcode {
	SW_ACCEL_OPCODE_MEMMOVE		= 0,
	SW_ACCEL_OPCODE_MEMFILL		= 1,
	SW_ACCEL_OPCODE_COMPARE		= 2,
	SW_ACCEL_OPCODE_CRC32C		= 3,
	SW_ACCEL_OPCODE_DUALCAST	= 4,
};

struct sw_accel_op {
	struct sw_accel_io_channel	*sw_ch;
	void				*cb_arg;
	spdk_accel_completion_cb	cb_fn;
	void				*src;
	union {
		void			*dst;
		void			*src2;
	};
	void				*dst2;
	uint32_t			seed;
	uint64_t			fill_pattern;
	enum sw_accel_opcode		op_code;
	uint64_t			nbytes;
	TAILQ_ENTRY(sw_accel_op)	link;
};

/* The sw accel engine only supports one outstanding batch at a time. */
struct sw_accel_io_channel {
	TAILQ_HEAD(, sw_accel_op)	op_pool;
	TAILQ_HEAD(, sw_accel_op)	batch;
};

static uint64_t
sw_accel_get_capabilities(void)
{
	return ACCEL_COPY | ACCEL_FILL | ACCEL_CRC32C | ACCEL_COMPARE |
	       ACCEL_DUALCAST | ACCEL_BATCH;
}

static uint32_t
sw_accel_batch_get_max(void)
{
	return SW_ACCEL_BATCH_SIZE;
}

/* The sw engine plug-in does not ahve a public API, it is only callable
 * from the accel fw and thus does not need to have its own struct definition
 * of a batch, it just simply casts the address of the single supported batch
 * as the struct spdk_accel_batch pointer.
 */
static struct spdk_accel_batch *
sw_accel_batch_start(struct spdk_io_channel *ch)
{
	struct sw_accel_io_channel *sw_ch = spdk_io_channel_get_ctx(ch);

	if (!TAILQ_EMPTY(&sw_ch->batch)) {
		SPDK_ERRLOG("SW accel engine only supports one batch at a time.\n");
		return NULL;
	}

	return (struct spdk_accel_batch *)&sw_ch->batch;
}

static struct sw_accel_op *
_prep_op(struct sw_accel_io_channel *sw_ch, struct spdk_accel_batch *batch,
	 spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct sw_accel_op *op;

	if ((struct spdk_accel_batch *)&sw_ch->batch != batch) {
		SPDK_ERRLOG("Invalid batch\n");
		return NULL;
	}

	if (!TAILQ_EMPTY(&sw_ch->op_pool)) {
		op = TAILQ_FIRST(&sw_ch->op_pool);
		TAILQ_REMOVE(&sw_ch->op_pool, op, link);
	} else {
		SPDK_ERRLOG("Ran out of operations for batch\n");
		return NULL;
	}

	op->cb_arg = cb_arg;
	op->cb_fn = cb_fn;
	op->sw_ch = sw_ch;

	return op;
}

static int
sw_accel_batch_prep_copy(struct spdk_io_channel *ch, struct spdk_accel_batch *batch,
			 void *dst, void *src, uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct sw_accel_op *op;
	struct sw_accel_io_channel *sw_ch = spdk_io_channel_get_ctx(ch);

	op = _prep_op(sw_ch, batch, cb_fn, cb_arg);
	if (op == NULL) {
		return -EINVAL;
	}

	/* Command specific. */
	op->src = src;
	op->dst = dst;
	op->nbytes = nbytes;
	op->op_code = SW_ACCEL_OPCODE_MEMMOVE;
	TAILQ_INSERT_TAIL(&sw_ch->batch, op, link);

	return 0;
}

static int
sw_accel_batch_prep_dualcast(struct spdk_io_channel *ch, struct spdk_accel_batch *batch, void *dst1,
			     void *dst2,
			     void *src, uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct sw_accel_op *op;
	struct sw_accel_io_channel *sw_ch = spdk_io_channel_get_ctx(ch);

	op = _prep_op(sw_ch, batch, cb_fn, cb_arg);
	if (op == NULL) {
		return -EINVAL;
	}

	/* Command specific. */
	op->src = src;
	op->dst = dst1;
	op->dst2 = dst2;
	op->nbytes = nbytes;
	op->op_code = SW_ACCEL_OPCODE_DUALCAST;
	TAILQ_INSERT_TAIL(&sw_ch->batch, op, link);

	return 0;
}

static int
sw_accel_batch_prep_compare(struct spdk_io_channel *ch, struct spdk_accel_batch *batch, void *src1,
			    void *src2, uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct sw_accel_op *op;
	struct sw_accel_io_channel *sw_ch = spdk_io_channel_get_ctx(ch);

	op = _prep_op(sw_ch, batch, cb_fn, cb_arg);
	if (op == NULL) {
		return -EINVAL;
	}

	/* Command specific. */
	op->src = src1;
	op->src2 = src2;
	op->nbytes = nbytes;
	op->op_code = SW_ACCEL_OPCODE_COMPARE;
	TAILQ_INSERT_TAIL(&sw_ch->batch, op, link);

	return 0;
}

static int
sw_accel_batch_prep_fill(struct spdk_io_channel *ch, struct spdk_accel_batch *batch, void *dst,
			 uint8_t fill,
			 uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct sw_accel_op *op;
	struct sw_accel_io_channel *sw_ch = spdk_io_channel_get_ctx(ch);

	op = _prep_op(sw_ch, batch, cb_fn, cb_arg);
	if (op == NULL) {
		return -EINVAL;
	}

	/* Command specific. */
	op->dst = dst;
	op->fill_pattern = fill;
	op->nbytes = nbytes;
	op->op_code = SW_ACCEL_OPCODE_MEMFILL;
	TAILQ_INSERT_TAIL(&sw_ch->batch, op, link);

	return 0;
}

static int
sw_accel_batch_prep_crc32c(struct spdk_io_channel *ch, struct spdk_accel_batch *batch,
			   uint32_t *dst,
			   void *src, uint32_t seed, uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct sw_accel_op *op;
	struct sw_accel_io_channel *sw_ch = spdk_io_channel_get_ctx(ch);

	op = _prep_op(sw_ch, batch, cb_fn, cb_arg);
	if (op == NULL) {
		return -EINVAL;
	}

	/* Command specific. */
	op->dst = (void *)dst;
	op->src = src;
	op->seed = seed;
	op->nbytes = nbytes;
	op->op_code = SW_ACCEL_OPCODE_CRC32C;
	TAILQ_INSERT_TAIL(&sw_ch->batch, op, link);

	return 0;
}


static int
sw_accel_batch_cancel(struct spdk_io_channel *ch, struct spdk_accel_batch *batch)
{
	struct sw_accel_op *op;
	struct sw_accel_io_channel *sw_ch = spdk_io_channel_get_ctx(ch);

	if ((struct spdk_accel_batch *)&sw_ch->batch != batch) {
		SPDK_ERRLOG("Invalid batch\n");
		return -EINVAL;
	}

	/* Cancel the batch items by moving them back to the op_pool. */
	while ((op = TAILQ_FIRST(&sw_ch->batch))) {
		TAILQ_REMOVE(&sw_ch->batch, op, link);
		TAILQ_INSERT_TAIL(&sw_ch->op_pool, op, link);
	}

	return 0;
}

static int
sw_accel_batch_submit(struct spdk_io_channel *ch, struct spdk_accel_batch *batch,
		      spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct sw_accel_op *op;
	struct sw_accel_io_channel *sw_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_req;
	int batch_status = 0, cmd_status = 0;

	if ((struct spdk_accel_batch *)&sw_ch->batch != batch) {
		SPDK_ERRLOG("Invalid batch\n");
		return -EINVAL;
	}

	/* Complete the batch items. */
	while ((op = TAILQ_FIRST(&sw_ch->batch))) {
		TAILQ_REMOVE(&sw_ch->batch, op, link);
		accel_req = (struct spdk_accel_task *)((uintptr_t)op->cb_arg -
						       offsetof(struct spdk_accel_task, offload_ctx));

		switch (op->op_code) {
		case SW_ACCEL_OPCODE_MEMMOVE:
			memcpy(op->dst, op->src, op->nbytes);
			break;
		case SW_ACCEL_OPCODE_DUALCAST:
			memcpy(op->dst, op->src, op->nbytes);
			memcpy(op->dst2, op->src, op->nbytes);
			break;
		case SW_ACCEL_OPCODE_COMPARE:
			cmd_status = memcmp(op->src, op->src2, op->nbytes);
			break;
		case SW_ACCEL_OPCODE_MEMFILL:
			memset(op->dst, op->fill_pattern, op->nbytes);
			break;
		case SW_ACCEL_OPCODE_CRC32C:
			*(uint32_t *)op->dst = spdk_crc32c_update(op->src, op->nbytes, ~op->seed);
			break;
		default:
			assert(false);
			break;
		}

		batch_status |= cmd_status;
		op->cb_fn(accel_req, cmd_status);
		TAILQ_INSERT_TAIL(&sw_ch->op_pool, op, link);
	}

	/* Now complete the batch request itself. */
	accel_req = (struct spdk_accel_task *)((uintptr_t)cb_arg -
					       offsetof(struct spdk_accel_task, offload_ctx));
	cb_fn(accel_req, batch_status);

	return 0;
}

static int
sw_accel_submit_copy(struct spdk_io_channel *ch, void *dst, void *src,
		     uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct spdk_accel_task *accel_req;

	memcpy(dst, src, (size_t)nbytes);

	accel_req = (struct spdk_accel_task *)((uintptr_t)cb_arg -
					       offsetof(struct spdk_accel_task, offload_ctx));
	cb_fn(accel_req, 0);
	return 0;
}

static int
sw_accel_submit_dualcast(struct spdk_io_channel *ch, void *dst1, void *dst2,
			 void *src, uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct spdk_accel_task *accel_req;

	memcpy(dst1, src, (size_t)nbytes);
	memcpy(dst2, src, (size_t)nbytes);

	accel_req = (struct spdk_accel_task *)((uintptr_t)cb_arg -
					       offsetof(struct spdk_accel_task, offload_ctx));
	cb_fn(accel_req, 0);
	return 0;
}

static int
sw_accel_submit_compare(struct spdk_io_channel *ch, void *src1, void *src2,
			uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct spdk_accel_task *accel_req;
	int result;

	result = memcmp(src1, src2, (size_t)nbytes);

	accel_req = (struct spdk_accel_task *)((uintptr_t)cb_arg -
					       offsetof(struct spdk_accel_task, offload_ctx));
	cb_fn(accel_req, result);

	return 0;
}

static int
sw_accel_submit_fill(struct spdk_io_channel *ch, void *dst, uint8_t fill,
		     uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct spdk_accel_task *accel_req;

	memset(dst, fill, nbytes);
	accel_req = (struct spdk_accel_task *)((uintptr_t)cb_arg -
					       offsetof(struct spdk_accel_task, offload_ctx));
	cb_fn(accel_req, 0);

	return 0;
}

static int
sw_accel_submit_crc32c(struct spdk_io_channel *ch, uint32_t *dst, void *src,
		       uint32_t seed, uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct spdk_accel_task *accel_req;

	*dst = spdk_crc32c_update(src, nbytes, ~seed);
	accel_req = (struct spdk_accel_task *)((uintptr_t)cb_arg -
					       offsetof(struct spdk_accel_task, offload_ctx));
	cb_fn(accel_req, 0);

	return 0;
}

static struct spdk_io_channel *sw_accel_get_io_channel(void);

static struct spdk_accel_engine sw_accel_engine = {
	.get_capabilities	= sw_accel_get_capabilities,
	.copy			= sw_accel_submit_copy,
	.dualcast		= sw_accel_submit_dualcast,
	.batch_get_max		= sw_accel_batch_get_max,
	.batch_create		= sw_accel_batch_start,
	.batch_cancel		= sw_accel_batch_cancel,
	.batch_prep_copy	= sw_accel_batch_prep_copy,
	.batch_prep_dualcast	= sw_accel_batch_prep_dualcast,
	.batch_prep_compare	= sw_accel_batch_prep_compare,
	.batch_prep_fill	= sw_accel_batch_prep_fill,
	.batch_prep_crc32c	= sw_accel_batch_prep_crc32c,
	.batch_submit		= sw_accel_batch_submit,
	.compare		= sw_accel_submit_compare,
	.fill			= sw_accel_submit_fill,
	.crc32c			= sw_accel_submit_crc32c,
	.get_io_channel		= sw_accel_get_io_channel,
};

static int
sw_accel_create_cb(void *io_device, void *ctx_buf)
{
	struct sw_accel_io_channel *sw_ch = ctx_buf;
	struct sw_accel_op *op;
	int i;

	TAILQ_INIT(&sw_ch->batch);

	TAILQ_INIT(&sw_ch->op_pool);
	for (i = 0 ; i < SW_ACCEL_BATCH_SIZE ; i++) {
		op = calloc(1, sizeof(struct sw_accel_op));
		if (op == NULL) {
			SPDK_ERRLOG("Failed to allocate operation for batch.\n");
			while ((op = TAILQ_FIRST(&sw_ch->op_pool))) {
				TAILQ_REMOVE(&sw_ch->op_pool, op, link);
				free(op);
			}
			return -ENOMEM;
		}
		TAILQ_INSERT_TAIL(&sw_ch->op_pool, op, link);
	}

	return 0;
}

static void
sw_accel_destroy_cb(void *io_device, void *ctx_buf)
{
	struct sw_accel_io_channel *sw_ch = ctx_buf;
	struct sw_accel_op *op;

	while ((op = TAILQ_FIRST(&sw_ch->op_pool))) {
		TAILQ_REMOVE(&sw_ch->op_pool, op, link);
		free(op);
	}
}

static struct spdk_io_channel *sw_accel_get_io_channel(void)
{
	return spdk_get_io_channel(&sw_accel_engine);
}

static size_t
sw_accel_engine_get_ctx_size(void)
{
	return sizeof(struct spdk_accel_task);
}

static int
sw_accel_engine_init(void)
{
	accel_sw_register(&sw_accel_engine);
	spdk_io_device_register(&sw_accel_engine, sw_accel_create_cb, sw_accel_destroy_cb,
				sizeof(struct sw_accel_io_channel), "sw_accel_engine");

	return 0;
}

static void
sw_accel_engine_fini(void *ctxt)
{
	spdk_io_device_unregister(&sw_accel_engine, NULL);
	accel_sw_unregister();

	spdk_accel_engine_module_finish();
}

SPDK_ACCEL_MODULE_REGISTER(sw_accel_engine_init, sw_accel_engine_fini,
			   NULL, NULL, sw_accel_engine_get_ctx_size)
