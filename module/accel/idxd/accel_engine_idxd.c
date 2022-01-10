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

#include "accel_engine_idxd.h"

#include "spdk/stdinc.h"

#include "spdk_internal/accel_engine.h"
#include "spdk/log.h"
#include "spdk_internal/idxd.h"

#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/thread.h"
#include "spdk/idxd.h"
#include "spdk/util.h"
#include "spdk/json.h"
#include "spdk/trace.h"
#include "spdk_internal/trace_defs.h"

static bool g_idxd_enable = false;
static bool g_kernel_mode = false;
uint32_t g_config_number;

enum channel_state {
	IDXD_CHANNEL_ACTIVE,
	IDXD_CHANNEL_ERROR,
};

static bool g_idxd_initialized = false;

struct idxd_device {
	struct				spdk_idxd_device *idxd;
	TAILQ_ENTRY(idxd_device)	tailq;
};
static TAILQ_HEAD(, idxd_device) g_idxd_devices = TAILQ_HEAD_INITIALIZER(g_idxd_devices);
static struct idxd_device *g_next_dev = NULL;
static uint32_t g_num_devices = 0;
static pthread_mutex_t g_dev_lock = PTHREAD_MUTEX_INITIALIZER;

struct idxd_io_channel {
	struct spdk_idxd_io_channel	*chan;
	struct idxd_device		*dev;
	enum channel_state		state;
	struct spdk_poller		*poller;
	uint32_t			num_outstanding;
	TAILQ_HEAD(, spdk_accel_task)	queued_tasks;
};

static struct spdk_io_channel *idxd_get_io_channel(void);

static struct idxd_device *
idxd_select_device(struct idxd_io_channel *chan)
{
	uint32_t count = 0;
	struct idxd_device *dev;
	uint32_t socket_id = spdk_env_get_socket_id(spdk_env_get_current_core());

	/*
	 * We allow channels to share underlying devices,
	 * selection is round-robin based with a limitation
	 * on how many channel can share one device.
	 */
	do {
		/* select next device */
		pthread_mutex_lock(&g_dev_lock);
		g_next_dev = TAILQ_NEXT(g_next_dev, tailq);
		if (g_next_dev == NULL) {
			g_next_dev = TAILQ_FIRST(&g_idxd_devices);
		}
		dev = g_next_dev;
		pthread_mutex_unlock(&g_dev_lock);

		if (socket_id != spdk_idxd_get_socket(dev->idxd)) {
			continue;
		}

		/*
		 * Now see if a channel is available on this one. We only
		 * allow a specific number of channels to share a device
		 * to limit outstanding IO for flow control purposes.
		 */
		chan->chan = spdk_idxd_get_channel(dev->idxd);
		if (chan->chan != NULL) {
			SPDK_DEBUGLOG(accel_idxd, "On socket %d using device on socket %d\n",
				      socket_id, spdk_idxd_get_socket(dev->idxd));
			return dev;
		}
	} while (count++ < g_num_devices);

	/* We are out of available channels and/or devices for the local socket. We fix the number
	 * of channels that we allocate per device and only allocate devices on the same socket
	 * that the current thread is on. If on a 2 socket system it may be possible to avoid
	 * this situation by spreading threads across the sockets.
	 */
	SPDK_ERRLOG("No more DSA devices available on the local socket.\n");
	return NULL;
}

static void
idxd_done(void *cb_arg, int status)
{
	struct spdk_accel_task *accel_task = cb_arg;
	struct idxd_io_channel *chan = spdk_io_channel_get_ctx(accel_task->accel_ch->engine_ch);

	assert(chan->num_outstanding > 0);
	spdk_trace_record(TRACE_IDXD_OP_COMPLETE, 0, 0, 0, chan->num_outstanding - 1);
	chan->num_outstanding--;

	spdk_accel_task_complete(accel_task, status);
}

static int
_process_single_task(struct spdk_io_channel *ch, struct spdk_accel_task *task)
{
	struct idxd_io_channel *chan = spdk_io_channel_get_ctx(ch);
	int rc = 0;
	uint8_t fill_pattern = (uint8_t)task->fill_pattern;
	struct iovec *iov;
	uint32_t iovcnt;
	struct iovec siov = {};
	struct iovec diov = {};

	switch (task->op_code) {
	case ACCEL_OPCODE_MEMMOVE:
		siov.iov_base = task->src;
		siov.iov_len = task->nbytes;
		diov.iov_base = task->dst;
		diov.iov_len = task->nbytes;
		rc = spdk_idxd_submit_copy(chan->chan, &diov, 1, &siov, 1, idxd_done, task);
		break;
	case ACCEL_OPCODE_DUALCAST:
		rc = spdk_idxd_submit_dualcast(chan->chan, task->dst, task->dst2, task->src, task->nbytes,
					       idxd_done, task);
		break;
	case ACCEL_OPCODE_COMPARE:
		siov.iov_base = task->src;
		siov.iov_len = task->nbytes;
		diov.iov_base = task->dst;
		diov.iov_len = task->nbytes;
		rc = spdk_idxd_submit_compare(chan->chan, &siov, 1, &diov, 1, idxd_done, task);
		break;
	case ACCEL_OPCODE_MEMFILL:
		memset(&task->fill_pattern, fill_pattern, sizeof(uint64_t));
		diov.iov_base = task->dst;
		diov.iov_len = task->nbytes;
		rc = spdk_idxd_submit_fill(chan->chan, &diov, 1, task->fill_pattern, idxd_done,
					   task);
		break;
	case ACCEL_OPCODE_CRC32C:
		if (task->v.iovcnt == 0) {
			siov.iov_base = task->src;
			siov.iov_len = task->nbytes;
			iov = &siov;
			iovcnt = 1;
		} else {
			iov = task->v.iovs;
			iovcnt = task->v.iovcnt;
		}
		rc = spdk_idxd_submit_crc32c(chan->chan, iov, iovcnt, task->seed, task->crc_dst,
					     idxd_done, task);
		break;
	case ACCEL_OPCODE_COPY_CRC32C:
		if (task->v.iovcnt == 0) {
			siov.iov_base = task->src;
			siov.iov_len = task->nbytes;
			iov = &siov;
			iovcnt = 1;
		} else {
			iov = task->v.iovs;
			iovcnt = task->v.iovcnt;
		}
		diov.iov_base = task->dst;
		diov.iov_len = task->nbytes;
		rc = spdk_idxd_submit_copy_crc32c(chan->chan, &diov, 1, iov, iovcnt,
						  task->seed, task->crc_dst,
						  idxd_done, task);
		break;
	default:
		assert(false);
		rc = -EINVAL;
		break;
	}

	if (rc == 0) {
		chan->num_outstanding++;
		spdk_trace_record(TRACE_IDXD_OP_SUBMIT, 0, 0, 0, chan->num_outstanding);
	}

	return rc;
}

static int
idxd_submit_tasks(struct spdk_io_channel *ch, struct spdk_accel_task *first_task)
{
	struct idxd_io_channel *chan = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *task, *tmp;
	int rc = 0;

	task = first_task;

	if (chan->state == IDXD_CHANNEL_ERROR) {
		while (task) {
			tmp = TAILQ_NEXT(task, link);
			spdk_accel_task_complete(task, -EINVAL);
			task = tmp;
		}
		return 0;
	}

	if (!TAILQ_EMPTY(&chan->queued_tasks)) {
		goto queue_tasks;
	}

	/* The caller will either submit a single task or a group of tasks that are
	 * linked together but they cannot be on a list. For example, see idxd_poll()
	 * where a list of queued tasks is being resubmitted, the list they are on
	 * is initialized after saving off the first task from the list which is then
	 * passed in here.  Similar thing is done in the accel framework.
	 */
	while (task) {
		tmp = TAILQ_NEXT(task, link);
		rc = _process_single_task(ch, task);

		if (rc == -EBUSY) {
			goto queue_tasks;
		} else if (rc) {
			spdk_accel_task_complete(task, rc);
		}
		task = tmp;
	}

	return 0;

queue_tasks:
	while (task != NULL) {
		tmp = TAILQ_NEXT(task, link);
		TAILQ_INSERT_TAIL(&chan->queued_tasks, task, link);
		task = tmp;
	}
	return 0;
}

static int
idxd_poll(void *arg)
{
	struct idxd_io_channel *chan = arg;
	struct spdk_accel_task *task = NULL;
	int count;

	count = spdk_idxd_process_events(chan->chan);

	/* Check if there are any pending ops to process if the channel is active */
	if (chan->state == IDXD_CHANNEL_ACTIVE) {
		/* Submit queued tasks */
		if (!TAILQ_EMPTY(&chan->queued_tasks)) {
			task = TAILQ_FIRST(&chan->queued_tasks);

			TAILQ_INIT(&chan->queued_tasks);

			idxd_submit_tasks(task->accel_ch->engine_ch, task);
		}
	}

	return count > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static size_t
accel_engine_idxd_get_ctx_size(void)
{
	return 0;
}

static uint64_t
idxd_get_capabilities(void)
{
	return ACCEL_COPY | ACCEL_FILL | ACCEL_CRC32C | ACCEL_COMPARE |
	       ACCEL_DUALCAST | ACCEL_COPY_CRC32C;
}

static struct spdk_accel_engine idxd_accel_engine = {
	.get_capabilities	= idxd_get_capabilities,
	.get_io_channel		= idxd_get_io_channel,
	.submit_tasks		= idxd_submit_tasks,
};

static int
idxd_create_cb(void *io_device, void *ctx_buf)
{
	struct idxd_io_channel *chan = ctx_buf;
	struct idxd_device *dev;

	dev = idxd_select_device(chan);
	if (dev == NULL) {
		SPDK_ERRLOG("Failed to get an idxd channel\n");
		return -EINVAL;
	}

	chan->dev = dev;
	chan->poller = SPDK_POLLER_REGISTER(idxd_poll, chan, 0);
	TAILQ_INIT(&chan->queued_tasks);
	chan->num_outstanding = 0;
	chan->state = IDXD_CHANNEL_ACTIVE;

	return 0;
}

static void
idxd_destroy_cb(void *io_device, void *ctx_buf)
{
	struct idxd_io_channel *chan = ctx_buf;

	spdk_poller_unregister(&chan->poller);
	spdk_idxd_put_channel(chan->chan);
}

static struct spdk_io_channel *
idxd_get_io_channel(void)
{
	return spdk_get_io_channel(&idxd_accel_engine);
}

static void
attach_cb(void *cb_ctx, struct spdk_idxd_device *idxd)
{
	struct idxd_device *dev;

	dev = calloc(1, sizeof(*dev));
	if (dev == NULL) {
		SPDK_ERRLOG("Failed to allocate device struct\n");
		return;
	}

	dev->idxd = idxd;
	if (g_next_dev == NULL) {
		g_next_dev = dev;
	}

	TAILQ_INSERT_TAIL(&g_idxd_devices, dev, tailq);
	g_num_devices++;
}

void
accel_engine_idxd_enable_probe(uint32_t config_number, bool kernel_mode)
{
	if (config_number > IDXD_MAX_CONFIG_NUM) {
		SPDK_ERRLOG("Invalid config number, using default of 0\n");
		config_number = 0;
	}

	g_config_number = config_number;
	g_kernel_mode = kernel_mode;
	g_idxd_enable = true;
	spdk_idxd_set_config(g_config_number, g_kernel_mode);
}

static int
accel_engine_idxd_init(void)
{
	if (!g_idxd_enable) {
		return -EINVAL;
	}

	if (spdk_idxd_probe(NULL, attach_cb) != 0) {
		SPDK_ERRLOG("spdk_idxd_probe() failed\n");
		return -EINVAL;
	}

	if (TAILQ_EMPTY(&g_idxd_devices)) {
		SPDK_NOTICELOG("no available idxd devices\n");
		return -EINVAL;
	}

	g_idxd_initialized = true;
	SPDK_NOTICELOG("Accel engine updated to use IDXD DSA engine.\n");
	spdk_accel_hw_engine_register(&idxd_accel_engine);
	spdk_io_device_register(&idxd_accel_engine, idxd_create_cb, idxd_destroy_cb,
				sizeof(struct idxd_io_channel), "idxd_accel_engine");
	return 0;
}

static void
accel_engine_idxd_exit(void *ctx)
{
	struct idxd_device *dev;

	if (g_idxd_initialized) {
		spdk_io_device_unregister(&idxd_accel_engine, NULL);
	}

	while (!TAILQ_EMPTY(&g_idxd_devices)) {
		dev = TAILQ_FIRST(&g_idxd_devices);
		TAILQ_REMOVE(&g_idxd_devices, dev, tailq);
		spdk_idxd_detach(dev->idxd);
		free(dev);
	}

	spdk_accel_engine_module_finish();
}

static void
accel_engine_idxd_write_config_json(struct spdk_json_write_ctx *w)
{
	if (g_idxd_enable) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "idxd_scan_accel_engine");
		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_uint32(w, "config_number", g_config_number);
		spdk_json_write_named_uint32(w, "config_kernel_mode", g_kernel_mode);
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);
	}
}

SPDK_TRACE_REGISTER_FN(idxd_trace, "idxd", TRACE_GROUP_IDXD)
{
	spdk_trace_register_description("IDXD_OP_SUBMIT", TRACE_IDXD_OP_SUBMIT, OWNER_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "count");
	spdk_trace_register_description("IDXD_OP_COMPLETE", TRACE_IDXD_OP_COMPLETE, OWNER_NONE, OBJECT_NONE,
					0, SPDK_TRACE_ARG_TYPE_INT, "count");
}

SPDK_ACCEL_MODULE_REGISTER(accel_engine_idxd_init, accel_engine_idxd_exit,
			   accel_engine_idxd_write_config_json,
			   accel_engine_idxd_get_ctx_size)

SPDK_LOG_REGISTER_COMPONENT(accel_idxd)
