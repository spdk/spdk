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

static bool g_idxd_enable = false;
uint32_t g_config_number;
static uint32_t g_batch_max;

enum channel_state {
	IDXD_CHANNEL_ACTIVE,
	IDXD_CHANNEL_PAUSED,
	IDXD_CHANNEL_ERROR,
};

static bool g_idxd_initialized = false;

struct idxd_device {
	struct				spdk_idxd_device *idxd;
	TAILQ_ENTRY(idxd_device)	tailq;
};
static TAILQ_HEAD(, idxd_device) g_idxd_devices = TAILQ_HEAD_INITIALIZER(g_idxd_devices);
static struct idxd_device *g_next_dev = NULL;

struct idxd_io_channel {
	struct spdk_idxd_io_channel	*chan;
	struct idxd_device		*dev;
	enum channel_state		state;
	struct spdk_poller		*poller;
	TAILQ_HEAD(, spdk_accel_task)	queued_tasks;
};

static struct spdk_io_channel *idxd_get_io_channel(void);

static struct idxd_device *
idxd_select_device(void)
{
	/*
	 * We allow channels to share underlying devices,
	 * selection is round-robin based.
	 */

	g_next_dev = TAILQ_NEXT(g_next_dev, tailq);
	if (g_next_dev == NULL) {
		g_next_dev = TAILQ_FIRST(&g_idxd_devices);
	}
	return g_next_dev;
}

static void
idxd_done(void *cb_arg, int status)
{
	struct spdk_accel_task *accel_task = cb_arg;

	spdk_accel_task_complete(accel_task, status);
}

static int
_process_single_task(struct spdk_io_channel *ch, struct spdk_accel_task *task)
{
	struct idxd_io_channel *chan = spdk_io_channel_get_ctx(ch);
	int rc = 0;
	uint8_t fill_pattern = (uint8_t)task->fill_pattern;
	void *src;

	switch (task->op_code) {
	case ACCEL_OPCODE_MEMMOVE:
		rc = spdk_idxd_submit_copy(chan->chan, task->dst, task->src, task->nbytes, idxd_done, task);
		break;
	case ACCEL_OPCODE_DUALCAST:
		rc = spdk_idxd_submit_dualcast(chan->chan, task->dst, task->dst2, task->src, task->nbytes,
					       idxd_done, task);
		break;
	case ACCEL_OPCODE_COMPARE:
		rc = spdk_idxd_submit_compare(chan->chan, task->src, task->src2, task->nbytes, idxd_done, task);
		break;
	case ACCEL_OPCODE_MEMFILL:
		memset(&task->fill_pattern, fill_pattern, sizeof(uint64_t));
		rc = spdk_idxd_submit_fill(chan->chan, task->dst, task->fill_pattern, task->nbytes, idxd_done,
					   task);
		break;
	case ACCEL_OPCODE_CRC32C:
		src = (task->v.iovcnt == 0) ? task->src : task->v.iovs[0].iov_base;
		rc = spdk_idxd_submit_crc32c(chan->chan, task->dst, src, task->seed, task->nbytes, idxd_done,
					     task);
		break;
	case ACCEL_OPCODE_COPY_CRC32C:
		rc = spdk_idxd_submit_copy_crc32c(chan->chan, task->dst, task->src, task->crc_dst, task->seed,
						  task->nbytes, idxd_done, task);
		break;
	default:
		assert(false);
		rc = -EINVAL;
		break;
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

	if (chan->state == IDXD_CHANNEL_PAUSED) {
		goto queue_tasks;
	} else if (chan->state == IDXD_CHANNEL_ERROR) {
		while (task) {
			tmp = TAILQ_NEXT(task, link);
			spdk_accel_task_complete(task, -EINVAL);
			task = tmp;
		}
		return 0;
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

static uint32_t
idxd_batch_get_max(struct spdk_io_channel *ch)
{
	return spdk_idxd_batch_get_max();
}

static struct spdk_accel_engine idxd_accel_engine = {
	.get_capabilities	= idxd_get_capabilities,
	.get_io_channel		= idxd_get_io_channel,
	.batch_get_max		= idxd_batch_get_max,
	.submit_tasks		= idxd_submit_tasks,
};

/*
 * Configure the max number of descriptors that a channel is
 * allowed to use based on the total number of current channels.
 * This is to allow for dynamic load balancing for hw flow control.
 */
static void
_config_max_desc(struct spdk_io_channel_iter *i)
{
	struct idxd_io_channel *chan;
	struct spdk_io_channel *ch;
	struct spdk_idxd_device *idxd;
	int rc;

	ch = spdk_io_channel_iter_get_channel(i);
	chan = spdk_io_channel_get_ctx(ch);
	idxd = spdk_io_channel_iter_get_ctx(i);

	/* reconfigure channel only if this channel is on the same idxd
	 * device that initiated the rebalance.
	 */
	if (chan->dev->idxd == idxd) {
		rc = spdk_idxd_reconfigure_chan(chan->chan);
		if (rc == 0) {
			chan->state = IDXD_CHANNEL_ACTIVE;
		} else {
			chan->state = IDXD_CHANNEL_ERROR;
		}
	}

	spdk_for_each_channel_continue(i, 0);
}

/* Pauses a channel so that it can be re-configured. */
static void
_pause_chan(struct spdk_io_channel_iter *i)
{
	struct idxd_io_channel *chan;
	struct spdk_io_channel *ch;
	struct spdk_idxd_device *idxd;

	ch = spdk_io_channel_iter_get_channel(i);
	chan = spdk_io_channel_get_ctx(ch);
	idxd = spdk_io_channel_iter_get_ctx(i);

	/* start queueing up new requests if this channel is on the same idxd
	 * device that initiated the rebalance.
	 */
	if (chan->dev->idxd == idxd) {
		chan->state = IDXD_CHANNEL_PAUSED;
	}

	spdk_for_each_channel_continue(i, 0);
}

static void
_pause_chan_done(struct spdk_io_channel_iter *i, int status)
{
	struct spdk_idxd_device *idxd;

	idxd = spdk_io_channel_iter_get_ctx(i);

	spdk_for_each_channel(&idxd_accel_engine, _config_max_desc, idxd, NULL);
}

static int
idxd_create_cb(void *io_device, void *ctx_buf)
{
	struct idxd_io_channel *chan = ctx_buf;
	struct idxd_device *dev;
	int rc;

	dev = idxd_select_device();
	if (dev == NULL) {
		SPDK_ERRLOG("Failed to allocate idxd_device\n");
		return -EINVAL;
	}

	chan->chan = spdk_idxd_get_channel(dev->idxd);
	if (chan->chan == NULL) {
		return -ENOMEM;
	}

	chan->dev = dev;
	chan->poller = SPDK_POLLER_REGISTER(idxd_poll, chan, 0);
	TAILQ_INIT(&chan->queued_tasks);

	/*
	 * Configure the channel but leave paused until all others
	 * are paused and re-configured based on the new number of
	 * channels. This enables dynamic load balancing for HW
	 * flow control. The idxd device will tell us if rebalance is
	 * needed based on how many channels are using it.
	 */
	rc = spdk_idxd_configure_chan(chan->chan);
	if (rc) {
		SPDK_ERRLOG("Failed to configure new channel rc = %d\n", rc);
		chan->state = IDXD_CHANNEL_ERROR;
		spdk_poller_unregister(&chan->poller);
		return rc;
	}

	if (spdk_idxd_device_needs_rebalance(chan->dev->idxd) == false) {
		chan->state = IDXD_CHANNEL_ACTIVE;
		return 0;
	}

	chan->state = IDXD_CHANNEL_PAUSED;

	/*
	 * Pause all channels so that we can set proper flow control
	 * per channel. When all are paused, we'll update the max
	 * number of descriptors allowed per channel.
	 */
	spdk_for_each_channel(&idxd_accel_engine, _pause_chan, chan->dev->idxd,
			      _pause_chan_done);

	return 0;
}

static void
_pause_chan_destroy_done(struct spdk_io_channel_iter *i, int status)
{
	struct spdk_idxd_device *idxd;

	idxd = spdk_io_channel_iter_get_ctx(i);

	/* Rebalance the rings with the smaller number of remaining channels, but
	 * pass the idxd device along so its only done on shared channels.
	 */
	spdk_for_each_channel(&idxd_accel_engine, _config_max_desc, idxd, NULL);
}

static void
idxd_destroy_cb(void *io_device, void *ctx_buf)
{
	struct idxd_io_channel *chan = ctx_buf;
	bool rebalance;

	spdk_poller_unregister(&chan->poller);
	rebalance = spdk_idxd_put_channel(chan->chan);

	/* Only rebalance if there are still other channels on this device */
	if (rebalance == true) {
		/* Pause each channel then rebalance the max number of ring slots. */
		spdk_for_each_channel(&idxd_accel_engine, _pause_chan, chan->dev->idxd,
				      _pause_chan_destroy_done);
	}
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
}

void
accel_engine_idxd_enable_probe(uint32_t config_number)
{
	if (config_number > IDXD_MAX_CONFIG_NUM) {
		SPDK_ERRLOG("Invalid config number, using default of 0\n");
		config_number = 0;
	}

	g_config_number = config_number;
	g_idxd_enable = true;
	spdk_idxd_set_config(g_config_number);
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
	g_batch_max = spdk_idxd_batch_get_max();
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
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);
	}
}

SPDK_ACCEL_MODULE_REGISTER(accel_engine_idxd_init, accel_engine_idxd_exit,
			   accel_engine_idxd_write_config_json,
			   accel_engine_idxd_get_ctx_size)

SPDK_LOG_REGISTER_COMPONENT(accel_idxd)
