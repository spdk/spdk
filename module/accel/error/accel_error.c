/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (C) 2023 Intel Corporation. All rights reserved.
 */

#include "spdk/accel.h"
#include "spdk/accel_module.h"
#include "spdk/thread.h"

struct accel_error_channel {
	struct spdk_io_channel *swch;
};

struct accel_error_task {
	spdk_accel_completion_cb	cb_fn;
	void				*cb_arg;
};

static struct spdk_accel_module_if *g_sw_module;
static size_t g_task_offset;

static struct accel_error_task *
accel_error_get_task_ctx(struct spdk_accel_task *task)
{
	return (void *)((uint8_t *)task + g_task_offset);
}

static void
accel_error_task_complete_cb(void *arg, int status)
{
	struct spdk_accel_task *task = arg;
	struct accel_error_task *errtask = accel_error_get_task_ctx(task);
	spdk_accel_completion_cb cb_fn = errtask->cb_fn;
	void *cb_arg = errtask->cb_arg;

	cb_fn(cb_arg, status);
}

static int
accel_error_submit_tasks(struct spdk_io_channel *ch, struct spdk_accel_task *task)
{
	struct accel_error_channel *errch = spdk_io_channel_get_ctx(ch);
	struct accel_error_task *errtask = accel_error_get_task_ctx(task);

	errtask->cb_fn = task->cb_fn;
	errtask->cb_arg = task->cb_arg;
	task->cb_fn = accel_error_task_complete_cb;
	task->cb_arg = task;

	return g_sw_module->submit_tasks(errch->swch, task);
}

static int
accel_error_channel_create_cb(void *io_device, void *ctx)
{
	struct accel_error_channel *errch = ctx;

	errch->swch = g_sw_module->get_io_channel();
	if (errch->swch == NULL) {
		return -ENOMEM;
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
		return -ENODEV;
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
	return false;
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

static struct spdk_accel_module_if g_accel_error_module = {
	.name			= "error",
	.priority		= INT_MIN,
	.module_init		= accel_error_module_init,
	.module_fini		= accel_error_module_fini,
	.supports_opcode	= accel_error_supports_opcode,
	.get_ctx_size		= accel_error_get_ctx_size,
	.get_io_channel		= accel_error_get_io_channel,
	.submit_tasks		= accel_error_submit_tasks,
};
SPDK_ACCEL_MODULE_REGISTER(error, &g_accel_error_module)
