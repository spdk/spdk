/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
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

#ifdef SPDK_CONFIG_PMDK
#include "libpmem.h"
#endif

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

static struct spdk_accel_module_if *g_accel_engine_module = NULL;
static spdk_accel_fini_cb g_fini_cb_fn = NULL;
static void *g_fini_cb_arg = NULL;
static bool g_engine_started = false;

/* Global list of registered accelerator modules */
static TAILQ_HEAD(, spdk_accel_module_if) spdk_accel_module_list =
	TAILQ_HEAD_INITIALIZER(spdk_accel_module_list);

/* Global array mapping capabilities to engines */
static struct spdk_accel_module_if *g_engines_opc[ACCEL_OPC_LAST] = {};
static char *g_engines_opc_override[ACCEL_OPC_LAST] = {};

static int sw_accel_submit_tasks(struct spdk_io_channel *ch, struct spdk_accel_task *first_task);

int
spdk_accel_get_opc_engine_name(enum accel_opcode opcode, const char **engine_name)
{
	if (opcode >= ACCEL_OPC_LAST) {
		/* invalid opcode */
		return -EINVAL;
	}

	if (g_engines_opc[opcode]) {
		*engine_name = g_engines_opc[opcode]->name;
	} else {
		return -ENOENT;
	}

	return 0;
}

void
_accel_for_each_engine(struct engine_info *info, _accel_for_each_engine_fn fn)
{
	struct spdk_accel_module_if *accel_engine;
	enum accel_opcode opcode;
	int j = 0;

	TAILQ_FOREACH(accel_engine, &spdk_accel_module_list, tailq) {
		for (opcode = 0; opcode < ACCEL_OPC_LAST; opcode++) {
			if (accel_engine->supports_opcode(opcode)) {
				info->ops[j] = opcode;
				j++;
			}
		}
		info->name = accel_engine->name;
		info->num_ops = j;
		fn(info);
		j = 0;
	}
}

int
spdk_accel_assign_opc(enum accel_opcode opcode, const char *name)
{
	if (g_engine_started == true) {
		/* we don't allow re-assignment once things have started */
		return -EINVAL;
	}

	if (opcode >= ACCEL_OPC_LAST) {
		/* invalid opcode */
		return -EINVAL;
	}

	/* engine selection will be validated after the framework starts. */
	g_engines_opc_override[opcode] = strdup(name);

	return 0;
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
_add_to_comp_list(struct sw_accel_io_channel *sw_ch, struct spdk_accel_task *accel_task, int status)
{
	accel_task->status = status;
	TAILQ_INSERT_TAIL(&sw_ch->tasks_to_complete, accel_task, link);
}

/* Used when the SW engine is selected and the durable flag is set. */
inline static int
_check_flags(int flags)
{
	if (flags & ACCEL_FLAG_PERSISTENT) {
#ifndef SPDK_CONFIG_PMDK
		/* PMDK is required to use this flag. */
		SPDK_ERRLOG("ACCEL_FLAG_PERSISTENT set but PMDK not configured. Configure PMDK or do not use this flag.\n");
		return -EINVAL;
#endif
	}
	return 0;
}

/* Accel framework public API for copy function */
int
spdk_accel_submit_copy(struct spdk_io_channel *ch, void *dst, void *src,
		       uint64_t nbytes, int flags, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;
	struct spdk_accel_module_if *engine = g_engines_opc[ACCEL_OPC_COPY];
	struct spdk_io_channel *engine_ch = accel_ch->engine_ch[ACCEL_OPC_COPY];

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (accel_task == NULL) {
		return -ENOMEM;
	}

	accel_task->dst = dst;
	accel_task->src = src;
	accel_task->op_code = ACCEL_OPC_COPY;
	accel_task->nbytes = nbytes;
	accel_task->flags = flags;

	return engine->submit_tasks(engine_ch, accel_task);
}

/* Accel framework public API for dual cast copy function */
int
spdk_accel_submit_dualcast(struct spdk_io_channel *ch, void *dst1,
			   void *dst2, void *src, uint64_t nbytes, int flags,
			   spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;
	struct spdk_accel_module_if *engine = g_engines_opc[ACCEL_OPC_DUALCAST];
	struct spdk_io_channel *engine_ch = accel_ch->engine_ch[ACCEL_OPC_DUALCAST];

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
	accel_task->flags = flags;
	accel_task->op_code = ACCEL_OPC_DUALCAST;

	return engine->submit_tasks(engine_ch, accel_task);
}

/* Accel framework public API for compare function */
int
spdk_accel_submit_compare(struct spdk_io_channel *ch, void *src1,
			  void *src2, uint64_t nbytes, spdk_accel_completion_cb cb_fn,
			  void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;
	struct spdk_accel_module_if *engine = g_engines_opc[ACCEL_OPC_COMPARE];
	struct spdk_io_channel *engine_ch = accel_ch->engine_ch[ACCEL_OPC_COMPARE];

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (accel_task == NULL) {
		return -ENOMEM;
	}

	accel_task->src = src1;
	accel_task->src2 = src2;
	accel_task->nbytes = nbytes;
	accel_task->op_code = ACCEL_OPC_COMPARE;

	return engine->submit_tasks(engine_ch, accel_task);
}

/* Accel framework public API for fill function */
int
spdk_accel_submit_fill(struct spdk_io_channel *ch, void *dst,
		       uint8_t fill, uint64_t nbytes, int flags,
		       spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;
	struct spdk_accel_module_if *engine = g_engines_opc[ACCEL_OPC_FILL];
	struct spdk_io_channel *engine_ch = accel_ch->engine_ch[ACCEL_OPC_FILL];

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (accel_task == NULL) {
		return -ENOMEM;
	}

	accel_task->dst = dst;
	memset(&accel_task->fill_pattern, fill, sizeof(uint64_t));
	accel_task->nbytes = nbytes;
	accel_task->flags = flags;
	accel_task->op_code = ACCEL_OPC_FILL;

	return engine->submit_tasks(engine_ch, accel_task);
}

/* Accel framework public API for CRC-32C function */
int
spdk_accel_submit_crc32c(struct spdk_io_channel *ch, uint32_t *crc_dst,
			 void *src, uint32_t seed, uint64_t nbytes, spdk_accel_completion_cb cb_fn,
			 void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;
	struct spdk_accel_module_if *engine = g_engines_opc[ACCEL_OPC_CRC32C];
	struct spdk_io_channel *engine_ch = accel_ch->engine_ch[ACCEL_OPC_CRC32C];

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (accel_task == NULL) {
		return -ENOMEM;
	}

	accel_task->crc_dst = crc_dst;
	accel_task->src = src;
	accel_task->v.iovcnt = 0;
	accel_task->seed = seed;
	accel_task->nbytes = nbytes;
	accel_task->op_code = ACCEL_OPC_CRC32C;

	return engine->submit_tasks(engine_ch, accel_task);
}

/* Accel framework public API for chained CRC-32C function */
int
spdk_accel_submit_crc32cv(struct spdk_io_channel *ch, uint32_t *crc_dst,
			  struct iovec *iov, uint32_t iov_cnt, uint32_t seed,
			  spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;
	struct spdk_accel_module_if *engine = g_engines_opc[ACCEL_OPC_CRC32C];
	struct spdk_io_channel *engine_ch = accel_ch->engine_ch[ACCEL_OPC_CRC32C];

	if (iov == NULL) {
		SPDK_ERRLOG("iov should not be NULL");
		return -EINVAL;
	}

	if (!iov_cnt) {
		SPDK_ERRLOG("iovcnt should not be zero value\n");
		return -EINVAL;
	}

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
	accel_task->op_code = ACCEL_OPC_CRC32C;

	return engine->submit_tasks(engine_ch, accel_task);
}

/* Accel framework public API for copy with CRC-32C function */
int
spdk_accel_submit_copy_crc32c(struct spdk_io_channel *ch, void *dst,
			      void *src, uint32_t *crc_dst, uint32_t seed, uint64_t nbytes,
			      int flags, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;
	struct spdk_accel_module_if *engine = g_engines_opc[ACCEL_OPC_COPY_CRC32C];
	struct spdk_io_channel *engine_ch = accel_ch->engine_ch[ACCEL_OPC_COPY_CRC32C];

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
	accel_task->flags = flags;
	accel_task->op_code = ACCEL_OPC_COPY_CRC32C;

	return engine->submit_tasks(engine_ch, accel_task);
}

/* Accel framework public API for chained copy + CRC-32C function */
int
spdk_accel_submit_copy_crc32cv(struct spdk_io_channel *ch, void *dst,
			       struct iovec *src_iovs, uint32_t iov_cnt, uint32_t *crc_dst,
			       uint32_t seed, int flags, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;
	struct spdk_accel_module_if *engine = g_engines_opc[ACCEL_OPC_COPY_CRC32C];
	struct spdk_io_channel *engine_ch = accel_ch->engine_ch[ACCEL_OPC_COPY_CRC32C];
	uint64_t nbytes;
	uint32_t i;

	if (src_iovs == NULL) {
		SPDK_ERRLOG("iov should not be NULL");
		return -EINVAL;
	}

	if (!iov_cnt) {
		SPDK_ERRLOG("iovcnt should not be zero value\n");
		return -EINVAL;
	}

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (accel_task == NULL) {
		SPDK_ERRLOG("no memory\n");
		assert(0);
		return -ENOMEM;
	}

	nbytes = 0;
	for (i = 0; i < iov_cnt; i++) {
		nbytes += src_iovs[i].iov_len;
	}

	accel_task->v.iovs = src_iovs;
	accel_task->v.iovcnt = iov_cnt;
	accel_task->dst = (void *)dst;
	accel_task->crc_dst = crc_dst;
	accel_task->seed = seed;
	accel_task->nbytes = nbytes;
	accel_task->flags = flags;
	accel_task->op_code = ACCEL_OPC_COPY_CRC32C;

	return engine->submit_tasks(engine_ch, accel_task);
}

int
spdk_accel_submit_compress(struct spdk_io_channel *ch, void *dst, void *src, uint64_t nbytes_dst,
			   uint64_t nbytes_src, uint32_t *output_size, int flags,
			   spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;
	struct spdk_accel_module_if *engine = g_engines_opc[ACCEL_OPC_COMPRESS];
	struct spdk_io_channel *engine_ch = accel_ch->engine_ch[ACCEL_OPC_COMPRESS];

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (accel_task == NULL) {
		return -ENOMEM;
	}

	accel_task->output_size = output_size;
	accel_task->src = src;
	accel_task->dst = dst;
	accel_task->nbytes = nbytes_src;
	accel_task->nbytes_dst = nbytes_dst;
	accel_task->flags = flags;
	accel_task->op_code = ACCEL_OPC_COMPRESS;

	return engine->submit_tasks(engine_ch, accel_task);

	return 0;
}

int
spdk_accel_submit_decompress(struct spdk_io_channel *ch, void *dst, void *src, uint64_t nbytes_dst,
			     uint64_t nbytes_src, int flags, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;
	struct spdk_accel_module_if *engine = g_engines_opc[ACCEL_OPC_DECOMPRESS];
	struct spdk_io_channel *engine_ch = accel_ch->engine_ch[ACCEL_OPC_DECOMPRESS];

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (accel_task == NULL) {
		return -ENOMEM;
	}

	accel_task->src = src;
	accel_task->dst = dst;
	accel_task->nbytes = nbytes_src;
	accel_task->nbytes_dst = nbytes_dst;
	accel_task->flags = flags;
	accel_task->op_code = ACCEL_OPC_DECOMPRESS;

	return engine->submit_tasks(engine_ch, accel_task);

	return 0;
}


static struct spdk_accel_module_if *
_module_find_by_name(const char *name)
{
	struct spdk_accel_module_if *accel_module = NULL;

	TAILQ_FOREACH(accel_module, &spdk_accel_module_list, tailq) {
		if (strcmp(name, accel_module->name) == 0) {
			break;
		}
	}

	return accel_module;
}

/* Helper function when when accel modules register with the framework. */
void
spdk_accel_module_list_add(struct spdk_accel_module_if *accel_module)
{
	if (_module_find_by_name(accel_module->name)) {
		SPDK_NOTICELOG("Accel module %s already registered\n", accel_module->name);
		assert(false);
		return;
	}

	/* Make sure that the software module is at the head of the list, this
	 * will assure that all opcodes are later assigned to software first and
	 * then udpated to HW engines as they are registered.
	 */
	if (strcmp(accel_module->name, "software") == 0) {
		TAILQ_INSERT_HEAD(&spdk_accel_module_list, accel_module, tailq);
	} else {
		TAILQ_INSERT_TAIL(&spdk_accel_module_list, accel_module, tailq);
	}

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
	int i, j;

	accel_ch->task_pool_base = calloc(MAX_TASKS_PER_CHANNEL, g_max_accel_module_size);
	if (accel_ch->task_pool_base == NULL) {
		return -ENOMEM;
	}

#ifdef SPDK_CONFIG_ISAL
	isal_deflate_stateless_init(&accel_ch->stream);
	accel_ch->stream.level = 1;
	accel_ch->stream.level_buf = calloc(1, ISAL_DEF_LVL1_DEFAULT);
	if (accel_ch->stream.level_buf == NULL) {
		SPDK_ERRLOG("Could not allocate isal internal buffer\n");
		goto err;
	}
	accel_ch->stream.level_buf_size = ISAL_DEF_LVL1_DEFAULT;
	isal_inflate_init(&accel_ch->state);
#endif

	TAILQ_INIT(&accel_ch->task_pool);
	task_mem = accel_ch->task_pool_base;
	for (i = 0 ; i < MAX_TASKS_PER_CHANNEL; i++) {
		accel_task = (struct spdk_accel_task *)task_mem;
		TAILQ_INSERT_TAIL(&accel_ch->task_pool, accel_task, link);
		task_mem += g_max_accel_module_size;
	}

	/* Assign engines and get IO channels for each */
	for (i = 0; i < ACCEL_OPC_LAST; i++) {
		accel_ch->engine_ch[i] = g_engines_opc[i]->get_io_channel();
		/* This can happen if idxd runs out of channels. */
		if (accel_ch->engine_ch[i] == NULL) {
			goto err2;
		}
	}

	return 0;
err2:
	for (j = 0; j < i; j++) {
		spdk_put_io_channel(accel_ch->engine_ch[j]);
	}
#ifdef SPDK_CONFIG_ISAL
	free(accel_ch->stream.level_buf);
err:
#endif
	free(accel_ch->task_pool_base);
	return -ENOMEM;
}

/* Framework level channel destroy callback. */
static void
accel_engine_destroy_cb(void *io_device, void *ctx_buf)
{
	struct accel_io_channel	*accel_ch = ctx_buf;
	int i;

	for (i = 0; i < ACCEL_OPC_LAST; i++) {
		assert(accel_ch->engine_ch[i] != NULL);
		spdk_put_io_channel(accel_ch->engine_ch[i]);
		accel_ch->engine_ch[i] = NULL;
	}

#ifdef SPDK_CONFIG_ISAL
	free(accel_ch->stream.level_buf);
#endif
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
	enum accel_opcode op;
	struct spdk_accel_module_if *accel_module = NULL;

	g_engine_started = true;
	accel_engine_module_initialize();

	/* Create our priority global map of opcodes to engines, we populate starting
	 * with the software engine (guaranteed to be first on the list) and then
	 * updating opcodes with HW engines that have been initilaized.
	 * NOTE: all opcodes must be suported by software in the event that no HW
	 * engines are initilaized to support the operation.
	 */
	TAILQ_FOREACH(accel_module, &spdk_accel_module_list, tailq) {
		for (op = 0; op < ACCEL_OPC_LAST; op++) {
			if (accel_module->supports_opcode(op)) {
				g_engines_opc[op] = accel_module;
				SPDK_DEBUGLOG(accel, "OPC 0x%x now assigned to %s\n", op, accel_module->name);
			}
		}
	}

	/* Now lets check for overrides and apply all that exist */
	for (op = 0; op < ACCEL_OPC_LAST; op++) {
		if (g_engines_opc_override[op] != NULL) {
			accel_module = _module_find_by_name(g_engines_opc_override[op]);
			if (accel_module == NULL) {
				SPDK_ERRLOG("Invalid module name of %s\n", g_engines_opc_override[op]);
				return -EINVAL;
			}
			if (accel_module->supports_opcode(op) == false) {
				SPDK_ERRLOG("Engine %s does not support op code %d\n", accel_module->name, op);
				return -EINVAL;
			}
			g_engines_opc[op] = accel_module;
		}
	}

#ifdef DEBUG
	for (op = 0; op < ACCEL_OPC_LAST; op++) {
		assert(g_engines_opc[op] != NULL);
	}
#endif
	/*
	 * We need a unique identifier for the accel engine framework, so use the
	 * spdk_accel_module_list address for this purpose.
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
	enum accel_opcode op;

	assert(cb_fn != NULL);

	g_fini_cb_fn = cb_fn;
	g_fini_cb_arg = cb_arg;

	for (op = 0; op < ACCEL_OPC_LAST; op++) {
		if (g_engines_opc_override[op] != NULL) {
			free(g_engines_opc_override[op]);
			g_engines_opc_override[op] = NULL;
		}
		g_engines_opc[op] = NULL;
	}

	spdk_io_device_unregister(&spdk_accel_module_list, NULL);
	spdk_accel_engine_module_finish();
}

/*
 * The SW Accelerator module is "built in" here (rest of file)
 */
static bool
sw_accel_supports_opcode(enum accel_opcode opc)
{
	switch (opc) {
	case ACCEL_OPC_COPY:
	case ACCEL_OPC_FILL:
	case ACCEL_OPC_DUALCAST:
	case ACCEL_OPC_COMPARE:
	case ACCEL_OPC_CRC32C:
	case ACCEL_OPC_COPY_CRC32C:
	case ACCEL_OPC_COMPRESS:
	case ACCEL_OPC_DECOMPRESS:
		return true;
	default:
		return false;
	}
}

static inline void
_pmem_memcpy(void *dst, const void *src, size_t len)
{
#ifdef SPDK_CONFIG_PMDK
	int is_pmem = pmem_is_pmem(dst, len);

	if (is_pmem) {
		pmem_memcpy_persist(dst, src, len);
	} else {
		memcpy(dst, src, len);
		pmem_msync(dst, len);
	}
#else
	SPDK_ERRLOG("Function not defined without SPDK_CONFIG_PMDK enabled.\n");
	assert(0);
#endif
}

static void
_sw_accel_dualcast(void *dst1, void *dst2, void *src, size_t nbytes, int flags)
{
	if (flags & ACCEL_FLAG_PERSISTENT) {
		_pmem_memcpy(dst1, src, nbytes);
		_pmem_memcpy(dst2, src, nbytes);
	} else {
		memcpy(dst1, src, nbytes);
		memcpy(dst2, src, nbytes);
	}
}

static void
_sw_accel_copy(void *dst, void *src, size_t nbytes, int flags)
{

	if (flags & ACCEL_FLAG_PERSISTENT) {
		_pmem_memcpy(dst, src, nbytes);
	} else {
		memcpy(dst, src, nbytes);
	}
}

static void
_sw_accel_copyv(void *dst, struct iovec *iov, uint32_t iovcnt, int flags)
{
	uint32_t i;

	for (i = 0; i < iovcnt; i++) {
		assert(iov[i].iov_base != NULL);
		if (flags & ACCEL_FLAG_PERSISTENT) {
			_pmem_memcpy(dst, iov[i].iov_base, (size_t)iov[i].iov_len);
		} else {
			memcpy(dst, iov[i].iov_base, (size_t)iov[i].iov_len);
		}
		dst += iov[i].iov_len;
	}
}

static int
_sw_accel_compare(void *src1, void *src2, size_t nbytes)
{
	return memcmp(src1, src2, nbytes);
}

static void
_sw_accel_fill(void *dst, uint8_t fill, size_t nbytes, int flags)
{
	if (flags & ACCEL_FLAG_PERSISTENT) {
#ifdef SPDK_CONFIG_PMDK
		int is_pmem = pmem_is_pmem(dst, nbytes);

		if (is_pmem) {
			pmem_memset_persist(dst, fill, nbytes);
		} else {
			memset(dst, fill, nbytes);
			pmem_msync(dst, nbytes);
		}
#else
		SPDK_ERRLOG("Function not defined without SPDK_CONFIG_PMDK enabled.\n");
		assert(0);
#endif
	} else {
		memset(dst, fill, nbytes);
	}
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

static int
_sw_accel_compress(struct spdk_accel_task *accel_task)
{
#ifdef SPDK_CONFIG_ISAL
	struct accel_io_channel *accel_ch = accel_task->accel_ch;

	accel_ch->stream.next_in = accel_task->src;
	accel_ch->stream.next_out = accel_task->dst;
	accel_ch->stream.avail_in = accel_task->nbytes;
	accel_ch->stream.avail_out = accel_task->nbytes_dst;

	isal_deflate_stateless(&accel_ch->stream);
	if (accel_task->output_size != NULL) {
		assert(accel_task->nbytes_dst > accel_ch->stream.avail_out);
		*accel_task->output_size = accel_task->nbytes_dst - accel_ch->stream.avail_out;
	}

	return 0;
#else
	SPDK_ERRLOG("ISAL option is required to use software compression.\n");
	return -EINVAL;
#endif
}

static int
_sw_accel_decompress(struct spdk_accel_task *accel_task)
{
#ifdef SPDK_CONFIG_ISAL
	struct accel_io_channel *accel_ch = accel_task->accel_ch;
	int rc;

	accel_ch->state.next_in = accel_task->src;
	accel_ch->state.avail_in = accel_task->nbytes;
	accel_ch->state.next_out = accel_task->dst;
	accel_ch->state.avail_out = accel_task->nbytes_dst;

	rc = isal_inflate_stateless(&accel_ch->state);
	if (rc) {
		SPDK_ERRLOG("isal_inflate_stateless retunred error %d.\n", rc);
	}
	return rc;
#else
	SPDK_ERRLOG("ISAL option is required to use software decompression.\n");
	return -EINVAL;
#endif
}

static int
sw_accel_submit_tasks(struct spdk_io_channel *ch, struct spdk_accel_task *accel_task)
{
	struct sw_accel_io_channel *sw_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *tmp;
	int rc = 0;

	do {
		switch (accel_task->op_code) {
		case ACCEL_OPC_COPY:
			rc = _check_flags(accel_task->flags);
			if (rc == 0) {
				_sw_accel_copy(accel_task->dst, accel_task->src, accel_task->nbytes, accel_task->flags);
			}
			break;
		case ACCEL_OPC_FILL:
			rc = _check_flags(accel_task->flags);
			if (rc == 0) {
				_sw_accel_fill(accel_task->dst, accel_task->fill_pattern, accel_task->nbytes, accel_task->flags);
			}
			break;
		case ACCEL_OPC_DUALCAST:
			rc = _check_flags(accel_task->flags);
			if (rc == 0) {
				_sw_accel_dualcast(accel_task->dst, accel_task->dst2, accel_task->src, accel_task->nbytes,
						   accel_task->flags);
			}
			break;
		case ACCEL_OPC_COMPARE:
			rc = _sw_accel_compare(accel_task->src, accel_task->src2, accel_task->nbytes);
			break;
		case ACCEL_OPC_CRC32C:
			if (accel_task->v.iovcnt == 0) {
				_sw_accel_crc32c(accel_task->crc_dst, accel_task->src, accel_task->seed, accel_task->nbytes);
			} else {
				_sw_accel_crc32cv(accel_task->crc_dst, accel_task->v.iovs, accel_task->v.iovcnt, accel_task->seed);
			}
			break;
		case ACCEL_OPC_COPY_CRC32C:
			rc = _check_flags(accel_task->flags);
			if (rc == 0) {
				if (accel_task->v.iovcnt == 0) {
					_sw_accel_copy(accel_task->dst, accel_task->src, accel_task->nbytes, accel_task->flags);
					_sw_accel_crc32c(accel_task->crc_dst, accel_task->src, accel_task->seed, accel_task->nbytes);
				} else {
					_sw_accel_copyv(accel_task->dst, accel_task->v.iovs, accel_task->v.iovcnt, accel_task->flags);
					_sw_accel_crc32cv(accel_task->crc_dst, accel_task->v.iovs, accel_task->v.iovcnt, accel_task->seed);
				}
			}
			break;
		case ACCEL_OPC_COMPRESS:
			rc = _sw_accel_compress(accel_task);
			break;
		case ACCEL_OPC_DECOMPRESS:
			rc = _sw_accel_decompress(accel_task);
			break;
		default:
			assert(false);
			break;
		}

		tmp = TAILQ_NEXT(accel_task, link);

		_add_to_comp_list(sw_ch, accel_task, rc);

		accel_task = tmp;
	} while (accel_task);

	return 0;
}

static struct spdk_io_channel *sw_accel_get_io_channel(void);
static int sw_accel_engine_init(void);
static void sw_accel_engine_fini(void *ctxt);
static size_t sw_accel_engine_get_ctx_size(void);

static struct spdk_accel_module_if g_sw_module = {
	.module_init = sw_accel_engine_init,
	.module_fini = sw_accel_engine_fini,
	.write_config_json = NULL,
	.get_ctx_size = sw_accel_engine_get_ctx_size,
	.name			= "software",
	.supports_opcode	= sw_accel_supports_opcode,
	.get_io_channel		= sw_accel_get_io_channel,
	.submit_tasks		= sw_accel_submit_tasks
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

static struct spdk_io_channel *
sw_accel_get_io_channel(void)
{
	return spdk_get_io_channel(&g_sw_module);
}

static size_t
sw_accel_engine_get_ctx_size(void)
{
	return sizeof(struct spdk_accel_task);
}

static int
sw_accel_engine_init(void)
{
	spdk_io_device_register(&g_sw_module, sw_accel_create_cb, sw_accel_destroy_cb,
				sizeof(struct sw_accel_io_channel), "sw_accel_engine");

	return 0;
}

static void
sw_accel_engine_fini(void *ctxt)
{
	spdk_io_device_unregister(&g_sw_module, NULL);

	spdk_accel_engine_module_finish();
}

SPDK_LOG_REGISTER_COMPONENT(accel)

SPDK_ACCEL_MODULE_REGISTER(sw, &g_sw_module)
