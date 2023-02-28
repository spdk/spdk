/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "accel_dsa.h"

#include "spdk/stdinc.h"

#include "spdk_internal/accel_module.h"
#include "spdk/log.h"
#include "spdk_internal/idxd.h"

#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/likely.h"
#include "spdk/thread.h"
#include "spdk/idxd.h"
#include "spdk/util.h"
#include "spdk/json.h"
#include "spdk/trace.h"
#include "spdk_internal/trace_defs.h"

static bool g_dsa_enable = false;
static bool g_kernel_mode = false;

enum channel_state {
	IDXD_CHANNEL_ACTIVE,
	IDXD_CHANNEL_ERROR,
};

static bool g_dsa_initialized = false;

struct idxd_device {
	struct				spdk_idxd_device *dsa;
	TAILQ_ENTRY(idxd_device)	tailq;
};
static TAILQ_HEAD(, idxd_device) g_dsa_devices = TAILQ_HEAD_INITIALIZER(g_dsa_devices);
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

static struct spdk_io_channel *dsa_get_io_channel(void);

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
			g_next_dev = TAILQ_FIRST(&g_dsa_devices);
		}
		dev = g_next_dev;
		pthread_mutex_unlock(&g_dev_lock);

		if (socket_id != spdk_idxd_get_socket(dev->dsa)) {
			continue;
		}

		/*
		 * Now see if a channel is available on this one. We only
		 * allow a specific number of channels to share a device
		 * to limit outstanding IO for flow control purposes.
		 */
		chan->chan = spdk_idxd_get_channel(dev->dsa);
		if (chan->chan != NULL) {
			SPDK_DEBUGLOG(accel_dsa, "On socket %d using device on socket %d\n",
				      socket_id, spdk_idxd_get_socket(dev->dsa));
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
dsa_done(void *cb_arg, int status)
{
	struct idxd_task *idxd_task = cb_arg;
	struct idxd_io_channel *chan;

	chan = idxd_task->chan;

	assert(chan->num_outstanding > 0);
	spdk_trace_record(TRACE_ACCEL_DSA_OP_COMPLETE, 0, 0, 0, chan->num_outstanding - 1);
	chan->num_outstanding--;

	spdk_accel_task_complete(&idxd_task->task, status);
}

static int
idxd_submit_dualcast(struct idxd_io_channel *ch, struct idxd_task *idxd_task, int flags)
{
	struct spdk_accel_task *task = &idxd_task->task;

	if (spdk_unlikely(task->d.iovcnt != 1 || task->d2.iovcnt != 1 || task->s.iovcnt != 1)) {
		return -EINVAL;
	}

	if (spdk_unlikely(task->d.iovs[0].iov_len != task->s.iovs[0].iov_len ||
			  task->d.iovs[0].iov_len != task->d2.iovs[0].iov_len)) {
		return -EINVAL;
	}

	return spdk_idxd_submit_dualcast(ch->chan, task->d.iovs[0].iov_base,
					 task->d2.iovs[0].iov_base, task->s.iovs[0].iov_base,
					 task->d.iovs[0].iov_len, flags, dsa_done, idxd_task);
}

static int
_process_single_task(struct spdk_io_channel *ch, struct spdk_accel_task *task)
{
	struct idxd_io_channel *chan = spdk_io_channel_get_ctx(ch);
	struct idxd_task *idxd_task;
	int rc = 0, flags = 0;

	idxd_task = SPDK_CONTAINEROF(task, struct idxd_task, task);
	idxd_task->chan = chan;

	switch (task->op_code) {
	case ACCEL_OPC_COPY:
		if (task->flags & ACCEL_FLAG_PERSISTENT) {
			flags |= SPDK_IDXD_FLAG_PERSISTENT;
			flags |= SPDK_IDXD_FLAG_NONTEMPORAL;
		}
		rc = spdk_idxd_submit_copy(chan->chan, task->d.iovs, task->d.iovcnt,
					   task->s.iovs, task->s.iovcnt, flags, dsa_done, idxd_task);
		break;
	case ACCEL_OPC_DUALCAST:
		if (task->flags & ACCEL_FLAG_PERSISTENT) {
			flags |= SPDK_IDXD_FLAG_PERSISTENT;
			flags |= SPDK_IDXD_FLAG_NONTEMPORAL;
		}
		rc = idxd_submit_dualcast(chan, idxd_task, flags);
		break;
	case ACCEL_OPC_COMPARE:
		rc = spdk_idxd_submit_compare(chan->chan, task->s.iovs, task->s.iovcnt,
					      task->s2.iovs, task->s2.iovcnt, flags,
					      dsa_done, idxd_task);
		break;
	case ACCEL_OPC_FILL:
		if (task->flags & ACCEL_FLAG_PERSISTENT) {
			flags |= SPDK_IDXD_FLAG_PERSISTENT;
			flags |= SPDK_IDXD_FLAG_NONTEMPORAL;
		}
		rc = spdk_idxd_submit_fill(chan->chan, task->d.iovs, task->d.iovcnt,
					   task->fill_pattern, flags, dsa_done, idxd_task);
		break;
	case ACCEL_OPC_CRC32C:
		rc = spdk_idxd_submit_crc32c(chan->chan, task->s.iovs, task->s.iovcnt, task->seed,
					     task->crc_dst, flags, dsa_done, idxd_task);
		break;
	case ACCEL_OPC_COPY_CRC32C:
		if (task->flags & ACCEL_FLAG_PERSISTENT) {
			flags |= SPDK_IDXD_FLAG_PERSISTENT;
			flags |= SPDK_IDXD_FLAG_NONTEMPORAL;
		}
		rc = spdk_idxd_submit_copy_crc32c(chan->chan, task->d.iovs, task->d.iovcnt,
						  task->s.iovs, task->s.iovcnt,
						  task->seed, task->crc_dst, flags,
						  dsa_done, idxd_task);
		break;
	default:
		assert(false);
		rc = -EINVAL;
		break;
	}

	if (rc == 0) {
		chan->num_outstanding++;
		spdk_trace_record(TRACE_ACCEL_DSA_OP_SUBMIT, 0, 0, 0, chan->num_outstanding);
	}

	return rc;
}

static int
dsa_submit_tasks(struct spdk_io_channel *ch, struct spdk_accel_task *first_task)
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

			dsa_submit_tasks(spdk_io_channel_from_ctx(idxd_task->chan), task);
		}
	}

	return count > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static size_t
accel_dsa_get_ctx_size(void)
{
	return sizeof(struct idxd_task);
}

static bool
dsa_supports_opcode(enum accel_opcode opc)
{
	if (!g_dsa_initialized) {
		return false;
	}

	switch (opc) {
	case ACCEL_OPC_COPY:
	case ACCEL_OPC_FILL:
	case ACCEL_OPC_DUALCAST:
	case ACCEL_OPC_COMPARE:
	case ACCEL_OPC_CRC32C:
	case ACCEL_OPC_COPY_CRC32C:
		return true;
	default:
		return false;
	}
}

static int accel_dsa_init(void);
static void accel_dsa_exit(void *ctx);
static void accel_dsa_write_config_json(struct spdk_json_write_ctx *w);

static struct spdk_accel_module_if g_dsa_module = {
	.module_init = accel_dsa_init,
	.module_fini = accel_dsa_exit,
	.write_config_json = accel_dsa_write_config_json,
	.get_ctx_size = accel_dsa_get_ctx_size,
	.name			= "dsa",
	.supports_opcode	= dsa_supports_opcode,
	.get_io_channel		= dsa_get_io_channel,
	.submit_tasks		= dsa_submit_tasks
};

SPDK_ACCEL_MODULE_REGISTER(dsa, &g_dsa_module)

static int
dsa_create_cb(void *io_device, void *ctx_buf)
{
	struct idxd_io_channel *chan = ctx_buf;
	struct idxd_device *dsa;

	dsa = idxd_select_device(chan);
	if (dsa == NULL) {
		SPDK_ERRLOG("Failed to get an idxd channel\n");
		return -EINVAL;
	}

	chan->dev = dsa;
	chan->poller = SPDK_POLLER_REGISTER(idxd_poll, chan, 0);
	TAILQ_INIT(&chan->queued_tasks);
	chan->num_outstanding = 0;
	chan->state = IDXD_CHANNEL_ACTIVE;

	return 0;
}

static void
dsa_destroy_cb(void *io_device, void *ctx_buf)
{
	struct idxd_io_channel *chan = ctx_buf;

	spdk_poller_unregister(&chan->poller);
	spdk_idxd_put_channel(chan->chan);
}

static struct spdk_io_channel *
dsa_get_io_channel(void)
{
	return spdk_get_io_channel(&g_dsa_module);
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

	dev->dsa = idxd;
	if (g_next_dev == NULL) {
		g_next_dev = dev;
	}

	TAILQ_INSERT_TAIL(&g_dsa_devices, dev, tailq);
	g_num_devices++;
}

void
accel_dsa_enable_probe(bool kernel_mode)
{
	g_kernel_mode = kernel_mode;
	g_dsa_enable = true;
	spdk_idxd_set_config(g_kernel_mode);
}

static bool
probe_cb(void *cb_ctx, struct spdk_pci_device *dev)
{
	if (dev->id.device_id == PCI_DEVICE_ID_INTEL_DSA) {
		return true;
	}

	return false;
}

static int
accel_dsa_init(void)
{
	if (!g_dsa_enable) {
		return -EINVAL;
	}

	if (spdk_idxd_probe(NULL, attach_cb, probe_cb) != 0) {
		SPDK_ERRLOG("spdk_idxd_probe() failed\n");
		return -EINVAL;
	}

	if (TAILQ_EMPTY(&g_dsa_devices)) {
		SPDK_NOTICELOG("no available dsa devices\n");
		return -EINVAL;
	}

	g_dsa_initialized = true;
	SPDK_NOTICELOG("Accel framework DSA module initialized.\n");
	spdk_io_device_register(&g_dsa_module, dsa_create_cb, dsa_destroy_cb,
				sizeof(struct idxd_io_channel), "dsa_accel_module");
	return 0;
}

static void
accel_dsa_exit(void *ctx)
{
	struct idxd_device *dev;

	if (g_dsa_initialized) {
		spdk_io_device_unregister(&g_dsa_module, NULL);
		g_dsa_initialized = false;
	}

	while (!TAILQ_EMPTY(&g_dsa_devices)) {
		dev = TAILQ_FIRST(&g_dsa_devices);
		TAILQ_REMOVE(&g_dsa_devices, dev, tailq);
		spdk_idxd_detach(dev->dsa);
		free(dev);
	}

	spdk_accel_module_finish();
}

static void
accel_dsa_write_config_json(struct spdk_json_write_ctx *w)
{
	if (g_dsa_enable) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "dsa_scan_accel_module");
		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_bool(w, "config_kernel_mode", g_kernel_mode);
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);
	}
}

SPDK_TRACE_REGISTER_FN(dsa_trace, "dsa", TRACE_GROUP_ACCEL_DSA)
{
	spdk_trace_register_description("DSA_OP_SUBMIT", TRACE_ACCEL_DSA_OP_SUBMIT, OWNER_NONE, OBJECT_NONE,
					0,
					SPDK_TRACE_ARG_TYPE_INT, "count");
	spdk_trace_register_description("DSA_OP_COMPLETE", TRACE_ACCEL_DSA_OP_COMPLETE, OWNER_NONE,
					OBJECT_NONE,
					0, SPDK_TRACE_ARG_TYPE_INT, "count");
}

SPDK_LOG_REGISTER_COMPONENT(accel_dsa)
