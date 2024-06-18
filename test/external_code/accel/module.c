/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2023 Intel Corporation.
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#include "spdk/accel.h"
#include "spdk/accel_module.h"
#include "spdk/thread.h"

static struct spdk_accel_module_if g_ex_module;

struct ex_accel_io_channel {
	struct spdk_poller *completion_poller;
	STAILQ_HEAD(, spdk_accel_task) tasks_to_complete;
};

static int
ex_accel_copy_iovs(struct iovec *dst_iovs, uint32_t dst_iovcnt,
		   struct iovec *src_iovs, uint32_t src_iovcnt)
{
	struct spdk_ioviter iter;
	void *src, *dst;
	size_t len;

	for (len = spdk_ioviter_first(&iter, src_iovs, src_iovcnt,
				      dst_iovs, dst_iovcnt, &src, &dst);
	     len != 0;
	     len = spdk_ioviter_next(&iter, &src, &dst)) {
		memcpy(dst, src, len);
	}

	return 0;
}

static int
ex_accel_compare(struct iovec *src_iovs, uint32_t src_iovcnt,
		 struct iovec *src2_iovs, uint32_t src2_iovcnt)
{
	if (spdk_unlikely(src_iovcnt != 1 || src2_iovcnt != 1)) {
		return -EINVAL;
	}

	if (spdk_unlikely(src_iovs[0].iov_len != src2_iovs[0].iov_len)) {
		return -EINVAL;
	}

	return memcmp(src_iovs[0].iov_base, src2_iovs[0].iov_base, src_iovs[0].iov_len);
}

static int
ex_accel_fill(struct iovec *iovs, uint32_t iovcnt, uint8_t fill)
{
	void *dst;
	size_t nbytes;

	if (spdk_unlikely(iovcnt != 1)) {
		fprintf(stderr, "Unexpected number of iovs: %" PRIu32 "\n", iovcnt);
		return -EINVAL;
	}

	dst = iovs[0].iov_base;
	nbytes = iovs[0].iov_len;

	memset(dst, fill, nbytes);

	return 0;
}

static int
ex_accel_comp_poll(void *arg)
{
	struct ex_accel_io_channel *ex_ch = arg;
	STAILQ_HEAD(, spdk_accel_task) tasks_to_complete;
	struct spdk_accel_task *accel_task;

	if (STAILQ_EMPTY(&ex_ch->tasks_to_complete)) {
		return SPDK_POLLER_IDLE;
	}

	STAILQ_INIT(&tasks_to_complete);
	STAILQ_SWAP(&tasks_to_complete, &ex_ch->tasks_to_complete, spdk_accel_task);

	while ((accel_task = STAILQ_FIRST(&tasks_to_complete))) {
		STAILQ_REMOVE_HEAD(&tasks_to_complete, link);
		spdk_accel_task_complete(accel_task, accel_task->status);
	}

	return SPDK_POLLER_BUSY;
}

static int
ex_accel_create_cb(void *io_device, void *ctx_buf)
{
	struct ex_accel_io_channel *ex_ch = ctx_buf;

	STAILQ_INIT(&ex_ch->tasks_to_complete);
	ex_ch->completion_poller = SPDK_POLLER_REGISTER(ex_accel_comp_poll, ex_ch, 0);

	return 0;
}

static void
ex_accel_destroy_cb(void *io_device, void *ctx_buf)
{
	struct ex_accel_io_channel *ex_ch = ctx_buf;

	spdk_poller_unregister(&ex_ch->completion_poller);
}

static int
ex_accel_module_init(void)
{
	spdk_io_device_register(&g_ex_module, ex_accel_create_cb, ex_accel_destroy_cb,
				sizeof(struct ex_accel_io_channel), "external_accel_module");

	return 0;
}

static void
ex_accel_module_fini(void *ctx)
{
	spdk_io_device_unregister(&g_ex_module, NULL);
	spdk_accel_module_finish();
}

static size_t
ex_accel_module_get_ctx_size(void)
{
	return sizeof(struct spdk_accel_task);
}

inline static void
add_to_comp_list(struct ex_accel_io_channel *ex_ch, struct spdk_accel_task *accel_task)
{
	STAILQ_INSERT_TAIL(&ex_ch->tasks_to_complete, accel_task, link);
}

static bool
ex_accel_supports_opcode(enum spdk_accel_opcode opc)
{
	switch (opc) {
	case SPDK_ACCEL_OPC_COPY:
	case SPDK_ACCEL_OPC_FILL:
	case SPDK_ACCEL_OPC_COMPARE:
		return true;
	default:
		return false;
	}
}

static struct spdk_io_channel *
ex_accel_get_io_channel(void)
{
	return spdk_get_io_channel(&g_ex_module);
}

static int
ex_accel_submit_tasks(struct spdk_io_channel *ch, struct spdk_accel_task *accel_task)
{
	struct ex_accel_io_channel *ex_ch = spdk_io_channel_get_ctx(ch);

	printf("Running on accel module task with code: %" PRIu8 "\n", accel_task->op_code);
	switch (accel_task->op_code) {
	case SPDK_ACCEL_OPC_COPY:
		accel_task->status = ex_accel_copy_iovs(accel_task->d.iovs, accel_task->d.iovcnt,
							accel_task->s.iovs, accel_task->s.iovcnt);
		break;
	case SPDK_ACCEL_OPC_FILL:
		accel_task->status = ex_accel_fill(accel_task->d.iovs, accel_task->d.iovcnt,
						   accel_task->fill_pattern);
		break;
	case SPDK_ACCEL_OPC_COMPARE:
		accel_task->status = ex_accel_compare(accel_task->s.iovs, accel_task->s.iovcnt,
						      accel_task->s2.iovs, accel_task->s2.iovcnt);
		break;
	default:
		fprintf(stderr, "Unsupported accel opcode: %" PRIu8 "\n", accel_task->op_code);
		accel_task->status = 1;
		break;
	}

	add_to_comp_list(ex_ch, accel_task);

	return accel_task->status;
}

static struct spdk_accel_module_if g_ex_module = {
	.module_init			= ex_accel_module_init,
	.module_fini			= ex_accel_module_fini,
	.get_ctx_size			= ex_accel_module_get_ctx_size,
	.name				= "external",
	.supports_opcode		= ex_accel_supports_opcode,
	.get_io_channel			= ex_accel_get_io_channel,
	.submit_tasks			= ex_accel_submit_tasks,
};

SPDK_ACCEL_MODULE_REGISTER(external, &g_ex_module)
