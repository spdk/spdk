/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2023 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/accel.h"
#include "spdk/accel_module.h"
#include "spdk/thread.h"

static struct spdk_accel_driver g_ex_driver;

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
ex_driver_create_cb(void *io_device, void *ctx_buf)
{
	return 0;
}

static void
ex_driver_destroy_cb(void *io_device, void *ctx_buf)
{
}

static int
ex_driver_execute_sequence(struct spdk_io_channel *ch, struct spdk_accel_sequence *seq)
{
	struct spdk_accel_task *task;
	int rc;

	while ((task = spdk_accel_sequence_first_task(seq)) != NULL) {
		printf("Running on accel driver task with code: %" PRIu8 "\n", task->op_code);
		if (task->op_code == SPDK_ACCEL_OPC_FILL) {
			rc = ex_accel_fill(task->d.iovs, task->d.iovcnt, task->fill_pattern);
			if (rc != 0) {
				fprintf(stderr, "Error during filling buffer: %d\n", rc);
			}
		} else {
			break;
		}

		spdk_accel_task_complete(task, rc);
		if (rc != 0) {
			break;
		}
	}

	spdk_accel_sequence_continue(seq);

	return 0;
}

static struct spdk_io_channel *
ex_driver_get_io_channel(void)
{
	return spdk_get_io_channel(&g_ex_driver);
}

static int
ex_accel_driver_init(void)
{
	spdk_io_device_register(&g_ex_driver, ex_driver_create_cb, ex_driver_destroy_cb,
				0, "external_accel_driver");

	return 0;
}

static void
ex_accel_driver_fini(void)
{
	spdk_io_device_unregister(&g_ex_driver, NULL);
}

static struct spdk_accel_driver g_ex_driver = {
	.name = "external",
	.execute_sequence = ex_driver_execute_sequence,
	.get_io_channel = ex_driver_get_io_channel,
	.init = ex_accel_driver_init,
	.fini = ex_accel_driver_fini
};

SPDK_ACCEL_DRIVER_REGISTER(external, &g_ex_driver);
