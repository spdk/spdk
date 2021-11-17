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
#include "spdk/likely.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/json.h"
#include "spdk/crc32.h"
#include "spdk/util.h"

/* Accelerator Engine Framework: The following provides a top level
 * generic API for the accelerator functions defined here. Modules,
 * such as the one in /module/accel/ioat, supply the implementation
 * with the exception of the pure software implementation contained
 * later in this file.
 */

#define ALIGN_4K			0x1000
#define MAX_TASKS_PER_CHANNEL		0x800

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

static void _sw_accel_dualcast(void *dst1, void *dst2, void *src, uint64_t nbytes);
static void _sw_accel_copy(void *dst, void *src, uint64_t nbytes);
static void _sw_accel_copyv(void *dst, struct iovec *iov, uint32_t iovcnt);
static int _sw_accel_compare(void *src1, void *src2, uint64_t nbytes);
static void _sw_accel_fill(void *dst, uint8_t fill, uint64_t nbytes);
static void _sw_accel_crc32c(uint32_t *dst, void *src, uint32_t seed, uint64_t nbytes);
static void _sw_accel_crc32cv(uint32_t *dst, struct iovec *iov, uint32_t iovcnt, uint32_t seed);

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

/* Used to determine whether a command is sent to an engine/module or done here
 * via SW implementation.
 */
inline static bool
_is_supported(struct spdk_accel_engine *engine, enum accel_capability operation)
{
	return ((engine->capabilities & operation) == operation);
}

void
spdk_accel_task_complete(struct spdk_accel_task *accel_task, int status)
{
	struct accel_io_channel *accel_ch = accel_task->accel_ch;
	spdk_accel_completion_cb	cb_fn = accel_task->cb_fn;
	void				*cb_arg = accel_task->cb_arg;

	/* We should put the accel_task into the list firstly in order to avoid
	 * the accel task list is exhausted when there is recursive call to
	 * allocate accel_task in user's call back function (cb_fn)
	 */
	TAILQ_INSERT_HEAD(&accel_ch->task_pool, accel_task, link);

	cb_fn(cb_arg, status);
}

/* Accel framework public API for discovering current engine capabilities. */
uint64_t
spdk_accel_get_capabilities(struct spdk_io_channel *ch)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);

	return accel_ch->engine->capabilities;
}

inline static struct spdk_accel_task *
_get_task(struct accel_io_channel *accel_ch, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct spdk_accel_task *accel_task;

	accel_task = TAILQ_FIRST(&accel_ch->task_pool);
	if (accel_task == NULL) {
		return NULL;
	}

	TAILQ_REMOVE(&accel_ch->task_pool, accel_task, link);
	accel_task->link.tqe_next = NULL;
	accel_task->link.tqe_prev = NULL;

	accel_task->cb_fn = cb_fn;
	accel_task->cb_arg = cb_arg;
	accel_task->accel_ch = accel_ch;

	return accel_task;
}

/* Post SW completions to a list and complete in a poller as we don't want to
 * complete them on the caller's stack as they'll likely submit another. */
inline static void
_add_to_comp_list(struct accel_io_channel *accel_ch, struct spdk_accel_task *accel_task, int status)
{
	struct sw_accel_io_channel *sw_ch = spdk_io_channel_get_ctx(accel_ch->sw_engine_ch);

	accel_task->status = status;
	TAILQ_INSERT_TAIL(&sw_ch->tasks_to_complete, accel_task, link);
}

/* Accel framework public API for copy function */
int
spdk_accel_submit_copy(struct spdk_io_channel *ch, void *dst, void *src, uint64_t nbytes,
		       spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (accel_task == NULL) {
		return -ENOMEM;
	}

	accel_task->dst = dst;
	accel_task->src = src;
	accel_task->op_code = ACCEL_OPCODE_MEMMOVE;
	accel_task->nbytes = nbytes;

	if (_is_supported(accel_ch->engine, ACCEL_COPY)) {
		return accel_ch->engine->submit_tasks(accel_ch->engine_ch, accel_task);
	} else {
		_sw_accel_copy(dst, src, nbytes);
		_add_to_comp_list(accel_ch, accel_task, 0);
		return 0;
	}
}

/* Accel framework public API for dual cast copy function */
int
spdk_accel_submit_dualcast(struct spdk_io_channel *ch, void *dst1, void *dst2, void *src,
			   uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;

	if ((uintptr_t)dst1 & (ALIGN_4K - 1) || (uintptr_t)dst2 & (ALIGN_4K - 1)) {
		SPDK_ERRLOG("Dualcast requires 4K alignment on dst addresses\n");
		return -EINVAL;
	}

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (accel_task == NULL) {
		return -ENOMEM;
	}

	accel_task->src = src;
	accel_task->dst = dst1;
	accel_task->dst2 = dst2;
	accel_task->nbytes = nbytes;
	accel_task->op_code = ACCEL_OPCODE_DUALCAST;

	if (_is_supported(accel_ch->engine, ACCEL_DUALCAST)) {
		return accel_ch->engine->submit_tasks(accel_ch->engine_ch, accel_task);
	} else {
		_sw_accel_dualcast(dst1, dst2, src, nbytes);
		_add_to_comp_list(accel_ch, accel_task, 0);
		return 0;
	}
}

/* Accel framework public API for compare function */
int
spdk_accel_submit_compare(struct spdk_io_channel *ch, void *src1, void *src2, uint64_t nbytes,
			  spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;
	int rc;

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (accel_task == NULL) {
		return -ENOMEM;
	}

	accel_task->src = src1;
	accel_task->src2 = src2;
	accel_task->nbytes = nbytes;
	accel_task->op_code = ACCEL_OPCODE_COMPARE;

	if (_is_supported(accel_ch->engine, ACCEL_COMPARE)) {
		return accel_ch->engine->submit_tasks(accel_ch->engine_ch, accel_task);
	} else {
		rc = _sw_accel_compare(src1, src2, nbytes);
		_add_to_comp_list(accel_ch, accel_task, rc);
		return 0;
	}
}

/* Accel framework public API for fill function */
int
spdk_accel_submit_fill(struct spdk_io_channel *ch, void *dst, uint8_t fill, uint64_t nbytes,
		       spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (accel_task == NULL) {
		return -ENOMEM;
	}

	accel_task->dst = dst;
	accel_task->fill_pattern = fill;
	accel_task->nbytes = nbytes;
	accel_task->op_code = ACCEL_OPCODE_MEMFILL;

	if (_is_supported(accel_ch->engine, ACCEL_FILL)) {
		return accel_ch->engine->submit_tasks(accel_ch->engine_ch, accel_task);
	} else {
		_sw_accel_fill(dst, fill, nbytes);
		_add_to_comp_list(accel_ch, accel_task, 0);
		return 0;
	}
}

/* Accel framework public API for CRC-32C function */
int
spdk_accel_submit_crc32c(struct spdk_io_channel *ch, uint32_t *crc_dst, void *src, uint32_t seed,
			 uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (accel_task == NULL) {
		return -ENOMEM;
	}

	accel_task->crc_dst = crc_dst;
	accel_task->src = src;
	accel_task->v.iovcnt = 0;
	accel_task->seed = seed;
	accel_task->nbytes = nbytes;
	accel_task->op_code = ACCEL_OPCODE_CRC32C;

	if (_is_supported(accel_ch->engine, ACCEL_CRC32C)) {
		return accel_ch->engine->submit_tasks(accel_ch->engine_ch, accel_task);
	} else {
		_sw_accel_crc32c(crc_dst, src, seed, nbytes);
		_add_to_comp_list(accel_ch, accel_task, 0);
		return 0;
	}
}

/* Accel framework public API for chained CRC-32C function */
int
spdk_accel_submit_crc32cv(struct spdk_io_channel *ch, uint32_t *crc_dst, struct iovec *iov,
			  uint32_t iov_cnt, uint32_t seed, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch;
	struct spdk_accel_task *accel_task;

	if (iov == NULL) {
		SPDK_ERRLOG("iov should not be NULL");
		return -EINVAL;
	}

	if (!iov_cnt) {
		SPDK_ERRLOG("iovcnt should not be zero value\n");
		return -EINVAL;
	}

	accel_ch = spdk_io_channel_get_ctx(ch);
	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (accel_task == NULL) {
		SPDK_ERRLOG("no memory\n");
		assert(0);
		return -ENOMEM;
	}

	accel_task->v.iovs = iov;
	accel_task->v.iovcnt = iov_cnt;
	accel_task->crc_dst = crc_dst;
	accel_task->seed = seed;
	accel_task->op_code = ACCEL_OPCODE_CRC32C;

	if (_is_supported(accel_ch->engine, ACCEL_CRC32C)) {
		return accel_ch->engine->submit_tasks(accel_ch->engine_ch, accel_task);
	} else {
		_sw_accel_crc32cv(crc_dst, iov, iov_cnt, seed);
		_add_to_comp_list(accel_ch, accel_task, 0);
		return 0;
	}
}

/* Accel framework public API for copy with CRC-32C function */
int
spdk_accel_submit_copy_crc32c(struct spdk_io_channel *ch, void *dst, void *src,
			      uint32_t *crc_dst, uint32_t seed, uint64_t nbytes,
			      spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (accel_task == NULL) {
		return -ENOMEM;
	}

	accel_task->dst = dst;
	accel_task->src = src;
	accel_task->crc_dst = crc_dst;
	accel_task->v.iovcnt = 0;
	accel_task->seed = seed;
	accel_task->nbytes = nbytes;
	accel_task->op_code = ACCEL_OPCODE_COPY_CRC32C;

	if (_is_supported(accel_ch->engine, ACCEL_COPY_CRC32C)) {
		return accel_ch->engine->submit_tasks(accel_ch->engine_ch, accel_task);
	} else {
		_sw_accel_copy(dst, src, nbytes);
		_sw_accel_crc32c(crc_dst, src, seed, nbytes);
		_add_to_comp_list(accel_ch, accel_task, 0);
		return 0;
	}
}

/* Accel framework public API for chained copy + CRC-32C function */
int
spdk_accel_submit_copy_crc32cv(struct spdk_io_channel *ch, void *dst, struct iovec *src_iovs,
			       uint32_t iov_cnt, uint32_t *crc_dst, uint32_t seed,
			       spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch;
	struct spdk_accel_task *accel_task;

	if (src_iovs == NULL) {
		SPDK_ERRLOG("iov should not be NULL");
		return -EINVAL;
	}

	if (!iov_cnt) {
		SPDK_ERRLOG("iovcnt should not be zero value\n");
		return -EINVAL;
	}

	accel_ch = spdk_io_channel_get_ctx(ch);
	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (accel_task == NULL) {
		SPDK_ERRLOG("no memory\n");
		assert(0);
		return -ENOMEM;
	}

	accel_task->v.iovs = src_iovs;
	accel_task->v.iovcnt = iov_cnt;
	accel_task->dst = (void *)dst;
	accel_task->crc_dst = crc_dst;
	accel_task->seed = seed;
	accel_task->op_code = ACCEL_OPCODE_COPY_CRC32C;

	if (_is_supported(accel_ch->engine, ACCEL_COPY_CRC32C)) {
		return accel_ch->engine->submit_tasks(accel_ch->engine_ch, accel_task);
	} else {
		_sw_accel_copyv(dst, src_iovs, iov_cnt);
		_sw_accel_crc32cv(crc_dst, src_iovs, iov_cnt, seed);
		_add_to_comp_list(accel_ch, accel_task, 0);
		return 0;
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
	struct spdk_accel_task *accel_task;
	uint8_t *task_mem;
	int i;

	accel_ch->task_pool_base = calloc(MAX_TASKS_PER_CHANNEL, g_max_accel_module_size);
	if (accel_ch->task_pool_base == NULL) {
		return -ENOMEM;
	}

	TAILQ_INIT(&accel_ch->task_pool);
	task_mem = accel_ch->task_pool_base;
	for (i = 0 ; i < MAX_TASKS_PER_CHANNEL; i++) {
		accel_task = (struct spdk_accel_task *)task_mem;
		TAILQ_INSERT_TAIL(&accel_ch->task_pool, accel_task, link);
		task_mem += g_max_accel_module_size;
	}

	/* Set sw engine channel for operations where hw engine does not support. */
	accel_ch->sw_engine_ch = g_sw_accel_engine->get_io_channel();
	assert(accel_ch->sw_engine_ch != NULL);

	if (g_hw_accel_engine != NULL) {
		accel_ch->engine_ch = g_hw_accel_engine->get_io_channel();
		accel_ch->engine = g_hw_accel_engine;
	} else {
		/* No hw engine enabled, use sw. */
		accel_ch->engine_ch = accel_ch->sw_engine_ch;
		accel_ch->engine = g_sw_accel_engine;
	}
	assert(accel_ch->engine_ch != NULL);
	accel_ch->engine->capabilities = accel_ch->engine->get_capabilities();

	return 0;
}

/* Framework level channel destroy callback. */
static void
accel_engine_destroy_cb(void *io_device, void *ctx_buf)
{
	struct accel_io_channel	*accel_ch = ctx_buf;

	if (accel_ch->sw_engine_ch != accel_ch->engine_ch) {
		spdk_put_io_channel(accel_ch->sw_engine_ch);
	}
	spdk_put_io_channel(accel_ch->engine_ch);
	free(accel_ch->task_pool_base);
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

	TAILQ_FOREACH(accel_engine_module, &spdk_accel_module_list, tailq) {
		accel_engine_module->module_init();
	}
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
	 * The accel fw has no config, there may be some in
	 * the engines/modules though.
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
}

/*
 * The SW Accelerator module is "built in" here (rest of file)
 */
static uint64_t
sw_accel_get_capabilities(void)
{
	/* No HW acceleration capabilities. */
	return 0;
}

static void
_sw_accel_dualcast(void *dst1, void *dst2, void *src, uint64_t nbytes)
{
	memcpy(dst1, src, (size_t)nbytes);
	memcpy(dst2, src, (size_t)nbytes);
}

static void
_sw_accel_copy(void *dst, void *src, uint64_t nbytes)
{
	memcpy(dst, src, (size_t)nbytes);
}

static void
_sw_accel_copyv(void *dst, struct iovec *iov, uint32_t iovcnt)
{
	uint32_t i;

	for (i = 0; i < iovcnt; i++) {
		assert(iov[i].iov_base != NULL);
		memcpy(dst, iov[i].iov_base, iov[i].iov_len);
		dst += iov[i].iov_len;
	}
}

static int
_sw_accel_compare(void *src1, void *src2, uint64_t nbytes)
{
	return memcmp(src1, src2, (size_t)nbytes);
}

static void
_sw_accel_fill(void *dst, uint8_t fill, uint64_t nbytes)
{
	memset(dst, fill, nbytes);
}

static void
_sw_accel_crc32c(uint32_t *crc_dst, void *src, uint32_t seed, uint64_t nbytes)
{
	*crc_dst = spdk_crc32c_update(src, nbytes, ~seed);
}

static void
_sw_accel_crc32cv(uint32_t *crc_dst, struct iovec *iov, uint32_t iovcnt, uint32_t seed)
{
	*crc_dst = spdk_crc32c_iov_update(iov, iovcnt, ~seed);
}

static struct spdk_io_channel *sw_accel_get_io_channel(void);


static struct spdk_accel_engine sw_accel_engine = {
	.get_capabilities	= sw_accel_get_capabilities,
	.get_io_channel		= sw_accel_get_io_channel,
};

static int
accel_comp_poll(void *arg)
{
	struct sw_accel_io_channel	*sw_ch = arg;
	TAILQ_HEAD(, spdk_accel_task)	tasks_to_complete;
	struct spdk_accel_task		*accel_task;

	if (TAILQ_EMPTY(&sw_ch->tasks_to_complete)) {
		return SPDK_POLLER_IDLE;
	}

	TAILQ_INIT(&tasks_to_complete);
	TAILQ_SWAP(&tasks_to_complete, &sw_ch->tasks_to_complete, spdk_accel_task, link);

	while ((accel_task = TAILQ_FIRST(&tasks_to_complete))) {
		TAILQ_REMOVE(&tasks_to_complete, accel_task, link);
		spdk_accel_task_complete(accel_task, accel_task->status);
	}

	return SPDK_POLLER_BUSY;
}

static int
sw_accel_create_cb(void *io_device, void *ctx_buf)
{
	struct sw_accel_io_channel *sw_ch = ctx_buf;

	TAILQ_INIT(&sw_ch->tasks_to_complete);
	sw_ch->completion_poller = SPDK_POLLER_REGISTER(accel_comp_poll, sw_ch, 0);

	return 0;
}

static void
sw_accel_destroy_cb(void *io_device, void *ctx_buf)
{
	struct sw_accel_io_channel *sw_ch = ctx_buf;

	spdk_poller_unregister(&sw_ch->completion_poller);
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

SPDK_LOG_REGISTER_COMPONENT(accel)

SPDK_ACCEL_MODULE_REGISTER(sw_accel_engine_init, sw_accel_engine_fini,
			   NULL, sw_accel_engine_get_ctx_size)
