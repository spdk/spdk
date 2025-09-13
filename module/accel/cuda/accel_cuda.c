/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES.
 *   Copyright (c) 2025 StarWind Software, Inc.
 *   All rights reserved.
 */

#include "spdk/accel_module.h"
#include "spdk/log.h"
#include "spdk/likely.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/thread.h"
#include "spdk/json.h"
#include "spdk/xor.h"

#include <cuda_runtime_api.h>
#include "accel_cuda_kern.h"
#include "accel_cuda.h"
#include "cuda_utils.h"

SPDK_STATIC_ASSERT(CUDA_XOR_MAX_SOURCES % 16 == 0, "Not aligned!");

static bool g_accel_cuda_enable = false;
static bool g_accel_cuda_initialized = false;

static struct cuda_mem_map *g_accel_cuda_mem_map = NULL;

struct cuda_task {
	struct spdk_accel_task	base;
	STAILQ_ENTRY(cuda_task)	link;
};

struct cuda_stream {
	STAILQ_ENTRY(cuda_stream) link;
	struct cuda_task *task;
	cudaStream_t stream;
	void **inputs;
	char *status;
};

struct cuda_io_channel {
	STAILQ_HEAD(, cuda_task) waiting_tasks;
	STAILQ_HEAD(, cuda_stream) idle_streams;

	struct spdk_poller *poller;
	struct cuda_stream *streams;

	uint32_t num_streams;
	uint32_t num_running_tasks;

	char *inputs_buf;
	char *status_buf;
};

static int accel_cuda_init(void);
static void accel_cuda_exit(void *ctx);
static void accel_cuda_write_config_json(struct spdk_json_write_ctx *w);
static bool accel_cuda_supports_opcode(enum spdk_accel_opcode opc);
static struct spdk_io_channel *accel_cuda_get_io_channel(void);
static int accel_cuda_submit_tasks(struct spdk_io_channel *ch, struct spdk_accel_task *accel_task);

static size_t
accel_cuda_get_ctx_size(void)
{
	return sizeof(struct cuda_task);
}

static struct spdk_accel_module_if  g_accel_cuda_module = {
	.module_init = accel_cuda_init,
	.module_fini = accel_cuda_exit,
	.write_config_json = accel_cuda_write_config_json,
	.get_ctx_size = accel_cuda_get_ctx_size,
	.name = "accel_cuda",
	.supports_opcode = accel_cuda_supports_opcode,
	.get_io_channel = accel_cuda_get_io_channel,
	.submit_tasks = accel_cuda_submit_tasks
};

static int
_accel_cuda_submit_xor(struct cuda_stream *stream, struct spdk_accel_task *task)
{
	int rc;

	memcpy(stream->inputs, task->nsrcs.srcs, task->nsrcs.cnt * sizeof(void *));

	rc = accel_cuda_xor_start(
		     task->d.iovs[0].iov_base,
		     stream->inputs,
		     task->nsrcs.cnt,
		     task->d.iovs[0].iov_len,
		     stream->status,
		     stream->stream);
	if (rc) {
		SPDK_ERRLOG("accel_cuda_xor_start failed (rc %d)!\n", rc);
	}
	return rc;
}

static int
_accel_cuda_submit_fill(struct cuda_stream *stream, struct spdk_accel_task *task)
{
	return accel_cuda_fill_start(task->d.iovs, task->d.iovcnt, task->fill_pattern,
				     stream->status, stream->stream);
}

static int
_accel_cuda_submit_copy(struct cuda_stream *stream, struct spdk_accel_task *task)
{
	return accel_cuda_copy_start(
		       task->s.iovs, task->s.iovcnt,
		       task->d.iovs, task->d.iovcnt,
		       stream->status, stream->stream);
}

static int
_accel_cuda_submit_request(struct cuda_io_channel *cch, struct spdk_accel_task *task)
{
	int rc = -EINVAL;
	struct cuda_stream *stream = STAILQ_FIRST(&cch->idle_streams);

	if (!stream) {
		SPDK_DEBUGLOG(accel_cuda, "no idle streams\n");
		return -EAGAIN;
	}
	STAILQ_REMOVE_HEAD(&cch->idle_streams, link);

	*stream->status = -1;
	stream->task = (struct cuda_task *)task;

	switch (task->op_code) {
	case SPDK_ACCEL_OPC_XOR:
		rc = _accel_cuda_submit_xor(stream, task);
		break;
	case SPDK_ACCEL_OPC_FILL:
		rc = _accel_cuda_submit_fill(stream, task);
		break;
	case SPDK_ACCEL_OPC_COPY:
		rc = _accel_cuda_submit_copy(stream, task);
		break;
	default:
		assert(false);
	}
	if (rc) {
		STAILQ_INSERT_TAIL(&cch->idle_streams, stream, link);
		stream->task = NULL;
		spdk_accel_task_complete((struct spdk_accel_task *)task, rc);
		return rc;
	}
	cch->num_running_tasks++;

	SPDK_DEBUGLOG(accel_cuda, "tid %u ch %p started task %p\n", gettid(), cch, task);
	return 0;
}

static int
_accel_cuda_start_request(struct cuda_io_channel *cch, struct spdk_accel_task *task)
{
	struct cuda_task *ctask = (struct cuda_task *)task;

	if (!STAILQ_EMPTY(&cch->idle_streams)) {
		return _accel_cuda_submit_request(cch, task);
	}

	SPDK_DEBUGLOG(accel_cuda, "tid %u ch %p queuing task %p\n", gettid(), cch, task);
	STAILQ_INSERT_TAIL(&cch->waiting_tasks, ctask, link);
	return 0;
}

static int
accel_cuda_poller(void *arg)
{
	struct cuda_io_channel *cch = arg;
	uint32_t num_completions = 0;
	uint32_t num_started = 0;
	uint32_t i;

	if (!cch->num_running_tasks) {
		return SPDK_POLLER_IDLE;
	}

	for (i = 0; i < cch->num_streams; i++) {
		int status;
		struct cuda_stream *stream = &cch->streams[i];

		if (stream->task && (status = *stream->status) == 0) {
			struct cuda_task *task = stream->task;
			stream->task = NULL;
			STAILQ_INSERT_TAIL(&cch->idle_streams, stream, link);
			cch->num_running_tasks--;
			spdk_accel_task_complete(&task->base, status ? -EIO : 0);
			num_completions++;
		}
	}

	if (!num_completions) {
		SPDK_DEBUGLOG(accel_cuda, "tid %u ch %p idle\n", gettid(), cch);
		return SPDK_POLLER_IDLE;
	}

	while (!STAILQ_EMPTY(&cch->waiting_tasks) && !STAILQ_EMPTY(&cch->idle_streams))	{
		/* start a waiting task now */
		struct cuda_task *task = STAILQ_FIRST(&cch->waiting_tasks);
		STAILQ_REMOVE_HEAD(&cch->waiting_tasks, link);

		if (_accel_cuda_submit_request(cch, &task->base) != 0) {
			break;
		}
		num_started++;
	}

	SPDK_DEBUGLOG(accel_cuda, "tid %u ch %p tasks: completed %u, started %u\n",
		      gettid(), cch, num_completions, num_started);

	return SPDK_POLLER_BUSY;
}

static struct spdk_io_channel *accel_cuda_get_io_channel(void);

static bool
accel_cuda_supports_opcode(enum spdk_accel_opcode opc)
{
	if (!g_accel_cuda_initialized) {
		SPDK_ERRLOG("not initialized!");
		return false;
	}

	switch (opc) {
	case SPDK_ACCEL_OPC_XOR:
	case SPDK_ACCEL_OPC_FILL:
	case SPDK_ACCEL_OPC_COPY:
		return true;
	default:
		return false;
	}
}

static int
accel_cuda_submit_xor(struct cuda_io_channel *cch, struct spdk_accel_task *task)
{
	uint32_t i;

	if (task->d.iovs[0].iov_len < ACCEL_CUDA_XOR_MIN_BUF_LEN ||
	    task->nsrcs.cnt > CUDA_XOR_MAX_SOURCES) {
		SPDK_INFOLOG(accel_cuda,
			     "tid %u ch %p redirecting task %p (len 0x%lx, nsrcs %u) to generic handler\n",
			     gettid(), cch, task, task->d.iovs[0].iov_len, task->nsrcs.cnt);

		return spdk_xor_gen(
			       task->d.iovs[0].iov_base,
			       task->nsrcs.srcs,
			       task->nsrcs.cnt,
			       task->d.iovs[0].iov_len);
	}

	if (spdk_unlikely(!task->d.iovs[0].iov_base || task->d.iovcnt != 1 || task->nsrcs.cnt < 2)) {
		SPDK_ERRLOG(" invalid iovcnt (dst %p, iovcnt %u, nsrcs %u)!\n",
			    task->d.iovs[0].iov_base, task->d.iovcnt, task->nsrcs.cnt);
		return -EINVAL;
	}

	for (i = 0; i < task->nsrcs.cnt; i++) {
		if (task->nsrcs.srcs[i] == NULL) {
			SPDK_ERRLOG("nsrcs.srcs[%u] == NULL!\n", i);
			return -EINVAL;
		}
	}
	return _accel_cuda_start_request(cch, task);
}

static int
accel_cuda_submit_fill(struct cuda_io_channel *cch, struct spdk_accel_task *task)
{
	if (spdk_unlikely(task->d.iovcnt < 1 || task->d.iovs->iov_base == NULL)) {
		SPDK_ERRLOG(" invalid iovcnt (iovcnt %u, addr %p)!\n", task->d.iovcnt, task->d.iovs->iov_base);
		return -EINVAL;
	}
	return _accel_cuda_start_request(cch, task);
}

static int
accel_cuda_submit_copy(struct cuda_io_channel *cch, struct spdk_accel_task *task)
{
	if (spdk_unlikely(task->d.iovcnt < 1 || task->d.iovs->iov_base == NULL ||
			  task->s.iovcnt < 1 || task->s.iovs->iov_base == NULL)) {
		SPDK_ERRLOG("invalid iovcnt (d.iovcnt %u, d.addr %p, s.iovcnt %u, s.addr %p)!\n",
			    task->d.iovcnt, task->d.iovs->iov_base, task->s.iovcnt, task->s.iovs->iov_base);
		return -EINVAL;
	}
	return _accel_cuda_start_request(cch, task);
}

static int
accel_cuda_submit_tasks(struct spdk_io_channel *ch, struct spdk_accel_task *accel_task)
{
	struct cuda_io_channel *cch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *tmp;
	int rc = 0;

	do {
		switch (accel_task->op_code) {
		case SPDK_ACCEL_OPC_XOR:
			rc = accel_cuda_submit_xor(cch, accel_task);
			break;
		case SPDK_ACCEL_OPC_FILL:
			rc = accel_cuda_submit_fill(cch, accel_task);
			break;
		case SPDK_ACCEL_OPC_COPY:
			rc = accel_cuda_submit_copy(cch, accel_task);
			break;
		default:
			assert(false);
			break;
		}

		tmp = STAILQ_NEXT(accel_task, link);

		/* Report any build errors via the callback now. */
		if (rc) {
			spdk_accel_task_complete(accel_task, rc);
		}
		accel_task = tmp;
	} while (accel_task);

	return 0;
}

static int
accel_cuda_create_cb(void *io_device, void *ctx_buf)
{
	struct cuda_io_channel *cch = ctx_buf;
	uint32_t i;
	int rc = 0;
	int buf_size = sizeof(void *) * CUDA_XOR_MAX_SOURCES;

	memset(cch, 0, sizeof(struct cuda_io_channel));
	STAILQ_INIT(&cch->waiting_tasks);
	STAILQ_INIT(&cch->idle_streams);
	cch->num_streams = ACCEL_CUDA_STREAMS_PER_CHANNEL;
	cch->num_running_tasks = 0;

	SPDK_INFOLOG(accel_cuda, "tid %u creating channel %p\n", gettid(), cch);

	cch->streams = aligned_alloc(SPDK_CACHE_LINE_SIZE, cch->num_streams * sizeof(struct cuda_stream));
	if (cch->streams == NULL) {
		SPDK_ERRLOG("Failed to allocate memory for streams!\n");
		rc = -ENOMEM;
		goto exit;
	}
	memset(cch->streams, 0, cch->num_streams * sizeof(struct cuda_stream));

	cch->inputs_buf = spdk_dma_zmalloc(cch->num_streams * buf_size, CUDA_CACHE_LINE_SIZE, NULL);
	if (cch->inputs_buf == NULL) {
		SPDK_ERRLOG("Failed to allocate dma memory for inputs_buf!\n");
		rc = -ENOMEM;
		goto exit;
	}

	cch->status_buf = spdk_dma_zmalloc(cch->num_streams * SPDK_CACHE_LINE_SIZE, SPDK_CACHE_LINE_SIZE,
					   NULL);
	if (cch->status_buf == NULL) {
		SPDK_ERRLOG("Failed to allocate for inputs_buf!\n");
		rc = -ENOMEM;
		goto exit;
	}
	memset(cch->status_buf, 0, cch->num_streams * SPDK_CACHE_LINE_SIZE);

	for (i = 0; i < cch->num_streams; i++) {
		if (cudaStreamCreateWithFlags(&cch->streams[i].stream, cudaStreamNonBlocking) != cudaSuccess) {
			SPDK_ERRLOG("ch %p create of cuda stream[%i] failed\n", cch, i);
			rc = -ENOMEM;
			goto exit;
		}
		cch->streams[i].inputs = (void **)(cch->inputs_buf + i * buf_size);
		cch->streams[i].status = cch->status_buf + i * SPDK_CACHE_LINE_SIZE;
		STAILQ_INSERT_TAIL(&cch->idle_streams, &cch->streams[i], link);
	}

	cch->poller = SPDK_POLLER_REGISTER(accel_cuda_poller, cch, 0);
	if (!cch->poller) {
		SPDK_ERRLOG("ch %p poller creation failed!\n", cch);
		rc = -ENOMEM;
		goto exit;
	}

exit:
	if (rc) {
		if (cch->inputs_buf) {
			spdk_dma_free(cch->inputs_buf);
		}
		if (cch->status_buf) {
			spdk_dma_free(cch->status_buf);
		}
		if (cch->streams) {
			for (i = 0; i < cch->num_streams; i++) {
				if (cch->streams[i].stream) {
					cudaStreamDestroy(cch->streams[i].stream);
				}
			}
			free(cch->streams);
		}
	}
	return rc;
}

static void
accel_cuda_destroy_cb(void *io_device, void *ctx_buf)
{
	struct cuda_io_channel *cch = ctx_buf;
	uint32_t i;

	spdk_poller_unregister(&cch->poller);
	for (i = 0; i < cch->num_streams; i++) {
		cudaStreamDestroy(cch->streams[i].stream);
	}
	free(cch->streams);
	spdk_dma_free(cch->status_buf);
	spdk_dma_free(cch->inputs_buf);
}

static struct spdk_io_channel *
accel_cuda_get_io_channel(void)
{
	return spdk_get_io_channel(&g_accel_cuda_module);
}

void
accel_cuda_enable_probe(void)
{
	SPDK_NOTICELOG("module enabled.\n");
	g_accel_cuda_enable = true;
	spdk_accel_module_list_add(&g_accel_cuda_module);
}

static int
accel_cuda_init(void)
{
	int dev_count = 0;

	if (!g_accel_cuda_enable) {
		SPDK_NOTICELOG("not enabled\n");
		return 0;
	}

	if (cudaGetDeviceCount(&dev_count) != cudaSuccess || dev_count == 0) {
		SPDK_NOTICELOG("found no cuda compatible devices\n");
		return -1;
	}

	g_accel_cuda_mem_map = cuda_utils_create_mem_map();
	if (!g_accel_cuda_mem_map) {
		SPDK_ERRLOG("cuda_utils_create_mem_map()\n");
		return -1;
	}

	SPDK_NOTICELOG("registering module\n");
	g_accel_cuda_initialized = true;

	spdk_io_device_register(&g_accel_cuda_module, accel_cuda_create_cb, accel_cuda_destroy_cb,
				sizeof(struct cuda_io_channel), "accel_cuda_module");
	return 0;
}

static void
accel_cuda_exit(void *ctx)
{
	spdk_accel_module_finish();
	cuda_utils_free_mem_map(&g_accel_cuda_mem_map);
}

static void
accel_cuda_write_config_json(struct spdk_json_write_ctx *w)
{
	if (g_accel_cuda_enable) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "cuda_scan_accel_module");
		spdk_json_write_object_end(w);
	}
}

SPDK_LOG_REGISTER_COMPONENT(accel_cuda)
