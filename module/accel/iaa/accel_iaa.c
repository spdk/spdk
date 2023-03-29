/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "accel_iaa.h"

#include "spdk/stdinc.h"

#include "spdk/accel_module.h"
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

static bool g_iaa_enable = false;

enum channel_state {
	IDXD_CHANNEL_ACTIVE,
	IDXD_CHANNEL_ERROR,
};

static bool g_iaa_initialized = false;

struct idxd_device {
	struct spdk_idxd_device		*iaa;
	TAILQ_ENTRY(idxd_device)	tailq;
};

static TAILQ_HEAD(, idxd_device) g_iaa_devices = TAILQ_HEAD_INITIALIZER(g_iaa_devices);
static struct idxd_device *g_next_dev = NULL;
static uint32_t g_num_devices = 0;
static pthread_mutex_t g_dev_lock = PTHREAD_MUTEX_INITIALIZER;

struct idxd_task {
	struct spdk_accel_task	task;
	struct idxd_io_channel	*chan;
};

struct idxd_io_channel {
	struct spdk_idxd_io_channel	*chan;
	struct idxd_device		*dev;
	enum channel_state		state;
	struct spdk_poller		*poller;
	uint32_t			num_outstanding;
	TAILQ_HEAD(, spdk_accel_task)	queued_tasks;
};

static struct spdk_io_channel *iaa_get_io_channel(void);

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
			g_next_dev = TAILQ_FIRST(&g_iaa_devices);
		}
		dev = g_next_dev;
		pthread_mutex_unlock(&g_dev_lock);

		if (socket_id != spdk_idxd_get_socket(dev->iaa)) {
			continue;
		}

		/*
		 * Now see if a channel is available on this one. We only
		 * allow a specific number of channels to share a device
		 * to limit outstanding IO for flow control purposes.
		 */
		chan->chan = spdk_idxd_get_channel(dev->iaa);
		if (chan->chan != NULL) {
			SPDK_DEBUGLOG(accel_iaa, "On socket %d using device on socket %d\n",
				      socket_id, spdk_idxd_get_socket(dev->iaa));
			return dev;
		}
	} while (count++ < g_num_devices);

	/* We are out of available channels and/or devices for the local socket. We fix the number
	 * of channels that we allocate per device and only allocate devices on the same socket
	 * that the current thread is on. If on a 2 socket system it may be possible to avoid
	 * this situation by spreading threads across the sockets.
	 */
	SPDK_ERRLOG("No more IAA devices available on the local socket.\n");
	return NULL;
}

static void
iaa_done(void *cb_arg, int status)
{
	struct idxd_task *idxd_task = cb_arg;
	struct idxd_io_channel *chan;

	chan = idxd_task->chan;

	assert(chan->num_outstanding > 0);
	spdk_trace_record(TRACE_ACCEL_IAA_OP_COMPLETE, 0, 0, 0, chan->num_outstanding - 1);
	chan->num_outstanding--;

	spdk_accel_task_complete(&idxd_task->task, status);
}

static int
_process_single_task(struct spdk_io_channel *ch, struct spdk_accel_task *task)
{
	struct idxd_io_channel *chan = spdk_io_channel_get_ctx(ch);
	struct idxd_task *idxd_task;
	int rc = 0;
	int flags = 0;

	idxd_task = SPDK_CONTAINEROF(task, struct idxd_task, task);
	idxd_task->chan = chan;

	/* TODO: iovec supprot */
	if (task->d.iovcnt > 1 || task->s.iovcnt > 1) {
		SPDK_ERRLOG("fatal: IAA does not support > 1 iovec\n");
		assert(0);
	}

	switch (task->op_code) {
	case ACCEL_OPC_COMPRESS:
		rc = spdk_idxd_submit_compress(chan->chan, task->d.iovs[0].iov_base, task->d.iovs[0].iov_len,
					       task->s.iovs, task->s.iovcnt, task->output_size, flags,
					       iaa_done, idxd_task);
		break;
	case ACCEL_OPC_DECOMPRESS:
		rc = spdk_idxd_submit_decompress(chan->chan, task->d.iovs, task->d.iovcnt, task->s.iovs,
						 task->s.iovcnt, flags, iaa_done, idxd_task);
		break;
	default:
		assert(false);
		rc = -EINVAL;
		break;
	}

	if (rc == 0) {
		chan->num_outstanding++;
		spdk_trace_record(TRACE_ACCEL_IAA_OP_SUBMIT, 0, 0, 0, chan->num_outstanding);
	}

	return rc;
}

static int
iaa_submit_tasks(struct spdk_io_channel *ch, struct spdk_accel_task *first_task)
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
	struct idxd_task *idxd_task;
	int count;

	count = spdk_idxd_process_events(chan->chan);

	/* Check if there are any pending ops to process if the channel is active */
	if (chan->state == IDXD_CHANNEL_ACTIVE) {
		/* Submit queued tasks */
		if (!TAILQ_EMPTY(&chan->queued_tasks)) {
			task = TAILQ_FIRST(&chan->queued_tasks);
			idxd_task = SPDK_CONTAINEROF(task, struct idxd_task, task);

			TAILQ_INIT(&chan->queued_tasks);

			iaa_submit_tasks(spdk_io_channel_from_ctx(idxd_task->chan), task);
		}
	}

	return count > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static size_t
accel_iaa_get_ctx_size(void)
{
	return sizeof(struct idxd_task);
}

static bool
iaa_supports_opcode(enum accel_opcode opc)
{
	if (!g_iaa_initialized) {
		return false;
	}

	switch (opc) {
	case ACCEL_OPC_COMPRESS:
	case ACCEL_OPC_DECOMPRESS:
		return true;
	default:
		return false;
	}
}

static int accel_iaa_init(void);
static void accel_iaa_exit(void *ctx);
static void accel_iaa_write_config_json(struct spdk_json_write_ctx *w);

static struct spdk_accel_module_if g_iaa_module = {
	.module_init = accel_iaa_init,
	.module_fini = accel_iaa_exit,
	.write_config_json = accel_iaa_write_config_json,
	.get_ctx_size = accel_iaa_get_ctx_size,
	.name			= "iaa",
	.supports_opcode	= iaa_supports_opcode,
	.get_io_channel		= iaa_get_io_channel,
	.submit_tasks		= iaa_submit_tasks
};

SPDK_ACCEL_MODULE_REGISTER(iaa, &g_iaa_module)

static int
idxd_create_cb(void *io_device, void *ctx_buf)
{
	struct idxd_io_channel *chan = ctx_buf;
	struct idxd_device *iaa;

	iaa = idxd_select_device(chan);
	if (iaa == NULL) {
		SPDK_ERRLOG("Failed to get an idxd channel\n");
		return -EINVAL;
	}

	chan->dev = iaa;
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
iaa_get_io_channel(void)
{
	return spdk_get_io_channel(&g_iaa_module);
}

static void
attach_cb(void *cb_ctx, struct spdk_idxd_device *iaa)
{
	struct idxd_device *dev;

	dev = calloc(1, sizeof(*dev));
	if (dev == NULL) {
		SPDK_ERRLOG("Failed to allocate device struct\n");
		return;
	}

	dev->iaa = iaa;
	if (g_next_dev == NULL) {
		g_next_dev = dev;
	}

	TAILQ_INSERT_TAIL(&g_iaa_devices, dev, tailq);
	g_num_devices++;
}

void
accel_iaa_enable_probe(void)
{
	g_iaa_enable = true;
	/* TODO initially only support user mode w/IAA */
	spdk_idxd_set_config(false);
}

static bool
caller_probe_cb(void *cb_ctx, struct spdk_pci_device *dev)
{
	if (dev->id.device_id == PCI_DEVICE_ID_INTEL_IAA) {
		return true;
	}

	return false;
}

static int
accel_iaa_init(void)
{
	if (!g_iaa_enable) {
		return -EINVAL;
	}

	if (spdk_idxd_probe(NULL, attach_cb, caller_probe_cb) != 0) {
		SPDK_ERRLOG("spdk_idxd_probe() failed\n");
		return -EINVAL;
	}

	if (TAILQ_EMPTY(&g_iaa_devices)) {
		SPDK_NOTICELOG("no available idxd devices\n");
		return -EINVAL;
	}

	g_iaa_initialized = true;
	SPDK_NOTICELOG("Accel framework IAA module initialized.\n");
	spdk_io_device_register(&g_iaa_module, idxd_create_cb, idxd_destroy_cb,
				sizeof(struct idxd_io_channel), "iaa_accel_module");
	return 0;
}

static void
accel_iaa_exit(void *ctx)
{
	struct idxd_device *dev;

	if (g_iaa_initialized) {
		spdk_io_device_unregister(&g_iaa_module, NULL);
		g_iaa_initialized = false;
	}

	while (!TAILQ_EMPTY(&g_iaa_devices)) {
		dev = TAILQ_FIRST(&g_iaa_devices);
		TAILQ_REMOVE(&g_iaa_devices, dev, tailq);
		spdk_idxd_detach(dev->iaa);
		free(dev);
	}

	spdk_accel_module_finish();
}

static void
accel_iaa_write_config_json(struct spdk_json_write_ctx *w)
{
	if (g_iaa_enable) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "iaa_scan_accel_module");
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);
	}
}

SPDK_TRACE_REGISTER_FN(iaa_trace, "iaa", TRACE_GROUP_ACCEL_IAA)
{
	spdk_trace_register_description("IAA_OP_SUBMIT", TRACE_ACCEL_IAA_OP_SUBMIT, OWNER_NONE, OBJECT_NONE,
					0, SPDK_TRACE_ARG_TYPE_INT, "count");
	spdk_trace_register_description("IAA_OP_COMPLETE", TRACE_ACCEL_IAA_OP_COMPLETE, OWNER_NONE,
					OBJECT_NONE, 0, SPDK_TRACE_ARG_TYPE_INT, "count");
}

SPDK_LOG_REGISTER_COMPONENT(accel_iaa)
