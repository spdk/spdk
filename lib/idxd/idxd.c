/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
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

#include "spdk/stdinc.h"

#include "spdk/env.h"
#include "spdk/util.h"
#include "spdk/memory.h"
#include "spdk/likely.h"

#include "spdk/log.h"
#include "spdk_internal/idxd.h"

#include "idxd.h"

#define ALIGN_4K 0x1000
#define USERSPACE_DRIVER_NAME "user"
#define KERNEL_DRIVER_NAME "kernel"

static STAILQ_HEAD(, spdk_idxd_impl) g_idxd_impls = STAILQ_HEAD_INITIALIZER(g_idxd_impls);
static struct spdk_idxd_impl *g_idxd_impl;

/*
 * g_dev_cfg gives us 2 pre-set configurations of DSA to choose from
 * via RPC.
 */
struct device_config *g_dev_cfg = NULL;

/*
 * Pre-built configurations. Variations depend on various factors
 * including how many different types of target latency profiles there
 * are, how many different QOS requirements there might be, etc.
 */
struct device_config g_dev_cfg0 = {
	.config_num = 0,
	.num_groups = 1,
	.total_wqs = 1,
	.total_engines = 4,
};

struct device_config g_dev_cfg1 = {
	.config_num = 1,
	.num_groups = 2,
	.total_wqs = 4,
	.total_engines = 4,
};

uint32_t
spdk_idxd_get_socket(struct spdk_idxd_device *idxd)
{
	return idxd->socket_id;
}

static inline void
_submit_to_hw(struct spdk_idxd_io_channel *chan, struct idxd_ops *op)
{
	TAILQ_INSERT_TAIL(&chan->ops_outstanding, op, link);
	movdir64b(chan->portal + chan->portal_offset, op->desc);
	chan->portal_offset = (chan->portal_offset + chan->idxd->chan_per_device * PORTAL_STRIDE) &
			      PORTAL_MASK;
}

inline static int
_vtophys(const void *buf, uint64_t *buf_addr, uint64_t size)
{
	uint64_t updated_size = size;

	*buf_addr = spdk_vtophys(buf, &updated_size);

	if (*buf_addr == SPDK_VTOPHYS_ERROR) {
		SPDK_ERRLOG("Error translating address\n");
		return -EINVAL;
	}

	if (updated_size < size) {
		SPDK_ERRLOG("Error translating size (0x%lx), return size (0x%lx)\n", size, updated_size);
		return -EINVAL;
	}

	return 0;
}

struct spdk_idxd_io_channel *
spdk_idxd_get_channel(struct spdk_idxd_device *idxd)
{
	struct spdk_idxd_io_channel *chan;
	struct idxd_batch *batch;
	struct idxd_hw_desc *desc;
	struct idxd_ops *op;
	int i, j, num_batches, num_descriptors, rc;

	assert(idxd != NULL);

	chan = calloc(1, sizeof(struct spdk_idxd_io_channel));
	if (chan == NULL) {
		SPDK_ERRLOG("Failed to allocate idxd chan\n");
		return NULL;
	}

	chan->idxd = idxd;
	TAILQ_INIT(&chan->ops_pool);
	TAILQ_INIT(&chan->batch_pool);
	TAILQ_INIT(&chan->ops_outstanding);

	/* Assign WQ, portal */
	pthread_mutex_lock(&idxd->num_channels_lock);
	if (idxd->num_channels == idxd->chan_per_device) {
		/* too many channels sharing this device */
		pthread_mutex_unlock(&idxd->num_channels_lock);
		goto err_chan;
	}

	/* Have each channel start at a different offset. */
	chan->portal = idxd->impl->portal_get_addr(idxd);
	chan->portal_offset = (idxd->num_channels * PORTAL_STRIDE) & PORTAL_MASK;
	idxd->num_channels++;

	/* Round robin the WQ selection for the chan on this IDXD device. */
	idxd->wq_id++;
	if (idxd->wq_id == g_dev_cfg->total_wqs) {
		idxd->wq_id = 0;
	}

	pthread_mutex_unlock(&idxd->num_channels_lock);

	/* Allocate descriptors and completions */
	num_descriptors = idxd->queues[idxd->wq_id].wqcfg.wq_size / idxd->chan_per_device;
	chan->desc_base = desc = spdk_zmalloc(num_descriptors * sizeof(struct idxd_hw_desc),
					      0x40, NULL,
					      SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (chan->desc_base == NULL) {
		SPDK_ERRLOG("Failed to allocate descriptor memory\n");
		goto err_chan;
	}

	chan->ops_base = op = spdk_zmalloc(num_descriptors * sizeof(struct idxd_ops),
					   0x40, NULL,
					   SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (chan->ops_base == NULL) {
		SPDK_ERRLOG("Failed to allocate completion memory\n");
		goto err_op;
	}

	for (i = 0; i < num_descriptors; i++) {
		TAILQ_INSERT_TAIL(&chan->ops_pool, op, link);
		op->desc = desc;
		rc = _vtophys(&op->hw, &desc->completion_addr, sizeof(struct idxd_hw_comp_record));
		if (rc) {
			SPDK_ERRLOG("Failed to translate completion memory\n");
			goto err_op;
		}
		op++;
		desc++;
	}

	/* Allocate batches */
	num_batches = idxd->queues[idxd->wq_id].wqcfg.wq_size / idxd->chan_per_device;
	chan->batch_base = calloc(num_batches, sizeof(struct idxd_batch));
	if (chan->batch_base == NULL) {
		SPDK_ERRLOG("Failed to allocate batch pool\n");
		goto err_op;
	}
	batch = chan->batch_base;
	for (i = 0 ; i < num_batches ; i++) {
		batch->user_desc = desc = spdk_zmalloc(DESC_PER_BATCH * sizeof(struct idxd_hw_desc),
						       0x40, NULL,
						       SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		if (batch->user_desc == NULL) {
			SPDK_ERRLOG("Failed to allocate batch descriptor memory\n");
			goto err_user_desc_or_op;
		}

		rc = _vtophys(batch->user_desc, &batch->user_desc_addr,
			      DESC_PER_BATCH * sizeof(struct idxd_hw_desc));
		if (rc) {
			SPDK_ERRLOG("Failed to translate batch descriptor memory\n");
			goto err_user_desc_or_op;
		}

		batch->user_ops = op = spdk_zmalloc(DESC_PER_BATCH * sizeof(struct idxd_ops),
						    0x40, NULL,
						    SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		if (batch->user_ops == NULL) {
			SPDK_ERRLOG("Failed to allocate user completion memory\n");
			goto err_user_desc_or_op;
		}

		for (j = 0; j < DESC_PER_BATCH; j++) {
			rc = _vtophys(&op->hw, &desc->completion_addr, sizeof(struct idxd_hw_comp_record));
			if (rc) {
				SPDK_ERRLOG("Failed to translate batch entry completion memory\n");
				goto err_user_desc_or_op;
			}
			op++;
			desc++;
		}

		TAILQ_INSERT_TAIL(&chan->batch_pool, batch, link);
		batch++;
	}

	return chan;

err_user_desc_or_op:
	TAILQ_FOREACH(batch, &chan->batch_pool, link) {
		spdk_free(batch->user_desc);
		batch->user_desc = NULL;
		spdk_free(batch->user_ops);
		batch->user_ops = NULL;
	}
	spdk_free(chan->ops_base);
	chan->ops_base = NULL;
err_op:
	spdk_free(chan->desc_base);
	chan->desc_base = NULL;
err_chan:
	free(chan);
	return NULL;
}

static int idxd_batch_cancel(struct spdk_idxd_io_channel *chan, struct idxd_batch *batch,
			     int status);

void
spdk_idxd_put_channel(struct spdk_idxd_io_channel *chan)
{
	struct idxd_batch *batch;

	assert(chan != NULL);

	if (chan->batch) {
		assert(chan->batch->transparent);
		idxd_batch_cancel(chan, chan->batch, -ECANCELED);
	}

	pthread_mutex_lock(&chan->idxd->num_channels_lock);
	assert(chan->idxd->num_channels > 0);
	chan->idxd->num_channels--;
	pthread_mutex_unlock(&chan->idxd->num_channels_lock);

	spdk_free(chan->ops_base);
	spdk_free(chan->desc_base);
	while ((batch = TAILQ_FIRST(&chan->batch_pool))) {
		TAILQ_REMOVE(&chan->batch_pool, batch, link);
		spdk_free(batch->user_ops);
		spdk_free(batch->user_desc);
	}
	free(chan->batch_base);
	free(chan);
}

static inline struct spdk_idxd_impl *
idxd_get_impl_by_name(const char *impl_name)
{
	struct spdk_idxd_impl *impl;

	assert(impl_name != NULL);
	STAILQ_FOREACH(impl, &g_idxd_impls, link) {
		if (0 == strcmp(impl_name, impl->name)) {
			return impl;
		}
	}

	return NULL;
}

/* Called via RPC to select a pre-defined configuration. */
void
spdk_idxd_set_config(uint32_t config_num, bool kernel_mode)
{
	if (kernel_mode) {
		g_idxd_impl = idxd_get_impl_by_name(KERNEL_DRIVER_NAME);
	} else {
		g_idxd_impl = idxd_get_impl_by_name(USERSPACE_DRIVER_NAME);
	}

	if (g_idxd_impl == NULL) {
		SPDK_ERRLOG("Cannot set the idxd implementation with %s mode\n",
			    kernel_mode ? KERNEL_DRIVER_NAME : USERSPACE_DRIVER_NAME);
		return;
	}

	switch (config_num) {
	case 0:
		g_dev_cfg = &g_dev_cfg0;
		break;
	case 1:
		g_dev_cfg = &g_dev_cfg1;
		break;
	default:
		g_dev_cfg = &g_dev_cfg0;
		SPDK_ERRLOG("Invalid config, using default\n");
		break;
	}

	g_idxd_impl->set_config(g_dev_cfg, config_num);
}

static void
idxd_device_destruct(struct spdk_idxd_device *idxd)
{
	assert(idxd->impl != NULL);

	idxd->impl->destruct(idxd);
}

int
spdk_idxd_probe(void *cb_ctx, spdk_idxd_attach_cb attach_cb)
{
	if (g_idxd_impl == NULL) {
		SPDK_ERRLOG("No idxd impl is selected\n");
		return -1;
	}

	return g_idxd_impl->probe(cb_ctx, attach_cb);
}

void
spdk_idxd_detach(struct spdk_idxd_device *idxd)
{
	assert(idxd != NULL);
	idxd_device_destruct(idxd);
}

static int
_idxd_prep_command(struct spdk_idxd_io_channel *chan, spdk_idxd_req_cb cb_fn,
		   void *cb_arg, struct idxd_hw_desc **_desc, struct idxd_ops **_op)
{
	struct idxd_hw_desc *desc;
	struct idxd_ops *op;
	uint64_t comp_addr;

	if (!TAILQ_EMPTY(&chan->ops_pool)) {
		op = *_op = TAILQ_FIRST(&chan->ops_pool);
		desc = *_desc = op->desc;
		comp_addr = desc->completion_addr;
		memset(desc, 0, sizeof(*desc));
		desc->completion_addr = comp_addr;
		TAILQ_REMOVE(&chan->ops_pool, op, link);
	} else {
		/* The application needs to handle this, violation of flow control */
		return -EBUSY;
	}

	desc->flags = IDXD_FLAG_COMPLETION_ADDR_VALID | IDXD_FLAG_REQUEST_COMPLETION;
	op->cb_arg = cb_arg;
	op->cb_fn = cb_fn;
	op->batch = NULL;

	return 0;
}

static bool
_is_batch_valid(struct idxd_batch *batch, struct spdk_idxd_io_channel *chan)
{
	return batch->chan == chan;
}

static int
_idxd_prep_batch_cmd(struct spdk_idxd_io_channel *chan, spdk_idxd_req_cb cb_fn,
		     void *cb_arg, struct idxd_batch *batch,
		     struct idxd_hw_desc **_desc, struct idxd_ops **_op)
{
	struct idxd_hw_desc *desc;
	struct idxd_ops *op;

	if (_is_batch_valid(batch, chan) == false) {
		SPDK_ERRLOG("Attempt to add to an invalid batch.\n");
		return -EINVAL;
	}

	assert(batch != NULL); /* suppress scan-build warning. */
	if (batch->index == DESC_PER_BATCH) {
		return -EBUSY;
	}

	desc = *_desc = &batch->user_desc[batch->index];
	op = *_op = &batch->user_ops[batch->index];

	op->desc = desc;
	SPDK_DEBUGLOG(idxd, "Prep batch %p index %u\n", batch, batch->index);

	batch->index++;

	desc->flags = IDXD_FLAG_COMPLETION_ADDR_VALID | IDXD_FLAG_REQUEST_COMPLETION;
	op->cb_arg = cb_arg;
	op->cb_fn = cb_fn;
	op->batch = batch;

	return 0;
}

static struct idxd_batch *
idxd_batch_create(struct spdk_idxd_io_channel *chan, bool transparent)
{
	struct idxd_batch *batch;

	assert(chan != NULL);

	if (!TAILQ_EMPTY(&chan->batch_pool)) {
		batch = TAILQ_FIRST(&chan->batch_pool);
		batch->index = 0;
		batch->chan = chan;
		batch->transparent = transparent;
		if (transparent) {
			/* this is the active transparent batch */
			chan->batch = batch;
		}
		TAILQ_REMOVE(&chan->batch_pool, batch, link);
	} else {
		/* The application needs to handle this. */
		return NULL;
	}

	return batch;
}

static void
_free_batch(struct idxd_batch *batch, struct spdk_idxd_io_channel *chan)
{
	SPDK_DEBUGLOG(idxd, "Free batch %p\n", batch);
	batch->index = 0;
	batch->chan = NULL;
	TAILQ_INSERT_TAIL(&chan->batch_pool, batch, link);
}

static int
idxd_batch_cancel(struct spdk_idxd_io_channel *chan, struct idxd_batch *batch, int status)
{
	struct idxd_ops *op;
	int i;

	assert(chan != NULL);
	assert(batch != NULL);

	if (_is_batch_valid(batch, chan) == false) {
		SPDK_ERRLOG("Attempt to cancel an invalid batch.\n");
		return -EINVAL;
	}

	if (batch->index == UINT8_MAX) {
		SPDK_ERRLOG("Cannot cancel batch, already submitted to HW.\n");
		return -EINVAL;
	}

	if (batch->transparent) {
		chan->batch = NULL;
	}

	for (i = 0; i < batch->index; i++) {
		op = &batch->user_ops[i];
		if (op->cb_fn) {
			op->cb_fn(op->cb_arg, status);
		}
	}

	_free_batch(batch, chan);

	return 0;
}

static int
idxd_batch_submit(struct spdk_idxd_io_channel *chan, struct idxd_batch *batch,
		  spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_ops *op;
	int i, rc;

	assert(chan != NULL);
	assert(batch != NULL);

	if (_is_batch_valid(batch, chan) == false) {
		SPDK_ERRLOG("Attempt to submit an invalid batch.\n");
		return -EINVAL;
	}

	if (batch->index == 0) {
		return idxd_batch_cancel(chan, batch, 0);
	}

	/* Common prep. */
	rc = _idxd_prep_command(chan, cb_fn, cb_arg, &desc, &op);
	if (rc) {
		return rc;
	}

	if (batch->index == 1) {
		uint64_t completion_addr;

		/* If there's only one command, convert it away from a batch. */
		completion_addr = desc->completion_addr;
		memcpy(desc, &batch->user_desc[0], sizeof(*desc));
		desc->completion_addr = completion_addr;
		op->cb_fn = batch->user_ops[0].cb_fn;
		op->cb_arg = batch->user_ops[0].cb_arg;
		op->crc_dst = batch->user_ops[0].crc_dst;
		batch->index = 0;
		idxd_batch_cancel(chan, batch, 0);
	} else {
		/* Command specific. */
		desc->opcode = IDXD_OPCODE_BATCH;
		desc->desc_list_addr = batch->user_desc_addr;
		desc->desc_count = batch->index;
		op->batch = batch;
		assert(batch->index <= DESC_PER_BATCH);

		/* Add the batch elements completion contexts to the outstanding list to be polled. */
		for (i = 0 ; i < batch->index; i++) {
			TAILQ_INSERT_TAIL(&chan->ops_outstanding, (struct idxd_ops *)&batch->user_ops[i],
					  link);
		}
		batch->index = UINT8_MAX;
		if (batch->transparent == true) {
			/* for transparent batching, once we submit this batch it is no longer availablee */
			chan->batch = NULL;
		}
	}

	/* Submit operation. */
	_submit_to_hw(chan, op);
	SPDK_DEBUGLOG(idxd, "Submitted batch %p\n", batch);

	return 0;
}

static int
_idxd_setup_batch(struct spdk_idxd_io_channel *chan)
{
	struct idxd_batch *batch;

	if (chan->batch == NULL) {
		/* Open a new transparent batch */
		batch = idxd_batch_create(chan, true);
		if (batch == NULL) {
			return -EBUSY;
		}
	} /* else use the existing one */

	return 0;
}

static int
_idxd_flush_batch(struct spdk_idxd_io_channel *chan)
{
	int rc;

	if (chan->batch != NULL && chan->batch->index >= DESC_PER_BATCH) {
		assert(chan->batch->transparent);

		/* Close out the full batch */
		rc = idxd_batch_submit(chan, chan->batch, NULL, NULL);
		if (rc) {
			assert(rc == -EBUSY);
			/* return 0 here as this is a transparent batch and we want to try again
			 * internally.
			 */
			return 0;
		}
	}

	return 0;
}

static inline int
_idxd_submit_copy_single(struct spdk_idxd_io_channel *chan, void *dst, const void *src,
			 uint64_t nbytes, spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_ops *op;
	uint64_t src_addr, dst_addr;
	int rc;

	assert(chan != NULL);
	assert(dst != NULL);
	assert(src != NULL);

	rc = _idxd_setup_batch(chan);
	if (rc) {
		return rc;
	}

	/* Common prep. */
	rc = _idxd_prep_batch_cmd(chan, cb_fn, cb_arg, chan->batch, &desc, &op);
	if (rc) {
		return rc;
	}

	rc = _vtophys(src, &src_addr, nbytes);
	if (rc) {
		goto error;
	}

	rc = _vtophys(dst, &dst_addr, nbytes);
	if (rc) {
		goto error;
	}

	/* Command specific. */
	desc->opcode = IDXD_OPCODE_MEMMOVE;
	desc->src_addr = src_addr;
	desc->dst_addr = dst_addr;
	desc->xfer_size = nbytes;
	desc->flags |= IDXD_FLAG_CACHE_CONTROL; /* direct IO to CPU cache instead of mem */

	return _idxd_flush_batch(chan);

error:
	chan->batch->index--;
	return rc;
}

int
spdk_idxd_submit_copy(struct spdk_idxd_io_channel *chan,
		      struct iovec *diov, uint32_t diovcnt,
		      struct iovec *siov, uint32_t siovcnt,
		      spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_ops *op;
	void *src, *dst;
	uint64_t src_addr, dst_addr;
	int rc;
	uint64_t len;
	struct idxd_batch *batch;
	struct spdk_ioviter iter;

	assert(chan != NULL);
	assert(diov != NULL);
	assert(siov != NULL);

	if (diovcnt == 1 && siovcnt == 1) {
		/* Simple case - copying one buffer to another */
		if (diov[0].iov_len < siov[0].iov_len) {
			return -EINVAL;
		}

		return _idxd_submit_copy_single(chan, diov[0].iov_base,
						siov[0].iov_base, siov[0].iov_len,
						cb_fn, cb_arg);
	}

	if (chan->batch) {
		/* Close out existing batch */
		rc = idxd_batch_submit(chan, chan->batch, NULL, NULL);
		if (rc) {
			assert(rc == -EBUSY);
			/* we can't submit the existing transparent batch so reply to the incoming
			 * op that we are busy so it will try again.
			 */
			return -EBUSY;
		}
	}

	batch = idxd_batch_create(chan, false);
	if (!batch) {
		return -EBUSY;
	}

	for (len = spdk_ioviter_first(&iter, siov, siovcnt, diov, diovcnt, &src, &dst);
	     len > 0;
	     len = spdk_ioviter_next(&iter, &src, &dst)) {
		rc = _idxd_prep_batch_cmd(chan, NULL, NULL, batch, &desc, &op);
		if (rc) {
			goto err;
		}

		rc = _vtophys(src, &src_addr, len);
		if (rc) {
			goto err;
		}

		rc = _vtophys(dst, &dst_addr, len);
		if (rc) {
			goto err;
		}

		desc->opcode = IDXD_OPCODE_MEMMOVE;
		desc->src_addr = src_addr;
		desc->dst_addr = dst_addr;
		desc->xfer_size = len;
	}

	rc = idxd_batch_submit(chan, batch, cb_fn, cb_arg);
	if (rc) {
		assert(rc == -EBUSY);
		goto err;
	}

	return 0;
err:
	idxd_batch_cancel(chan, batch, rc);
	return rc;
}

/* Dual-cast copies the same source to two separate destination buffers. */
int
spdk_idxd_submit_dualcast(struct spdk_idxd_io_channel *chan, void *dst1, void *dst2,
			  const void *src, uint64_t nbytes, spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_ops *op;
	uint64_t src_addr, dst1_addr, dst2_addr;
	int rc;

	assert(chan != NULL);
	assert(dst1 != NULL);
	assert(dst2 != NULL);
	assert(src != NULL);

	if ((uintptr_t)dst1 & (ALIGN_4K - 1) || (uintptr_t)dst2 & (ALIGN_4K - 1)) {
		SPDK_ERRLOG("Dualcast requires 4K alignment on dst addresses\n");
		return -EINVAL;
	}

	/* Common prep. */
	rc = _idxd_prep_command(chan, cb_fn, cb_arg, &desc, &op);
	if (rc) {
		return rc;
	}

	rc = _vtophys(src, &src_addr, nbytes);
	if (rc) {
		goto error;
	}

	rc = _vtophys(dst1, &dst1_addr, nbytes);
	if (rc) {
		goto error;
	}

	rc = _vtophys(dst2, &dst2_addr, nbytes);
	if (rc) {
		goto error;
	}

	/* Command specific. */
	desc->opcode = IDXD_OPCODE_DUALCAST;
	desc->src_addr = src_addr;
	desc->dst_addr = dst1_addr;
	desc->dest2 = dst2_addr;
	desc->xfer_size = nbytes;
	desc->flags |= IDXD_FLAG_CACHE_CONTROL; /* direct IO to CPU cache instead of mem */

	/* Submit operation. */
	_submit_to_hw(chan, op);

	return 0;
error:
	TAILQ_INSERT_TAIL(&chan->ops_pool, op, link);
	return rc;
}

static inline int
_idxd_submit_compare_single(struct spdk_idxd_io_channel *chan, void *src1, const void *src2,
			    uint64_t nbytes, spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_ops *op;
	uint64_t src1_addr, src2_addr;
	int rc;

	assert(chan != NULL);
	assert(src1 != NULL);
	assert(src2 != NULL);

	rc = _idxd_setup_batch(chan);
	if (rc) {
		return rc;
	}

	/* Common prep. */
	rc = _idxd_prep_batch_cmd(chan, cb_fn, cb_arg, chan->batch, &desc, &op);
	if (rc) {
		return rc;
	}

	rc = _vtophys(src1, &src1_addr, nbytes);
	if (rc) {
		goto error;
	}

	rc = _vtophys(src2, &src2_addr, nbytes);
	if (rc) {
		goto error;
	}

	/* Command specific. */
	desc->opcode = IDXD_OPCODE_COMPARE;
	desc->src_addr = src1_addr;
	desc->src2_addr = src2_addr;
	desc->xfer_size = nbytes;

	return _idxd_flush_batch(chan);

error:
	chan->batch->index--;
	return rc;
}

int
spdk_idxd_submit_compare(struct spdk_idxd_io_channel *chan,
			 struct iovec *siov1, size_t siov1cnt,
			 struct iovec *siov2, size_t siov2cnt,
			 spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_ops *op;
	void *src1, *src2;
	uint64_t src1_addr, src2_addr;
	int rc;
	size_t len;
	struct idxd_batch *batch;
	struct spdk_ioviter iter;

	if (siov1cnt == 1 && siov2cnt == 1) {
		/* Simple case - comparing one buffer to another */
		if (siov1[0].iov_len != siov2[0].iov_len) {
			return -EINVAL;
		}

		return _idxd_submit_compare_single(chan, siov1[0].iov_base, siov2[0].iov_base, siov1[0].iov_len,
						   cb_fn, cb_arg);
	}

	if (chan->batch) {
		/* Close out existing batch */
		rc = idxd_batch_submit(chan, chan->batch, NULL, NULL);
		if (rc) {
			assert(rc == -EBUSY);
			/* we can't submit the existing transparent batch so reply to the incoming
			 * op that we are busy so it will try again.
			 */
			return -EBUSY;
		}
	}

	batch = idxd_batch_create(chan, false);
	if (!batch) {
		return -EBUSY;
	}

	for (len = spdk_ioviter_first(&iter, siov1, siov1cnt, siov2, siov2cnt, &src1, &src2);
	     len > 0;
	     len = spdk_ioviter_next(&iter, &src1, &src2)) {
		rc = _idxd_prep_batch_cmd(chan, NULL, NULL, batch, &desc, &op);
		if (rc) {
			goto err;
		}

		rc = _vtophys(src1, &src1_addr, len);
		if (rc) {
			goto err;
		}

		rc = _vtophys(src2, &src2_addr, len);
		if (rc) {
			goto err;
		}

		desc->opcode = IDXD_OPCODE_COMPARE;
		desc->src_addr = src1_addr;
		desc->src2_addr = src2_addr;
		desc->xfer_size = len;
	}

	rc = idxd_batch_submit(chan, batch, cb_fn, cb_arg);
	if (rc) {
		assert(rc == -EBUSY);
		goto err;
	}

	return 0;
err:
	idxd_batch_cancel(chan, batch, rc);
	return rc;
}

static inline int
_idxd_submit_fill_single(struct spdk_idxd_io_channel *chan, void *dst, uint64_t fill_pattern,
			 uint64_t nbytes, spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_ops *op;
	uint64_t dst_addr;
	int rc;

	assert(chan != NULL);
	assert(dst != NULL);

	rc = _idxd_setup_batch(chan);
	if (rc) {
		return rc;
	}

	/* Common prep. */
	rc = _idxd_prep_batch_cmd(chan, cb_fn, cb_arg, chan->batch, &desc, &op);
	if (rc) {
		return rc;
	}

	rc = _vtophys(dst, &dst_addr, nbytes);
	if (rc) {
		goto error;
	}

	/* Command specific. */
	desc->opcode = IDXD_OPCODE_MEMFILL;
	desc->pattern = fill_pattern;
	desc->dst_addr = dst_addr;
	desc->xfer_size = nbytes;
	desc->flags |= IDXD_FLAG_CACHE_CONTROL; /* direct IO to CPU cache instead of mem */

	return _idxd_flush_batch(chan);

error:
	chan->batch->index--;
	return rc;
}

int
spdk_idxd_submit_fill(struct spdk_idxd_io_channel *chan,
		      struct iovec *diov, size_t diovcnt,
		      uint64_t fill_pattern, spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_ops *op;
	uint64_t dst_addr;
	int rc;
	size_t i;
	struct idxd_batch *batch;

	if (diovcnt == 1) {
		/* Simple case - filling one buffer */
		return _idxd_submit_fill_single(chan, diov[0].iov_base, fill_pattern,
						diov[0].iov_len, cb_fn, cb_arg);
	}

	if (chan->batch) {
		/* Close out existing batch */
		rc = idxd_batch_submit(chan, chan->batch, NULL, NULL);
		if (rc) {
			assert(rc == -EBUSY);
			/* we can't submit the existing transparent batch so reply to the incoming
			 * op that we are busy so it will try again.
			 */
			return -EBUSY;
		}
	}

	batch = idxd_batch_create(chan, false);
	if (!batch) {
		return -EBUSY;
	}

	for (i = 0; i < diovcnt; i++) {
		rc = _idxd_prep_batch_cmd(chan, NULL, NULL, batch, &desc, &op);
		if (rc) {
			goto err;
		}

		rc = _vtophys(diov[i].iov_base, &dst_addr, diov[i].iov_len);
		if (rc) {
			goto err;
		}

		desc->opcode = IDXD_OPCODE_MEMFILL;
		desc->pattern = fill_pattern;
		desc->dst_addr = dst_addr;
		desc->xfer_size = diov[i].iov_len;
		desc->flags |= IDXD_FLAG_CACHE_CONTROL; /* direct IO to CPU cache instead of mem */
	}

	rc = idxd_batch_submit(chan, batch, cb_fn, cb_arg);
	if (rc) {
		assert(rc == -EBUSY);
		goto err;
	}

	return 0;
err:
	idxd_batch_cancel(chan, batch, rc);
	return rc;
}

static inline int
_idxd_submit_crc32c_single(struct spdk_idxd_io_channel *chan, uint32_t *crc_dst, void *src,
			   uint32_t seed, uint64_t nbytes,
			   spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_ops *op;
	uint64_t src_addr;
	int rc;

	assert(chan != NULL);
	assert(crc_dst != NULL);
	assert(src != NULL);

	rc = _idxd_setup_batch(chan);
	if (rc) {
		return rc;
	}

	/* Common prep. */
	rc = _idxd_prep_batch_cmd(chan, cb_fn, cb_arg, chan->batch, &desc, &op);
	if (rc) {
		return rc;
	}

	rc = _vtophys(src, &src_addr, nbytes);
	if (rc) {
		goto error;
	}

	/* Command specific. */
	desc->opcode = IDXD_OPCODE_CRC32C_GEN;
	desc->src_addr = src_addr;
	desc->flags &= IDXD_CLEAR_CRC_FLAGS;
	desc->crc32c.seed = seed;
	desc->xfer_size = nbytes;
	op->crc_dst = crc_dst;

	return _idxd_flush_batch(chan);

error:
	chan->batch->index--;
	return rc;
}

int
spdk_idxd_submit_crc32c(struct spdk_idxd_io_channel *chan,
			struct iovec *siov, size_t siovcnt,
			uint32_t seed, uint32_t *crc_dst,
			spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_ops *op = NULL;
	uint64_t src_addr;
	int rc;
	size_t i;
	struct idxd_batch *batch;
	void *prev_crc;

	if (siovcnt == 1) {
		/* Simple case - crc on one buffer */
		return _idxd_submit_crc32c_single(chan, crc_dst, siov[0].iov_base,
						  seed, siov[0].iov_len, cb_fn, cb_arg);
	}

	if (chan->batch) {
		/* Close out existing batch */
		rc = idxd_batch_submit(chan, chan->batch, NULL, NULL);
		if (rc) {
			assert(rc == -EBUSY);
			/* we can't submit the existing transparent batch so reply to the incoming
			 * op that we are busy so it will try again.
			 */
			return -EBUSY;
		}
	}

	batch = idxd_batch_create(chan, false);
	if (!batch) {
		return -EBUSY;
	}

	prev_crc = NULL;
	for (i = 0; i < siovcnt; i++) {
		rc = _idxd_prep_batch_cmd(chan, NULL, NULL, batch, &desc, &op);
		if (rc) {
			goto err;
		}

		rc = _vtophys(siov[i].iov_base, &src_addr, siov[i].iov_len);
		if (rc) {
			goto err;
		}

		desc->opcode = IDXD_OPCODE_CRC32C_GEN;
		desc->src_addr = src_addr;
		if (i == 0) {
			desc->crc32c.seed = seed;
		} else {
			desc->flags |= IDXD_FLAG_FENCE | IDXD_FLAG_CRC_READ_CRC_SEED;
			desc->crc32c.addr = (uint64_t)prev_crc;
		}

		desc->xfer_size = siov[i].iov_len;
		prev_crc = &op->hw.crc32c_val;
	}

	/* Only the last op copies the crc to the destination */
	if (op) {
		op->crc_dst = crc_dst;
	}

	rc = idxd_batch_submit(chan, batch, cb_fn, cb_arg);
	if (rc) {
		assert(rc == -EBUSY);
		goto err;
	}

	return 0;
err:
	idxd_batch_cancel(chan, batch, rc);
	return rc;
}

static inline int
_idxd_submit_copy_crc32c_single(struct spdk_idxd_io_channel *chan, void *dst, void *src,
				uint32_t *crc_dst, uint32_t seed, uint64_t nbytes,
				spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_ops *op;
	uint64_t src_addr, dst_addr;
	int rc;

	assert(chan != NULL);
	assert(dst != NULL);
	assert(src != NULL);
	assert(crc_dst != NULL);

	rc = _idxd_setup_batch(chan);
	if (rc) {
		return rc;
	}

	/* Common prep. */
	rc = _idxd_prep_batch_cmd(chan, cb_fn, cb_arg, chan->batch, &desc, &op);
	if (rc) {
		return rc;
	}

	rc = _vtophys(src, &src_addr, nbytes);
	if (rc) {
		goto error;
	}

	rc = _vtophys(dst, &dst_addr, nbytes);
	if (rc) {
		goto error;
	}

	/* Command specific. */
	desc->opcode = IDXD_OPCODE_COPY_CRC;
	desc->dst_addr = dst_addr;
	desc->src_addr = src_addr;
	desc->flags &= IDXD_CLEAR_CRC_FLAGS;
	desc->crc32c.seed = seed;
	desc->xfer_size = nbytes;
	op->crc_dst = crc_dst;

	return _idxd_flush_batch(chan);

error:
	chan->batch->index--;
	return rc;
}

int
spdk_idxd_submit_copy_crc32c(struct spdk_idxd_io_channel *chan,
			     struct iovec *diov, size_t diovcnt,
			     struct iovec *siov, size_t siovcnt,
			     uint32_t seed, uint32_t *crc_dst,
			     spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_ops *op = NULL;
	void *src, *dst;
	uint64_t src_addr, dst_addr;
	int rc;
	uint64_t len;
	struct idxd_batch *batch;
	struct spdk_ioviter iter;
	void *prev_crc;

	assert(chan != NULL);
	assert(diov != NULL);
	assert(siov != NULL);

	if (siovcnt == 1 && diovcnt == 1) {
		/* Simple case - crc on one buffer */
		return _idxd_submit_copy_crc32c_single(chan, diov[0].iov_base, siov[0].iov_base,
						       crc_dst, seed, siov[0].iov_len, cb_fn, cb_arg);
	}

	if (chan->batch) {
		/* Close out existing batch */
		rc = idxd_batch_submit(chan, chan->batch, NULL, NULL);
		if (rc) {
			assert(rc == -EBUSY);
			/* we can't submit the existing transparent batch so reply to the incoming
			 * op that we are busy so it will try again.
			 */
			return -EBUSY;
		}
	}

	batch = idxd_batch_create(chan, false);
	if (!batch) {
		return -EBUSY;
	}

	prev_crc = NULL;
	for (len = spdk_ioviter_first(&iter, siov, siovcnt, diov, diovcnt, &src, &dst);
	     len > 0;
	     len = spdk_ioviter_next(&iter, &src, &dst)) {
		rc = _idxd_prep_batch_cmd(chan, NULL, NULL, batch, &desc, &op);
		if (rc) {
			goto err;
		}

		rc = _vtophys(src, &src_addr, len);
		if (rc) {
			goto err;
		}

		rc = _vtophys(dst, &dst_addr, len);
		if (rc) {
			goto err;
		}

		desc->opcode = IDXD_OPCODE_COPY_CRC;
		desc->dst_addr = dst_addr;
		desc->src_addr = src_addr;
		if (prev_crc == NULL) {
			desc->crc32c.seed = seed;
		} else {
			desc->flags |= IDXD_FLAG_FENCE | IDXD_FLAG_CRC_READ_CRC_SEED;
			desc->crc32c.addr = (uint64_t)prev_crc;
		}

		desc->xfer_size = len;
		prev_crc = &op->hw.crc32c_val;
	}

	/* Only the last op copies the crc to the destination */
	if (op) {
		op->crc_dst = crc_dst;
	}

	rc = idxd_batch_submit(chan, batch, cb_fn, cb_arg);
	if (rc) {
		assert(rc == -EBUSY);
		goto err;
	}

	return 0;
err:
	idxd_batch_cancel(chan, batch, rc);
	return rc;
}

static inline void
_dump_sw_error_reg(struct spdk_idxd_io_channel *chan)
{
	struct spdk_idxd_device *idxd = chan->idxd;

	assert(idxd != NULL);
	idxd->impl->dump_sw_error(idxd, chan->portal);
}

/* TODO: more performance experiments. */
#define IDXD_COMPLETION(x) ((x) > (0) ? (1) : (0))
#define IDXD_FAILURE(x) ((x) > (1) ? (1) : (0))
#define IDXD_SW_ERROR(x) ((x) &= (0x1) ? (1) : (0))
int
spdk_idxd_process_events(struct spdk_idxd_io_channel *chan)
{
	struct idxd_ops *op, *tmp;
	int status = 0;
	int rc2, rc = 0;
	void *cb_arg;
	spdk_idxd_req_cb cb_fn;

	assert(chan != NULL);

	TAILQ_FOREACH_SAFE(op, &chan->ops_outstanding, link, tmp) {
		if (IDXD_COMPLETION(op->hw.status)) {

			TAILQ_REMOVE(&chan->ops_outstanding, op, link);
			rc++;

			if (spdk_unlikely(IDXD_FAILURE(op->hw.status))) {
				status = -EINVAL;
				_dump_sw_error_reg(chan);
			}

			switch (op->desc->opcode) {
			case IDXD_OPCODE_BATCH:
				SPDK_DEBUGLOG(idxd, "Complete batch %p\n", op->batch);
				break;
			case IDXD_OPCODE_CRC32C_GEN:
			case IDXD_OPCODE_COPY_CRC:
				if (spdk_likely(status == 0 && op->crc_dst != NULL)) {
					*op->crc_dst = op->hw.crc32c_val;
					*op->crc_dst ^= ~0;
				}
				break;
			case IDXD_OPCODE_COMPARE:
				if (spdk_likely(status == 0)) {
					status = op->hw.result;
				}
				break;
			}

			cb_fn = op->cb_fn;
			cb_arg = op->cb_arg;
			op->hw.status = 0;
			if (op->desc->opcode == IDXD_OPCODE_BATCH) {
				_free_batch(op->batch, chan);
				TAILQ_INSERT_HEAD(&chan->ops_pool, op, link);
			} else if (!op->batch) {
				TAILQ_INSERT_HEAD(&chan->ops_pool, op, link);
			}

			if (cb_fn) {
				cb_fn(cb_arg, status);
			}

			/* reset the status */
			status = 0;
		} else {
			/*
			 * oldest locations are at the head of the list so if
			 * we've polled a location that hasn't completed, bail
			 * now as there are unlikely to be any more completions.
			 */
			break;
		}
	}

	/* Submit any built-up batch */
	if (chan->batch) {
		rc2 = idxd_batch_submit(chan, chan->batch, NULL, NULL);
		if (rc2) {
			assert(rc2 == -EBUSY);
		}
	}

	return rc;
}

void
idxd_impl_register(struct spdk_idxd_impl *impl)
{
	STAILQ_INSERT_HEAD(&g_idxd_impls, impl, link);
}

SPDK_LOG_REGISTER_COMPONENT(idxd)
