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
#include "spdk_internal/log.h"
#include "spdk_internal/idxd.h"

#include "spdk/env.h"
#include "spdk/conf.h"
#include "spdk/event.h"
#include "spdk/thread.h"
#include "spdk/idxd.h"
#include "spdk/util.h"
#include "spdk/json.h"

#define ALIGN_4K 0x1000

static bool g_idxd_enable = false;
uint32_t g_config_number;

enum channel_state {
	IDXD_CHANNEL_ACTIVE,
	IDXD_CHANNEL_PAUSED,
	IDXD_CHANNEL_ERROR,
};

static bool g_idxd_initialized = false;

struct pci_device {
	struct spdk_pci_device *pci_dev;
	TAILQ_ENTRY(pci_device) tailq;
};
static TAILQ_HEAD(, pci_device) g_pci_devices = TAILQ_HEAD_INITIALIZER(g_pci_devices);

struct idxd_device {
	struct				spdk_idxd_device *idxd;
	int				num_channels;
	TAILQ_ENTRY(idxd_device)	tailq;
};
static TAILQ_HEAD(, idxd_device) g_idxd_devices = TAILQ_HEAD_INITIALIZER(g_idxd_devices);
static struct idxd_device *g_next_dev = NULL;

struct idxd_op {
	struct spdk_idxd_io_channel	*chan;
	void				*cb_arg;
	spdk_idxd_req_cb		cb_fn;
	void				*src;
	union {
		void			*dst;
		void			*src2;
	};
	void				*dst2;
	uint32_t			seed;
	uint64_t			fill_pattern;
	uint32_t			op_code;
	uint64_t			nbytes;
	struct idxd_batch		*batch;
	TAILQ_ENTRY(idxd_op)		link;
};

struct idxd_io_channel {
	struct spdk_idxd_io_channel	*chan;
	struct spdk_idxd_device		*idxd;
	struct idxd_device		*dev;
	enum channel_state		state;
	struct spdk_poller		*poller;
	TAILQ_HEAD(, idxd_op)		queued_ops;
};

struct idxd_task {
	spdk_accel_completion_cb	cb;
};

pthread_mutex_t g_configuration_lock = PTHREAD_MUTEX_INITIALIZER;

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

static int
idxd_poll(void *arg)
{
	struct idxd_io_channel *chan = arg;
	struct idxd_op *op = NULL;
	int rc;

	spdk_idxd_process_events(chan->chan);

	/* Check if there are any pending ops to process if the channel is active */
	if (chan->state != IDXD_CHANNEL_ACTIVE) {
		return -1;
	}

	while (!TAILQ_EMPTY(&chan->queued_ops)) {
		op = TAILQ_FIRST(&chan->queued_ops);

		switch (op->op_code) {
		case IDXD_OPCODE_MEMMOVE:
			rc = spdk_idxd_submit_copy(op->chan, op->dst, op->src, op->nbytes,
						   op->cb_fn, op->cb_arg);
			break;
		case IDXD_OPCODE_DUALCAST:
			rc = spdk_idxd_submit_dualcast(op->chan, op->dst, op->dst2, op->src, op->nbytes,
						       op->cb_fn, op->cb_arg);
			break;
		case IDXD_OPCODE_COMPARE:
			rc = spdk_idxd_submit_compare(op->chan, op->src, op->src2, op->nbytes,
						      op->cb_fn, op->cb_arg);
			break;
		case IDXD_OPCODE_MEMFILL:
			rc = spdk_idxd_submit_fill(op->chan, op->dst, op->fill_pattern, op->nbytes,
						   op->cb_fn, op->cb_arg);
			break;
		case IDXD_OPCODE_CRC32C_GEN:
			rc = spdk_idxd_submit_crc32c(op->chan, op->dst, op->src, op->seed, op->nbytes,
						     op->cb_fn, op->cb_arg);
			break;
		case IDXD_OPCODE_BATCH:
			rc = spdk_idxd_batch_submit(op->chan, op->batch, op->cb_fn, op->cb_arg);
			break;
		default:
			/* Should never get here */
			assert(false);
			break;
		}
		if (rc == 0) {
			TAILQ_REMOVE(&chan->queued_ops, op, link);
			free(op);
		} else {
			/* Busy, resubmit to try again later */
			break;
		}
	}

	return -1;
}

static size_t
accel_engine_idxd_get_ctx_size(void)
{
	return sizeof(struct idxd_task) + sizeof(struct spdk_accel_task);
}

static void
idxd_done(void *cb_arg, int status)
{
	struct spdk_accel_task *accel_req;
	struct idxd_task *idxd_task = cb_arg;

	accel_req = SPDK_CONTAINEROF(idxd_task, struct spdk_accel_task,
				     offload_ctx);

	idxd_task->cb(accel_req, status);
}

static struct idxd_op *
_prep_queue_command(struct idxd_io_channel *chan, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct idxd_op *op_to_queue;

	op_to_queue = calloc(1, sizeof(struct idxd_op));
	if (op_to_queue == NULL) {
		SPDK_ERRLOG("Failed to allocate operation for queueing\n");
		return NULL;
	}

	op_to_queue->chan = chan->chan;
	op_to_queue->cb_fn = cb_fn;
	op_to_queue->cb_arg = cb_arg;

	return op_to_queue;
}

static int
idxd_submit_copy(struct spdk_io_channel *ch, void *dst, void *src, uint64_t nbytes,
		 spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct idxd_task *idxd_task = (struct idxd_task *)cb_arg;
	struct idxd_io_channel *chan = spdk_io_channel_get_ctx(ch);
	int rc = 0;

	idxd_task->cb = cb_fn;

	if (chan->state == IDXD_CHANNEL_ACTIVE) {
		rc = spdk_idxd_submit_copy(chan->chan, dst, src, nbytes, idxd_done, idxd_task);
	}

	if (chan->state == IDXD_CHANNEL_PAUSED || rc == -EBUSY) {
		struct idxd_op *op_to_queue;

		/* Commpom prep. */
		op_to_queue = _prep_queue_command(chan, idxd_done, idxd_task);
		if (op_to_queue == NULL) {
			return -ENOMEM;
		}

		/* Command specific. */
		op_to_queue->dst = dst;
		op_to_queue->src = src;
		op_to_queue->nbytes = nbytes;
		op_to_queue->op_code = IDXD_OPCODE_MEMMOVE;

		/* Queue the operation. */
		TAILQ_INSERT_TAIL(&chan->queued_ops, op_to_queue, link);
		return 0;

	} else if (chan->state == IDXD_CHANNEL_ERROR) {
		return -EINVAL;
	}

	return rc;
}

static int
idxd_submit_dualcast(struct spdk_io_channel *ch, void *dst1, void *dst2, void *src,
		     uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct idxd_task *idxd_task = (struct idxd_task *)cb_arg;
	struct idxd_io_channel *chan = spdk_io_channel_get_ctx(ch);
	int rc = 0;

	idxd_task->cb = cb_fn;

	if (chan->state == IDXD_CHANNEL_ACTIVE) {
		rc = spdk_idxd_submit_dualcast(chan->chan, dst1, dst2, src, nbytes, idxd_done, idxd_task);
	}

	if (chan->state == IDXD_CHANNEL_PAUSED || rc == -EBUSY) {
		struct idxd_op *op_to_queue;

		/* Commpom prep. */
		op_to_queue = _prep_queue_command(chan, idxd_done, idxd_task);
		if (op_to_queue == NULL) {
			return -ENOMEM;
		}

		/* Command specific. */
		op_to_queue->dst = dst1;
		op_to_queue->dst2 = dst2;
		op_to_queue->src = src;
		op_to_queue->nbytes = nbytes;
		op_to_queue->op_code = IDXD_OPCODE_DUALCAST;

		/* Queue the operation. */
		TAILQ_INSERT_TAIL(&chan->queued_ops, op_to_queue, link);
		return 0;

	} else if (chan->state == IDXD_CHANNEL_ERROR) {
		return -EINVAL;
	}

	return rc;
}

static int
idxd_submit_compare(struct spdk_io_channel *ch, void *src1, void *src2,
		    uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct idxd_task *idxd_task = (struct idxd_task *)cb_arg;
	struct idxd_io_channel *chan = spdk_io_channel_get_ctx(ch);
	int rc = 0;

	idxd_task->cb = cb_fn;

	if (chan->state == IDXD_CHANNEL_ACTIVE) {
		rc = spdk_idxd_submit_compare(chan->chan, src1, src2, nbytes, idxd_done, idxd_task);
	}

	if (chan->state == IDXD_CHANNEL_PAUSED || rc == -EBUSY) {
		struct idxd_op *op_to_queue;

		/* Commpom prep. */
		op_to_queue = _prep_queue_command(chan, idxd_done, idxd_task);
		if (op_to_queue == NULL) {
			return -ENOMEM;
		}

		/* Command specific. */
		op_to_queue->src = src1;
		op_to_queue->src2 = src2;
		op_to_queue->nbytes = nbytes;
		op_to_queue->op_code = IDXD_OPCODE_COMPARE;

		/* Queue the operation. */
		TAILQ_INSERT_TAIL(&chan->queued_ops, op_to_queue, link);
		return 0;

	} else if (chan->state == IDXD_CHANNEL_ERROR) {
		return -EINVAL;
	}

	return rc;
}

static int
idxd_submit_fill(struct spdk_io_channel *ch, void *dst, uint8_t fill,
		 uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct idxd_task *idxd_task = (struct idxd_task *)cb_arg;
	struct idxd_io_channel *chan = spdk_io_channel_get_ctx(ch);
	int rc = 0;
	uint64_t fill_pattern;

	idxd_task->cb = cb_fn;
	memset(&fill_pattern, fill, sizeof(uint64_t));

	if (chan->state == IDXD_CHANNEL_ACTIVE) {
		rc = spdk_idxd_submit_fill(chan->chan, dst, fill_pattern, nbytes, idxd_done, idxd_task);
	}

	if (chan->state == IDXD_CHANNEL_PAUSED || rc == -EBUSY) {
		struct idxd_op *op_to_queue;

		/* Commpom prep. */
		op_to_queue = _prep_queue_command(chan, idxd_done, idxd_task);
		if (op_to_queue == NULL) {
			return -ENOMEM;
		}

		/* Command specific. */
		op_to_queue->dst = dst;
		op_to_queue->fill_pattern = fill_pattern;
		op_to_queue->nbytes = nbytes;
		op_to_queue->op_code = IDXD_OPCODE_MEMFILL;

		/* Queue the operation. */
		TAILQ_INSERT_TAIL(&chan->queued_ops, op_to_queue, link);
		return 0;

	} else if (chan->state == IDXD_CHANNEL_ERROR) {
		return -EINVAL;
	}

	return rc;
}

static int
idxd_submit_crc32c(struct spdk_io_channel *ch, uint32_t *dst, void *src,
		   uint32_t seed, uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct idxd_task *idxd_task = (struct idxd_task *)cb_arg;
	struct idxd_io_channel *chan = spdk_io_channel_get_ctx(ch);
	int rc = 0;

	idxd_task->cb = cb_fn;

	if (chan->state == IDXD_CHANNEL_ACTIVE) {
		rc = spdk_idxd_submit_crc32c(chan->chan, dst, src, seed, nbytes, idxd_done, idxd_task);
	}

	if (chan->state == IDXD_CHANNEL_PAUSED || rc == -EBUSY) {
		struct idxd_op *op_to_queue;

		/* Commpom prep. */
		op_to_queue = _prep_queue_command(chan, idxd_done, idxd_task);
		if (op_to_queue == NULL) {
			return -ENOMEM;
		}

		/* Command specific. */
		op_to_queue->dst = dst;
		op_to_queue->src = src;
		op_to_queue->seed = seed;
		op_to_queue->nbytes = nbytes;
		op_to_queue->op_code = IDXD_OPCODE_CRC32C_GEN;

		/* Queue the operation. */
		TAILQ_INSERT_TAIL(&chan->queued_ops, op_to_queue, link);
		return 0;

	} else if (chan->state == IDXD_CHANNEL_ERROR) {
		return -EINVAL;
	}

	return rc;
}

static uint64_t
idxd_get_capabilities(void)
{
	return ACCEL_COPY | ACCEL_FILL | ACCEL_CRC32C | ACCEL_COMPARE |
	       ACCEL_DUALCAST | ACCEL_BATCH;
}

static uint32_t
idxd_batch_get_max(void)
{
	return spdk_idxd_batch_get_max();
}

static struct spdk_accel_batch *
idxd_batch_start(struct spdk_io_channel *ch)
{
	struct idxd_io_channel *chan = spdk_io_channel_get_ctx(ch);

	return (struct spdk_accel_batch *)spdk_idxd_batch_create(chan->chan);
}

static int
idxd_batch_cancel(struct spdk_io_channel *ch, struct spdk_accel_batch *_batch)
{
	struct idxd_io_channel *chan = spdk_io_channel_get_ctx(ch);
	struct idxd_batch *batch = (struct idxd_batch *)_batch;

	return spdk_idxd_batch_cancel(chan->chan, batch);
}

static int
idxd_batch_submit(struct spdk_io_channel *ch, struct spdk_accel_batch *_batch,
		  spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct idxd_task *idxd_task = (struct idxd_task *)cb_arg;
	struct idxd_io_channel *chan = spdk_io_channel_get_ctx(ch);
	struct idxd_batch *batch = (struct idxd_batch *)_batch;
	int rc = 0;

	idxd_task->cb = cb_fn;

	if (chan->state == IDXD_CHANNEL_ACTIVE) {
		rc = spdk_idxd_batch_submit(chan->chan, batch, idxd_done, idxd_task);
	}

	if (chan->state == IDXD_CHANNEL_PAUSED || rc == -EBUSY) {
		struct idxd_op *op_to_queue;

		/* Commpom prep. */
		op_to_queue = _prep_queue_command(chan, idxd_done, idxd_task);
		if (op_to_queue == NULL) {
			return -ENOMEM;
		}

		/* Command specific. */
		op_to_queue->batch = batch;
		op_to_queue->op_code = IDXD_OPCODE_BATCH;

		/* Queue the operation. */
		TAILQ_INSERT_TAIL(&chan->queued_ops, op_to_queue, link);
		return 0;

	} else if (chan->state == IDXD_CHANNEL_ERROR) {
		return -EINVAL;
	}

	return rc;
}

static int
idxd_batch_prep_copy(struct spdk_io_channel *ch, struct spdk_accel_batch *_batch,
		     void *dst, void *src, uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct idxd_task *idxd_task = (struct idxd_task *)cb_arg;
	struct idxd_io_channel *chan = spdk_io_channel_get_ctx(ch);
	struct idxd_batch *batch = (struct idxd_batch *)_batch;

	idxd_task->cb = cb_fn;

	return spdk_idxd_batch_prep_copy(chan->chan, batch, dst, src, nbytes,
					 idxd_done, idxd_task);
}

static int
idxd_batch_prep_fill(struct spdk_io_channel *ch, struct spdk_accel_batch *_batch,
		     void *dst, uint8_t fill, uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct idxd_task *idxd_task = (struct idxd_task *)cb_arg;
	struct idxd_io_channel *chan = spdk_io_channel_get_ctx(ch);
	uint64_t fill_pattern;
	struct idxd_batch *batch = (struct idxd_batch *)_batch;

	idxd_task->cb = cb_fn;
	memset(&fill_pattern, fill, sizeof(uint64_t));

	return spdk_idxd_batch_prep_fill(chan->chan, batch, dst, fill_pattern, nbytes, idxd_done,
					 idxd_task);
}

static int
idxd_batch_prep_dualcast(struct spdk_io_channel *ch, struct spdk_accel_batch *_batch,
			 void *dst1, void *dst2, void *src, uint64_t nbytes,
			 spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct idxd_task *idxd_task = (struct idxd_task *)cb_arg;
	struct idxd_io_channel *chan = spdk_io_channel_get_ctx(ch);
	struct idxd_batch *batch = (struct idxd_batch *)_batch;

	idxd_task->cb = cb_fn;

	return spdk_idxd_batch_prep_dualcast(chan->chan, batch, dst1, dst2, src, nbytes, idxd_done,
					     idxd_task);
}

static int
idxd_batch_prep_crc32c(struct spdk_io_channel *ch, struct spdk_accel_batch *_batch,
		       uint32_t *dst, void *src, uint32_t seed, uint64_t nbytes,
		       spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct idxd_task *idxd_task = (struct idxd_task *)cb_arg;
	struct idxd_io_channel *chan = spdk_io_channel_get_ctx(ch);
	struct idxd_batch *batch = (struct idxd_batch *)_batch;

	idxd_task->cb = cb_fn;

	return spdk_idxd_batch_prep_crc32c(chan->chan, batch, dst, src, seed, nbytes, idxd_done,
					   idxd_task);
}

static int
idxd_batch_prep_compare(struct spdk_io_channel *ch, struct spdk_accel_batch *_batch,
			void *src1, void *src2, uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct idxd_task *idxd_task = (struct idxd_task *)cb_arg;
	struct idxd_io_channel *chan = spdk_io_channel_get_ctx(ch);
	struct idxd_batch *batch = (struct idxd_batch *)_batch;

	idxd_task->cb = cb_fn;

	return spdk_idxd_batch_prep_compare(chan->chan, batch, src1, src2, nbytes, idxd_done,
					    idxd_task);
}

static struct spdk_accel_engine idxd_accel_engine = {
	.get_capabilities	= idxd_get_capabilities,
	.copy			= idxd_submit_copy,
	.batch_get_max		= idxd_batch_get_max,
	.batch_create		= idxd_batch_start,
	.batch_cancel		= idxd_batch_cancel,
	.batch_prep_copy	= idxd_batch_prep_copy,
	.batch_prep_fill	= idxd_batch_prep_fill,
	.batch_prep_dualcast	= idxd_batch_prep_dualcast,
	.batch_prep_crc32c	= idxd_batch_prep_crc32c,
	.batch_prep_compare	= idxd_batch_prep_compare,
	.batch_submit		= idxd_batch_submit,
	.dualcast		= idxd_submit_dualcast,
	.compare		= idxd_submit_compare,
	.fill			= idxd_submit_fill,
	.crc32c			= idxd_submit_crc32c,
	.get_io_channel		= idxd_get_io_channel,
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
	int rc;

	ch = spdk_io_channel_iter_get_channel(i);
	chan = spdk_io_channel_get_ctx(ch);

	pthread_mutex_lock(&g_configuration_lock);
	rc = spdk_idxd_reconfigure_chan(chan->chan, chan->dev->num_channels);
	pthread_mutex_unlock(&g_configuration_lock);
	if (rc == 0) {
		chan->state = IDXD_CHANNEL_ACTIVE;
	} else {
		chan->state = IDXD_CHANNEL_ERROR;
	}

	spdk_for_each_channel_continue(i, 0);
}

/* Pauses a channel so that it can be re-configured. */
static void
_pause_chan(struct spdk_io_channel_iter *i)
{
	struct idxd_io_channel *chan;
	struct spdk_io_channel *ch;

	ch = spdk_io_channel_iter_get_channel(i);
	chan = spdk_io_channel_get_ctx(ch);

	/* start queueing up new requests. */
	chan->state = IDXD_CHANNEL_PAUSED;

	spdk_for_each_channel_continue(i, 0);
}

static void
_pause_chan_done(struct spdk_io_channel_iter *i, int status)
{
	spdk_for_each_channel(&idxd_accel_engine, _config_max_desc, NULL, NULL);
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
	chan->poller = spdk_poller_register(idxd_poll, chan, 0);
	TAILQ_INIT(&chan->queued_ops);

	/*
	 * Configure the channel but leave paused until all others
	 * are paused and re-configured based on the new number of
	 * channels. This enables dynamic load balancing for HW
	 * flow control.
	 */
	pthread_mutex_lock(&g_configuration_lock);
	rc = spdk_idxd_configure_chan(chan->chan);
	if (rc) {
		SPDK_ERRLOG("Failed to configure new channel rc = %d\n", rc);
		chan->state = IDXD_CHANNEL_ERROR;
		spdk_poller_unregister(&chan->poller);
		pthread_mutex_unlock(&g_configuration_lock);
		return rc;
	}

	chan->state = IDXD_CHANNEL_PAUSED;
	chan->dev->num_channels++;
	pthread_mutex_unlock(&g_configuration_lock);

	/*
	 * Pause all channels so that we can set proper flow control
	 * per channel. When all are paused, we'll update the max
	 * number of descriptors allowed per channel.
	 */
	spdk_for_each_channel(&idxd_accel_engine, _pause_chan, NULL,
			      _pause_chan_done);

	return 0;
}

static void
_pause_chan_destroy_done(struct spdk_io_channel_iter *i, int status)
{
	/* Rebalance the rings with the smaller number of remaining channels. */
	spdk_for_each_channel(&idxd_accel_engine, _config_max_desc, NULL, NULL);
}

static void
idxd_destroy_cb(void *io_device, void *ctx_buf)
{
	struct idxd_io_channel *chan = ctx_buf;

	pthread_mutex_lock(&g_configuration_lock);
	assert(chan->dev->num_channels > 0);
	chan->dev->num_channels--;
	spdk_idxd_reconfigure_chan(chan->chan, 0);
	pthread_mutex_unlock(&g_configuration_lock);

	spdk_poller_unregister(&chan->poller);
	spdk_idxd_put_channel(chan->chan);

	/* Pause each channel then rebalance the max number of ring slots. */
	spdk_for_each_channel(&idxd_accel_engine, _pause_chan, NULL,
			      _pause_chan_destroy_done);
}

static struct spdk_io_channel *
idxd_get_io_channel(void)
{
	return spdk_get_io_channel(&idxd_accel_engine);
}

static bool
probe_cb(void *cb_ctx, struct spdk_pci_device *pci_dev)
{
	struct spdk_pci_addr pci_addr = spdk_pci_device_get_addr(pci_dev);
	struct pci_device *pdev;

	SPDK_NOTICELOG(
		" Found matching device at %04x:%02x:%02x.%x vendor:0x%04x device:0x%04x\n",
		pci_addr.domain,
		pci_addr.bus,
		pci_addr.dev,
		pci_addr.func,
		spdk_pci_device_get_vendor_id(pci_dev),
		spdk_pci_device_get_device_id(pci_dev));

	pdev = calloc(1, sizeof(*pdev));
	if (pdev == NULL) {
		return false;
	}
	pdev->pci_dev = pci_dev;
	TAILQ_INSERT_TAIL(&g_pci_devices, pdev, tailq);

	/* Claim the device in case conflict with other process */
	if (spdk_pci_device_claim(pci_dev) < 0) {
		return false;
	}

	return true;
}

static void
attach_cb(void *cb_ctx, struct spdk_pci_device *pci_dev, struct spdk_idxd_device *idxd)
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

	if (spdk_idxd_probe(NULL, probe_cb, attach_cb) != 0) {
		SPDK_ERRLOG("spdk_idxd_probe() failed\n");
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
	struct pci_device *pci_dev;

	if (g_idxd_initialized) {
		spdk_io_device_unregister(&idxd_accel_engine, NULL);
	}

	while (!TAILQ_EMPTY(&g_idxd_devices)) {
		dev = TAILQ_FIRST(&g_idxd_devices);
		TAILQ_REMOVE(&g_idxd_devices, dev, tailq);
		spdk_idxd_detach(dev->idxd);
		free(dev);
	}

	while (!TAILQ_EMPTY(&g_pci_devices)) {
		pci_dev = TAILQ_FIRST(&g_pci_devices);
		TAILQ_REMOVE(&g_pci_devices, pci_dev, tailq);
		spdk_pci_device_detach(pci_dev->pci_dev);
		free(pci_dev);
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
			   NULL, accel_engine_idxd_write_config_json,
			   accel_engine_idxd_get_ctx_size)

SPDK_LOG_REGISTER_COMPONENT("accel_idxd", SPDK_LOG_ACCEL_IDXD)
