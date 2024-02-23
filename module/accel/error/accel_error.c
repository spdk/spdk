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

struct accel_error_channel {
	struct spdk_io_channel		*swch;
	struct accel_error_inject_info	injects[SPDK_ACCEL_OPC_LAST];
};

struct accel_error_task {
	struct accel_error_channel	*ch;
	spdk_accel_completion_cb	cb_fn;
	void				*cb_arg;
};

static struct spdk_accel_module_if *g_sw_module;
static struct accel_error_inject_opts g_injects[SPDK_ACCEL_OPC_LAST];
static size_t g_task_offset;

static struct accel_error_task *
accel_error_get_task_ctx(struct spdk_accel_task *task)
{
	return (void *)((uint8_t *)task + g_task_offset);
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
accel_error_task_complete_cb(void *arg, int status)
{
	struct spdk_accel_task *task = arg;
	struct accel_error_task *errtask = accel_error_get_task_ctx(task);
	struct accel_error_channel *ch = errtask->ch;
	struct accel_error_inject_info *info = &ch->injects[task->op_code];
	spdk_accel_completion_cb cb_fn = errtask->cb_fn;
	void *cb_arg = errtask->cb_arg;

	info->interval++;
	if (info->interval >= info->opts.interval) {
		info->interval = 0;
		info->count++;

		if (info->count <= info->opts.count) {
			switch (info->opts.type) {
			case ACCEL_ERROR_INJECT_CORRUPT:
				accel_error_corrupt_task(task);
				break;
			case ACCEL_ERROR_INJECT_FAILURE:
				status = info->opts.errcode;
				break;
			default:
				break;
			}
		} else {
			info->opts.type = ACCEL_ERROR_INJECT_DISABLE;
		}
	}

	cb_fn(cb_arg, status);
}

static int
accel_error_submit_tasks(struct spdk_io_channel *ch, struct spdk_accel_task *task)
{
	struct accel_error_channel *errch = spdk_io_channel_get_ctx(ch);
	struct accel_error_task *errtask = accel_error_get_task_ctx(task);

	errtask->ch = errch;
	errtask->cb_fn = task->cb_fn;
	errtask->cb_arg = task->cb_arg;
	task->cb_fn = accel_error_task_complete_cb;
	task->cb_arg = task;

	return g_sw_module->submit_tasks(errch->swch, task);
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

	errch->swch = g_sw_module->get_io_channel();
	if (errch->swch == NULL) {
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
