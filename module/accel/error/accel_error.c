/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (C) 2023 Intel Corporation. All rights reserved.
 */

#include "accel_error.h"
#include "spdk/accel.h"
#include "spdk/accel_module.h"
#include "spdk/json.h"
#include "spdk/thread.h"

struct accel_error_inject_info {
	/* Error injection options */
	struct accel_error_inject_opts	opts;
	/* Number of errors already injected on this channel */
	uint64_t			count;
	/* Number of operations executed since last error injection */
	uint64_t			interval;
};

struct accel_error_task;

struct accel_error_channel {
	struct spdk_io_channel		*swch;
	struct spdk_poller		*poller;
	struct accel_error_inject_info	injects[SPDK_ACCEL_OPC_LAST];
	STAILQ_HEAD(, accel_error_task)	tasks;
};

struct accel_error_task {
	struct accel_error_channel		*ch;
	union {
		spdk_accel_completion_cb	cpl;
		spdk_accel_step_cb		step;
	} cb_fn;
	void					*cb_arg;
	int					status;
	STAILQ_ENTRY(accel_error_task)		link;
};

static struct spdk_accel_module_if *g_sw_module;
static struct accel_error_inject_opts g_injects[SPDK_ACCEL_OPC_LAST];
static size_t g_task_offset;

static struct accel_error_task *
accel_error_get_task_ctx(struct spdk_accel_task *task)
{
	return (void *)((uint8_t *)task + g_task_offset);
}

static struct spdk_accel_task *
accel_error_get_task_from_ctx(struct accel_error_task *errtask)
{
	return (void *)((uint8_t *)errtask - g_task_offset);
}

static void
accel_error_corrupt_task(struct spdk_accel_task *task)
{
	switch (task->op_code) {
	case SPDK_ACCEL_OPC_CRC32C:
		*task->crc_dst += 1;
		break;
	default:
		break;
	}
}

static void
accel_error_corrupt_cb(void *arg, int status)
{
	struct spdk_accel_task *task = arg;
	struct accel_error_task *errtask = accel_error_get_task_ctx(task);
	spdk_accel_completion_cb cb_fn = errtask->cb_fn.cpl;
	void *cb_arg = errtask->cb_arg;

	accel_error_corrupt_task(task);
	cb_fn(cb_arg, status);
}

static void
accel_error_corrupt_step_cb(void *arg)
{
	struct spdk_accel_task *task = arg;
	struct accel_error_task *errtask = accel_error_get_task_ctx(task);
	spdk_accel_step_cb cb_fn = errtask->cb_fn.step;
	void *cb_arg = errtask->cb_arg;

	accel_error_corrupt_task(task);

	cb_fn(cb_arg);
}

static bool
accel_error_should_inject(struct spdk_io_channel *ch, struct spdk_accel_task *task)
{
	struct accel_error_channel *errch = spdk_io_channel_get_ctx(ch);
	struct accel_error_inject_info *info = &errch->injects[task->op_code];

	if (info->opts.type == ACCEL_ERROR_INJECT_DISABLE) {
		return false;
	}

	info->interval++;
	if (info->interval >= info->opts.interval) {
		info->interval = 0;
		info->count++;

		if (info->count <= info->opts.count) {
			return true;
		} else {
			info->opts.type = ACCEL_ERROR_INJECT_DISABLE;
			info->interval = 0;
			info->count = 0;
		}
	}

	return false;
}

static int
accel_error_submit_tasks(struct spdk_io_channel *ch, struct spdk_accel_task *task)
{
	struct accel_error_channel *errch = spdk_io_channel_get_ctx(ch);
	struct accel_error_task *errtask = accel_error_get_task_ctx(task);
	struct accel_error_inject_info *info = &errch->injects[task->op_code];

	if (!accel_error_should_inject(ch, task)) {
		goto submit;
	}

	switch (info->opts.type) {
	case ACCEL_ERROR_INJECT_CORRUPT:
		errtask->ch = errch;
		errtask->cb_arg = task->cb_arg;
		task->cb_arg = task;
		if (task->seq != NULL) {
			errtask->cb_fn.step = task->step_cb_fn;
			task->step_cb_fn = accel_error_corrupt_step_cb;
		} else {
			errtask->cb_fn.cpl = task->cb_fn;
			task->cb_fn = accel_error_corrupt_cb;
		}
		break;
	case ACCEL_ERROR_INJECT_FAILURE:
		errtask->status = info->opts.errcode;
		STAILQ_INSERT_TAIL(&errch->tasks, errtask, link);
		return 0;
	default:
		break;
	}
submit:
	return g_sw_module->submit_tasks(errch->swch, task);
}

static int
accel_error_poller(void *arg)
{
	struct accel_error_channel *errch = arg;
	struct accel_error_task *errtask;
	STAILQ_HEAD(, accel_error_task) tasks;
	struct spdk_accel_task *task;

	if (STAILQ_EMPTY(&errch->tasks)) {
		return SPDK_POLLER_IDLE;
	}

	STAILQ_INIT(&tasks);
	STAILQ_SWAP(&tasks, &errch->tasks, accel_error_task);

	while (!STAILQ_EMPTY(&tasks)) {
		errtask = STAILQ_FIRST(&tasks);
		STAILQ_REMOVE_HEAD(&tasks, link);

		task = accel_error_get_task_from_ctx(errtask);
		spdk_accel_task_complete(task, errtask->status);
	}

	return SPDK_POLLER_BUSY;
}

static void
accel_error_inject_channel(struct spdk_io_channel_iter *iter)
{
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(iter);
	struct accel_error_channel *errch = spdk_io_channel_get_ctx(ch);
	struct accel_error_inject_opts *opts = spdk_io_channel_iter_get_ctx(iter);
	struct accel_error_inject_info *info = &errch->injects[opts->opcode];

	info->count = 0;
	memcpy(&info->opts, opts, sizeof(info->opts));

	spdk_for_each_channel_continue(iter, 0);
}

static bool accel_error_supports_opcode(enum spdk_accel_opcode opcode);

int
accel_error_inject_error(struct accel_error_inject_opts *opts)
{
	struct accel_error_inject_opts *curr = &g_injects[opts->opcode];

	if (!accel_error_supports_opcode(opts->opcode)) {
		return -EINVAL;
	}

	memcpy(curr, opts, sizeof(*opts));
	if (curr->type == ACCEL_ERROR_INJECT_DISABLE) {
		curr->count = 0;
	}
	if (curr->count == 0) {
		curr->type = ACCEL_ERROR_INJECT_DISABLE;
	}

	spdk_for_each_channel(&g_sw_module, accel_error_inject_channel, curr, NULL);

	return 0;
}

static int
accel_error_channel_create_cb(void *io_device, void *ctx)
{
	struct accel_error_channel *errch = ctx;
	size_t i;

	STAILQ_INIT(&errch->tasks);
	errch->poller = SPDK_POLLER_REGISTER(accel_error_poller, errch, 0);
	if (errch->poller == NULL) {
		return -ENOMEM;
	}

	errch->swch = g_sw_module->get_io_channel();
	if (errch->swch == NULL) {
		spdk_poller_unregister(&errch->poller);
		return -ENOMEM;
	}

	for (i = 0; i < SPDK_COUNTOF(errch->injects); ++i) {
		memcpy(&errch->injects[i].opts, &g_injects[i], sizeof(g_injects[i]));
		errch->injects[i].count = 0;
	}

	return 0;
}

static void
accel_error_channel_destroy_cb(void *io_device, void *ctx)
{
	struct accel_error_channel *errch = ctx;

	assert(STAILQ_EMPTY(&errch->tasks));
	spdk_poller_unregister(&errch->poller);
	spdk_put_io_channel(errch->swch);
}

static int
accel_error_module_init(void)
{
	g_sw_module = spdk_accel_get_module("software");
	if (g_sw_module == NULL) {
		/* Should never really happen */
		return -ENOTSUP;
	}

	g_task_offset = g_sw_module->get_ctx_size();

	spdk_io_device_register(&g_sw_module, accel_error_channel_create_cb,
				accel_error_channel_destroy_cb,
				sizeof(struct accel_error_channel), "accel_error");

	return 0;
}

static void
accel_error_unregister_cb(void *unused)
{
	spdk_accel_module_finish();
}

static void
accel_error_module_fini(void *unused)
{
	spdk_io_device_unregister(&g_sw_module, accel_error_unregister_cb);
}

static bool
accel_error_supports_opcode(enum spdk_accel_opcode opcode)
{
	switch (opcode) {
	case SPDK_ACCEL_OPC_CRC32C:
		return true;
	default:
		return false;
	}
}

static struct spdk_io_channel *
accel_error_get_io_channel(void)
{
	return spdk_get_io_channel(&g_sw_module);
}

static size_t
accel_error_get_ctx_size(void)
{
	return g_task_offset + sizeof(struct accel_error_task);
}

const char *
accel_error_get_type_name(enum accel_error_inject_type type)
{
	const char *typenames[] = {
		[ACCEL_ERROR_INJECT_DISABLE] = "disable",
		[ACCEL_ERROR_INJECT_CORRUPT] = "corrupt",
		[ACCEL_ERROR_INJECT_FAILURE] = "failure",
		[ACCEL_ERROR_INJECT_MAX] = NULL
	};

	if ((int)type >= ACCEL_ERROR_INJECT_MAX) {
		return NULL;
	}

	return typenames[type];
}

static void
accel_error_write_config_json(struct spdk_json_write_ctx *w)
{
	struct accel_error_inject_opts *opts;
	int opcode;

	for (opcode = 0; opcode < SPDK_ACCEL_OPC_LAST; ++opcode) {
		opts = &g_injects[opcode];
		if (opts->type == ACCEL_ERROR_INJECT_DISABLE) {
			continue;
		}
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "accel_error_inject_error");
		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "opcode", spdk_accel_get_opcode_name(opcode));
		spdk_json_write_named_string(w, "type", accel_error_get_type_name(opts->type));
		spdk_json_write_named_uint64(w, "count", opts->count);
		spdk_json_write_named_uint64(w, "interval", opts->interval);
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);
	}
}

static struct spdk_accel_module_if g_accel_error_module = {
	.name			= "error",
	.priority		= INT_MIN,
	.module_init		= accel_error_module_init,
	.module_fini		= accel_error_module_fini,
	.supports_opcode	= accel_error_supports_opcode,
	.get_ctx_size		= accel_error_get_ctx_size,
	.get_io_channel		= accel_error_get_io_channel,
	.submit_tasks		= accel_error_submit_tasks,
	.write_config_json	= accel_error_write_config_json,
};
SPDK_ACCEL_MODULE_REGISTER(error, &g_accel_error_module)
