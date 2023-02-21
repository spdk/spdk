/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/env.h"
#include "spdk/util.h"
#include "spdk/memory.h"
#include "spdk/likely.h"

#include "spdk/log.h"
#include "spdk_internal/idxd.h"

#include "idxd_internal.h"

#define ALIGN_4K 0x1000
#define USERSPACE_DRIVER_NAME "user"
#define KERNEL_DRIVER_NAME "kernel"

/* The max number of completions processed per poll */
#define IDXD_MAX_COMPLETIONS      128

static STAILQ_HEAD(, spdk_idxd_impl) g_idxd_impls = STAILQ_HEAD_INITIALIZER(g_idxd_impls);
static struct spdk_idxd_impl *g_idxd_impl;

uint32_t
spdk_idxd_get_socket(struct spdk_idxd_device *idxd)
{
	return idxd->socket_id;
}

static inline void
_submit_to_hw(struct spdk_idxd_io_channel *chan, struct idxd_ops *op)
{
	STAILQ_INSERT_TAIL(&chan->ops_outstanding, op, link);
	/*
	 * We must barrier before writing the descriptor to ensure that data
	 * has been correctly flushed from the associated data buffers before DMA
	 * operations begin.
	 */
	_spdk_wmb();
	movdir64b(chan->portal + chan->portal_offset, op->desc);
	chan->portal_offset = (chan->portal_offset + chan->idxd->chan_per_device * PORTAL_STRIDE) &
			      PORTAL_MASK;
}

inline static int
_vtophys(struct spdk_idxd_io_channel *chan, const void *buf, uint64_t *buf_addr, uint64_t size)
{
	uint64_t updated_size = size;

	if (chan->pasid_enabled) {
		/* We can just use virtual addresses */
		*buf_addr = (uint64_t)buf;
		return 0;
	}

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

struct idxd_vtophys_iter {
	const void	*src;
	void		*dst;
	uint64_t	len;

	uint64_t	offset;

	bool		pasid_enabled;
};

static void
idxd_vtophys_iter_init(struct spdk_idxd_io_channel *chan,
		       struct idxd_vtophys_iter *iter,
		       const void *src, void *dst, uint64_t len)
{
	iter->src = src;
	iter->dst = dst;
	iter->len = len;
	iter->offset = 0;
	iter->pasid_enabled = chan->pasid_enabled;
}

static uint64_t
idxd_vtophys_iter_next(struct idxd_vtophys_iter *iter,
		       uint64_t *src_phys, uint64_t *dst_phys)
{
	uint64_t src_off, dst_off, len;
	const void *src;
	void *dst;

	src = iter->src + iter->offset;
	dst = iter->dst + iter->offset;

	if (iter->offset == iter->len) {
		return 0;
	}

	if (iter->pasid_enabled) {
		*src_phys = (uint64_t)src;
		*dst_phys = (uint64_t)dst;
		return iter->len;
	}

	len = iter->len - iter->offset;

	src_off = len;
	*src_phys = spdk_vtophys(src, &src_off);
	if (*src_phys == SPDK_VTOPHYS_ERROR) {
		SPDK_ERRLOG("Error translating address\n");
		return SPDK_VTOPHYS_ERROR;
	}

	dst_off = len;
	*dst_phys = spdk_vtophys(dst, &dst_off);
	if (*dst_phys == SPDK_VTOPHYS_ERROR) {
		SPDK_ERRLOG("Error translating address\n");
		return SPDK_VTOPHYS_ERROR;
	}

	len = spdk_min(src_off, dst_off);
	iter->offset += len;

	return len;
}

/* helper function for DSA specific spdk_idxd_get_channel() stuff */
static int
_dsa_alloc_batches(struct spdk_idxd_io_channel *chan, int num_descriptors)
{
	struct idxd_batch *batch;
	struct idxd_hw_desc *desc;
	struct idxd_ops *op;
	int i, j, num_batches, rc = -1;

	/* Allocate batches */
	num_batches = num_descriptors;
	chan->batch_base = calloc(num_batches, sizeof(struct idxd_batch));
	if (chan->batch_base == NULL) {
		SPDK_ERRLOG("Failed to allocate batch pool\n");
		goto error_desc;
	}
	batch = chan->batch_base;
	for (i = 0 ; i < num_batches ; i++) {
		batch->user_desc = desc = spdk_zmalloc(DESC_PER_BATCH * sizeof(struct idxd_hw_desc),
						       0x40, NULL,
						       SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		if (batch->user_desc == NULL) {
			SPDK_ERRLOG("Failed to allocate batch descriptor memory\n");
			goto error_user;
		}

		rc = _vtophys(chan, batch->user_desc, &batch->user_desc_addr,
			      DESC_PER_BATCH * sizeof(struct idxd_hw_desc));
		if (rc) {
			SPDK_ERRLOG("Failed to translate batch descriptor memory\n");
			goto error_user;
		}

		batch->user_ops = op = spdk_zmalloc(DESC_PER_BATCH * sizeof(struct idxd_ops),
						    0x40, NULL,
						    SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		if (batch->user_ops == NULL) {
			SPDK_ERRLOG("Failed to allocate user completion memory\n");
			goto error_user;
		}

		for (j = 0; j < DESC_PER_BATCH; j++) {
			rc = _vtophys(chan, &op->hw, &desc->completion_addr, sizeof(struct dsa_hw_comp_record));
			if (rc) {
				SPDK_ERRLOG("Failed to translate batch entry completion memory\n");
				goto error_user;
			}
			op++;
			desc++;
		}
		TAILQ_INSERT_TAIL(&chan->batch_pool, batch, link);
		batch++;
	}
	return 0;

error_user:
	TAILQ_FOREACH(batch, &chan->batch_pool, link) {
		spdk_free(batch->user_ops);
		batch->user_ops = NULL;
		spdk_free(batch->user_desc);
		batch->user_desc = NULL;
	}
	spdk_free(chan->ops_base);
	chan->ops_base = NULL;
error_desc:
	STAILQ_INIT(&chan->ops_pool);
	spdk_free(chan->desc_base);
	chan->desc_base = NULL;
	return rc;
}

struct spdk_idxd_io_channel *
spdk_idxd_get_channel(struct spdk_idxd_device *idxd)
{
	struct spdk_idxd_io_channel *chan;
	struct idxd_hw_desc *desc;
	struct idxd_ops *op;
	int i, num_descriptors, rc = -1;
	uint32_t comp_rec_size;

	assert(idxd != NULL);

	chan = calloc(1, sizeof(struct spdk_idxd_io_channel));
	if (chan == NULL) {
		SPDK_ERRLOG("Failed to allocate idxd chan\n");
		return NULL;
	}

	chan->idxd = idxd;
	chan->pasid_enabled = idxd->pasid_enabled;
	STAILQ_INIT(&chan->ops_pool);
	TAILQ_INIT(&chan->batch_pool);
	STAILQ_INIT(&chan->ops_outstanding);

	/* Assign WQ, portal */
	pthread_mutex_lock(&idxd->num_channels_lock);
	if (idxd->num_channels == idxd->chan_per_device) {
		/* too many channels sharing this device */
		pthread_mutex_unlock(&idxd->num_channels_lock);
		SPDK_ERRLOG("Too many channels sharing this device\n");
		goto error;
	}

	/* Have each channel start at a different offset. */
	chan->portal = idxd->impl->portal_get_addr(idxd);
	chan->portal_offset = (idxd->num_channels * PORTAL_STRIDE) & PORTAL_MASK;
	idxd->num_channels++;

	pthread_mutex_unlock(&idxd->num_channels_lock);

	/* Allocate descriptors and completions */
	num_descriptors = idxd->total_wq_size / idxd->chan_per_device;
	chan->desc_base = desc = spdk_zmalloc(num_descriptors * sizeof(struct idxd_hw_desc),
					      0x40, NULL,
					      SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (chan->desc_base == NULL) {
		SPDK_ERRLOG("Failed to allocate DSA descriptor memory\n");
		goto error;
	}

	chan->ops_base = op = spdk_zmalloc(num_descriptors * sizeof(struct idxd_ops),
					   0x40, NULL,
					   SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (chan->ops_base == NULL) {
		SPDK_ERRLOG("Failed to allocate idxd_ops memory\n");
		goto error;
	}

	if (idxd->type == IDXD_DEV_TYPE_DSA) {
		comp_rec_size = sizeof(struct dsa_hw_comp_record);
		if (_dsa_alloc_batches(chan, num_descriptors)) {
			goto error;
		}
	} else {
		comp_rec_size = sizeof(struct iaa_hw_comp_record);
	}

	for (i = 0; i < num_descriptors; i++) {
		STAILQ_INSERT_TAIL(&chan->ops_pool, op, link);
		op->desc = desc;
		rc = _vtophys(chan, &op->hw, &desc->completion_addr, comp_rec_size);
		if (rc) {
			SPDK_ERRLOG("Failed to translate completion memory\n");
			goto error;
		}
		op++;
		desc++;
	}

	return chan;

error:
	spdk_free(chan->ops_base);
	chan->ops_base = NULL;
	spdk_free(chan->desc_base);
	chan->desc_base = NULL;
	free(chan);
	return NULL;
}

static int idxd_batch_cancel(struct spdk_idxd_io_channel *chan, int status);

void
spdk_idxd_put_channel(struct spdk_idxd_io_channel *chan)
{
	struct idxd_batch *batch;

	assert(chan != NULL);
	assert(chan->idxd != NULL);

	if (chan->batch) {
		idxd_batch_cancel(chan, -ECANCELED);
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

void
spdk_idxd_set_config(bool kernel_mode)
{
	struct spdk_idxd_impl *tmp;

	if (kernel_mode) {
		tmp = idxd_get_impl_by_name(KERNEL_DRIVER_NAME);
	} else {
		tmp = idxd_get_impl_by_name(USERSPACE_DRIVER_NAME);
	}

	if (g_idxd_impl != NULL && g_idxd_impl != tmp) {
		SPDK_ERRLOG("Cannot change idxd implementation after devices are initialized\n");
		assert(false);
		return;
	}
	g_idxd_impl = tmp;

	if (g_idxd_impl == NULL) {
		SPDK_ERRLOG("Cannot set the idxd implementation with %s mode\n",
			    kernel_mode ? KERNEL_DRIVER_NAME : USERSPACE_DRIVER_NAME);
		return;
	}
}

static void
idxd_device_destruct(struct spdk_idxd_device *idxd)
{
	assert(idxd->impl != NULL);

	idxd->impl->destruct(idxd);
}

int
spdk_idxd_probe(void *cb_ctx, spdk_idxd_attach_cb attach_cb,
		spdk_idxd_probe_cb probe_cb)
{
	if (g_idxd_impl == NULL) {
		SPDK_ERRLOG("No idxd impl is selected\n");
		return -1;
	}

	return g_idxd_impl->probe(cb_ctx, attach_cb, probe_cb);
}

void
spdk_idxd_detach(struct spdk_idxd_device *idxd)
{
	assert(idxd != NULL);
	idxd_device_destruct(idxd);
}

static int
_idxd_prep_command(struct spdk_idxd_io_channel *chan, spdk_idxd_req_cb cb_fn, void *cb_arg,
		   int flags, struct idxd_hw_desc **_desc, struct idxd_ops **_op)
{
	struct idxd_hw_desc *desc;
	struct idxd_ops *op;
	uint64_t comp_addr;

	if (!STAILQ_EMPTY(&chan->ops_pool)) {
		op = *_op = STAILQ_FIRST(&chan->ops_pool);
		desc = *_desc = op->desc;
		comp_addr = desc->completion_addr;
		memset(desc, 0, sizeof(*desc));
		desc->completion_addr = comp_addr;
		STAILQ_REMOVE_HEAD(&chan->ops_pool, link);
	} else {
		/* The application needs to handle this, violation of flow control */
		return -EBUSY;
	}

	flags |= IDXD_FLAG_COMPLETION_ADDR_VALID;
	flags |= IDXD_FLAG_REQUEST_COMPLETION;

	desc->flags = flags;
	op->cb_arg = cb_arg;
	op->cb_fn = cb_fn;
	op->batch = NULL;
	op->parent = NULL;
	op->count = 1;

	return 0;
}

static int
_idxd_prep_batch_cmd(struct spdk_idxd_io_channel *chan, spdk_idxd_req_cb cb_fn,
		     void *cb_arg, int flags,
		     struct idxd_hw_desc **_desc, struct idxd_ops **_op)
{
	struct idxd_hw_desc *desc;
	struct idxd_ops *op;
	uint64_t comp_addr;
	struct idxd_batch *batch;

	batch = chan->batch;

	assert(batch != NULL);
	if (batch->index == DESC_PER_BATCH) {
		return -EBUSY;
	}

	desc = *_desc = &batch->user_desc[batch->index];
	op = *_op = &batch->user_ops[batch->index];

	op->desc = desc;
	SPDK_DEBUGLOG(idxd, "Prep batch %p index %u\n", batch, batch->index);

	batch->index++;

	comp_addr = desc->completion_addr;
	memset(desc, 0, sizeof(*desc));
	desc->completion_addr = comp_addr;
	flags |= IDXD_FLAG_COMPLETION_ADDR_VALID;
	flags |= IDXD_FLAG_REQUEST_COMPLETION;
	desc->flags = flags;
	op->cb_arg = cb_arg;
	op->cb_fn = cb_fn;
	op->batch = batch;
	op->parent = NULL;
	op->count = 1;
	op->crc_dst = NULL;

	return 0;
}

static struct idxd_batch *
idxd_batch_create(struct spdk_idxd_io_channel *chan)
{
	struct idxd_batch *batch;

	assert(chan != NULL);
	assert(chan->batch == NULL);

	if (!TAILQ_EMPTY(&chan->batch_pool)) {
		batch = TAILQ_FIRST(&chan->batch_pool);
		batch->index = 0;
		batch->chan = chan;
		chan->batch = batch;
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
	assert(batch->refcnt == 0);
	batch->index = 0;
	batch->chan = NULL;
	TAILQ_INSERT_TAIL(&chan->batch_pool, batch, link);
}

static int
idxd_batch_cancel(struct spdk_idxd_io_channel *chan, int status)
{
	struct idxd_ops *op;
	struct idxd_batch *batch;
	int i;

	assert(chan != NULL);

	batch = chan->batch;
	assert(batch != NULL);

	if (batch->index == UINT8_MAX) {
		SPDK_ERRLOG("Cannot cancel batch, already submitted to HW.\n");
		return -EINVAL;
	}

	chan->batch = NULL;

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
idxd_batch_submit(struct spdk_idxd_io_channel *chan,
		  spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_batch *batch;
	struct idxd_ops *op;
	int i, rc, flags = 0;

	assert(chan != NULL);

	batch = chan->batch;
	assert(batch != NULL);

	if (batch->index == 0) {
		return idxd_batch_cancel(chan, 0);
	}

	/* Common prep. */
	rc = _idxd_prep_command(chan, cb_fn, cb_arg, flags, &desc, &op);
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
		_free_batch(batch, chan);
	} else {
		/* Command specific. */
		desc->opcode = IDXD_OPCODE_BATCH;
		desc->desc_list_addr = batch->user_desc_addr;
		desc->desc_count = batch->index;
		assert(batch->index <= DESC_PER_BATCH);

		/* Add the batch elements completion contexts to the outstanding list to be polled. */
		for (i = 0 ; i < batch->index; i++) {
			batch->refcnt++;
			STAILQ_INSERT_TAIL(&chan->ops_outstanding, (struct idxd_ops *)&batch->user_ops[i],
					   link);
		}
		batch->index = UINT8_MAX;
	}

	chan->batch = NULL;

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
		batch = idxd_batch_create(chan);
		if (batch == NULL) {
			return -EBUSY;
		}
	}

	return 0;
}

static int
_idxd_flush_batch(struct spdk_idxd_io_channel *chan)
{
	int rc;

	if (chan->batch != NULL && chan->batch->index >= DESC_PER_BATCH) {
		/* Close out the full batch */
		rc = idxd_batch_submit(chan, NULL, NULL);
		if (rc) {
			assert(rc == -EBUSY);
			/*
			 * Return 0. This will get re-submitted within idxd_process_events where
			 * if it fails, it will get correctly aborted.
			 */
			return 0;
		}
	}

	return 0;
}

static inline void
_update_write_flags(struct spdk_idxd_io_channel *chan, struct idxd_hw_desc *desc)
{
	if (desc->flags & SPDK_IDXD_FLAG_PERSISTENT) {
		/* recent spec changes require a different set of flags for PMEM writes */
		desc->flags &= ~IDXD_FLAG_DEST_STEERING_TAG;
		desc->flags &= ~IDXD_FLAG_CACHE_CONTROL;
		desc->flags |= IDXD_FLAG_DEST_READBACK;
	} else {
		desc->flags ^= IDXD_FLAG_CACHE_CONTROL;
	}
}

int
spdk_idxd_submit_copy(struct spdk_idxd_io_channel *chan,
		      struct iovec *diov, uint32_t diovcnt,
		      struct iovec *siov, uint32_t siovcnt,
		      int flags, spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_ops *first_op, *op;
	void *src, *dst;
	uint64_t src_addr, dst_addr;
	int rc, count;
	uint64_t len, seg_len;
	struct spdk_ioviter iter;
	struct idxd_vtophys_iter vtophys_iter;

	assert(chan != NULL);
	assert(diov != NULL);
	assert(siov != NULL);

	rc = _idxd_setup_batch(chan);
	if (rc) {
		return rc;
	}

	count = 0;
	first_op = NULL;
	for (len = spdk_ioviter_first(&iter, siov, siovcnt, diov, diovcnt, &src, &dst);
	     len > 0;
	     len = spdk_ioviter_next(&iter, &src, &dst)) {

		idxd_vtophys_iter_init(chan, &vtophys_iter, src, dst, len);

		while (len > 0) {
			if (first_op == NULL) {
				rc = _idxd_prep_batch_cmd(chan, cb_fn, cb_arg, flags, &desc, &op);
				if (rc) {
					goto error;
				}

				first_op = op;
			} else {
				rc = _idxd_prep_batch_cmd(chan, NULL, NULL, flags, &desc, &op);
				if (rc) {
					goto error;
				}

				first_op->count++;
				op->parent = first_op;
			}

			count++;

			src_addr = 0;
			dst_addr = 0;
			seg_len = idxd_vtophys_iter_next(&vtophys_iter, &src_addr, &dst_addr);
			if (seg_len == SPDK_VTOPHYS_ERROR) {
				rc = -EFAULT;
				goto error;
			}

			desc->opcode = IDXD_OPCODE_MEMMOVE;
			desc->src_addr = src_addr;
			desc->dst_addr = dst_addr;
			desc->xfer_size = seg_len;
			_update_write_flags(chan, desc);

			len -= seg_len;
		}
	}

	return _idxd_flush_batch(chan);

error:
	chan->batch->index -= count;
	return rc;
}

/* Dual-cast copies the same source to two separate destination buffers. */
int
spdk_idxd_submit_dualcast(struct spdk_idxd_io_channel *chan, void *dst1, void *dst2,
			  const void *src, uint64_t nbytes, int flags,
			  spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_ops *first_op, *op;
	uint64_t src_addr, dst1_addr, dst2_addr;
	int rc, count;
	uint64_t len;
	uint64_t outer_seg_len, inner_seg_len;
	struct idxd_vtophys_iter iter_outer, iter_inner;

	assert(chan != NULL);
	assert(dst1 != NULL);
	assert(dst2 != NULL);
	assert(src != NULL);

	if ((uintptr_t)dst1 & (ALIGN_4K - 1) || (uintptr_t)dst2 & (ALIGN_4K - 1)) {
		SPDK_ERRLOG("Dualcast requires 4K alignment on dst addresses\n");
		return -EINVAL;
	}

	rc = _idxd_setup_batch(chan);
	if (rc) {
		return rc;
	}

	idxd_vtophys_iter_init(chan, &iter_outer, src, dst1, nbytes);

	first_op = NULL;
	count = 0;
	while (nbytes > 0) {
		src_addr = 0;
		dst1_addr = 0;
		outer_seg_len = idxd_vtophys_iter_next(&iter_outer, &src_addr, &dst1_addr);
		if (outer_seg_len == SPDK_VTOPHYS_ERROR) {
			goto error;
		}

		idxd_vtophys_iter_init(chan, &iter_inner, src, dst2, nbytes);

		src += outer_seg_len;
		nbytes -= outer_seg_len;

		while (outer_seg_len > 0) {
			if (first_op == NULL) {
				rc = _idxd_prep_batch_cmd(chan, cb_fn, cb_arg, flags, &desc, &op);
				if (rc) {
					goto error;
				}

				first_op = op;
			} else {
				rc = _idxd_prep_batch_cmd(chan, NULL, NULL, flags, &desc, &op);
				if (rc) {
					goto error;
				}

				first_op->count++;
				op->parent = first_op;
			}

			count++;

			src_addr = 0;
			dst2_addr = 0;
			inner_seg_len = idxd_vtophys_iter_next(&iter_inner, &src_addr, &dst2_addr);
			if (inner_seg_len == SPDK_VTOPHYS_ERROR) {
				rc = -EFAULT;
				goto error;
			}

			len = spdk_min(outer_seg_len, inner_seg_len);

			/* Command specific. */
			desc->opcode = IDXD_OPCODE_DUALCAST;
			desc->src_addr = src_addr;
			desc->dst_addr = dst1_addr;
			desc->dest2 = dst2_addr;
			desc->xfer_size = len;
			_update_write_flags(chan, desc);

			dst1_addr += len;
			outer_seg_len -= len;
		}
	}

	return _idxd_flush_batch(chan);

error:
	chan->batch->index -= count;
	return rc;
}

int
spdk_idxd_submit_compare(struct spdk_idxd_io_channel *chan,
			 struct iovec *siov1, size_t siov1cnt,
			 struct iovec *siov2, size_t siov2cnt,
			 int flags, spdk_idxd_req_cb cb_fn, void *cb_arg)
{

	struct idxd_hw_desc *desc;
	struct idxd_ops *first_op, *op;
	void *src1, *src2;
	uint64_t src1_addr, src2_addr;
	int rc, count;
	uint64_t len, seg_len;
	struct spdk_ioviter iter;
	struct idxd_vtophys_iter vtophys_iter;

	assert(chan != NULL);
	assert(siov1 != NULL);
	assert(siov2 != NULL);

	rc = _idxd_setup_batch(chan);
	if (rc) {
		return rc;
	}

	count = 0;
	first_op = NULL;
	for (len = spdk_ioviter_first(&iter, siov1, siov1cnt, siov2, siov2cnt, &src1, &src2);
	     len > 0;
	     len = spdk_ioviter_next(&iter, &src1, &src2)) {

		idxd_vtophys_iter_init(chan, &vtophys_iter, src1, src2, len);

		while (len > 0) {
			if (first_op == NULL) {
				rc = _idxd_prep_batch_cmd(chan, cb_fn, cb_arg, flags, &desc, &op);
				if (rc) {
					goto error;
				}

				first_op = op;
			} else {
				rc = _idxd_prep_batch_cmd(chan, NULL, NULL, flags, &desc, &op);
				if (rc) {
					goto error;
				}

				first_op->count++;
				op->parent = first_op;
			}

			count++;

			src1_addr = 0;
			src2_addr = 0;
			seg_len = idxd_vtophys_iter_next(&vtophys_iter, &src1_addr, &src2_addr);
			if (seg_len == SPDK_VTOPHYS_ERROR) {
				rc = -EFAULT;
				goto error;
			}

			desc->opcode = IDXD_OPCODE_COMPARE;
			desc->src_addr = src1_addr;
			desc->src2_addr = src2_addr;
			desc->xfer_size = seg_len;

			len -= seg_len;
		}
	}

	return _idxd_flush_batch(chan);

error:
	chan->batch->index -= count;
	return rc;
}

int
spdk_idxd_submit_fill(struct spdk_idxd_io_channel *chan,
		      struct iovec *diov, size_t diovcnt,
		      uint64_t fill_pattern, int flags,
		      spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_ops *first_op, *op;
	uint64_t dst_addr;
	int rc, count;
	uint64_t len, seg_len;
	void *dst;
	size_t i;

	assert(chan != NULL);
	assert(diov != NULL);

	rc = _idxd_setup_batch(chan);
	if (rc) {
		return rc;
	}

	count = 0;
	first_op = NULL;
	for (i = 0; i < diovcnt; i++) {
		len = diov[i].iov_len;
		dst = diov[i].iov_base;

		while (len > 0) {
			if (first_op == NULL) {
				rc = _idxd_prep_batch_cmd(chan, cb_fn, cb_arg, flags, &desc, &op);
				if (rc) {
					goto error;
				}

				first_op = op;
			} else {
				rc = _idxd_prep_batch_cmd(chan, NULL, NULL, flags, &desc, &op);
				if (rc) {
					goto error;
				}

				first_op->count++;
				op->parent = first_op;
			}

			count++;

			seg_len = len;
			if (chan->pasid_enabled) {
				dst_addr = (uint64_t)dst;
			} else {
				dst_addr = spdk_vtophys(dst, &seg_len);
				if (dst_addr == SPDK_VTOPHYS_ERROR) {
					SPDK_ERRLOG("Error translating address\n");
					rc = -EFAULT;
					goto error;
				}
			}

			seg_len = spdk_min(seg_len, len);

			desc->opcode = IDXD_OPCODE_MEMFILL;
			desc->pattern = fill_pattern;
			desc->dst_addr = dst_addr;
			desc->xfer_size = seg_len;
			_update_write_flags(chan, desc);

			len -= seg_len;
			dst += seg_len;
		}
	}

	return _idxd_flush_batch(chan);

error:
	chan->batch->index -= count;
	return rc;
}

int
spdk_idxd_submit_crc32c(struct spdk_idxd_io_channel *chan,
			struct iovec *siov, size_t siovcnt,
			uint32_t seed, uint32_t *crc_dst, int flags,
			spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_ops *first_op, *op;
	uint64_t src_addr;
	int rc, count;
	uint64_t len, seg_len;
	void *src;
	size_t i;
	uint64_t prev_crc = 0;

	assert(chan != NULL);
	assert(siov != NULL);

	rc = _idxd_setup_batch(chan);
	if (rc) {
		return rc;
	}

	count = 0;
	op = NULL;
	first_op = NULL;
	for (i = 0; i < siovcnt; i++) {
		len = siov[i].iov_len;
		src = siov[i].iov_base;

		while (len > 0) {
			if (first_op == NULL) {
				rc = _idxd_prep_batch_cmd(chan, cb_fn, cb_arg, flags, &desc, &op);
				if (rc) {
					goto error;
				}

				first_op = op;
			} else {
				rc = _idxd_prep_batch_cmd(chan, NULL, NULL, flags, &desc, &op);
				if (rc) {
					goto error;
				}

				first_op->count++;
				op->parent = first_op;
			}

			count++;

			seg_len = len;
			if (chan->pasid_enabled) {
				src_addr = (uint64_t)src;
			} else {
				src_addr = spdk_vtophys(src, &seg_len);
				if (src_addr == SPDK_VTOPHYS_ERROR) {
					SPDK_ERRLOG("Error translating address\n");
					rc = -EFAULT;
					goto error;
				}
			}

			seg_len = spdk_min(seg_len, len);

			desc->opcode = IDXD_OPCODE_CRC32C_GEN;
			desc->src_addr = src_addr;
			if (op == first_op) {
				desc->crc32c.seed = seed;
			} else {
				desc->flags |= IDXD_FLAG_FENCE | IDXD_FLAG_CRC_READ_CRC_SEED;
				desc->crc32c.addr = prev_crc;
			}

			desc->xfer_size = seg_len;
			prev_crc = desc->completion_addr + offsetof(struct dsa_hw_comp_record, crc32c_val);

			len -= seg_len;
			src += seg_len;
		}
	}

	/* Only the last op copies the crc to the destination */
	if (op) {
		op->crc_dst = crc_dst;
	}

	return _idxd_flush_batch(chan);

error:
	chan->batch->index -= count;
	return rc;
}

int
spdk_idxd_submit_copy_crc32c(struct spdk_idxd_io_channel *chan,
			     struct iovec *diov, size_t diovcnt,
			     struct iovec *siov, size_t siovcnt,
			     uint32_t seed, uint32_t *crc_dst, int flags,
			     spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_ops *first_op, *op;
	void *src, *dst;
	uint64_t src_addr, dst_addr;
	int rc, count;
	uint64_t len, seg_len;
	struct spdk_ioviter iter;
	struct idxd_vtophys_iter vtophys_iter;
	uint64_t prev_crc = 0;

	assert(chan != NULL);
	assert(diov != NULL);
	assert(siov != NULL);

	rc = _idxd_setup_batch(chan);
	if (rc) {
		return rc;
	}

	count = 0;
	op = NULL;
	first_op = NULL;
	for (len = spdk_ioviter_first(&iter, siov, siovcnt, diov, diovcnt, &src, &dst);
	     len > 0;
	     len = spdk_ioviter_next(&iter, &src, &dst)) {


		idxd_vtophys_iter_init(chan, &vtophys_iter, src, dst, len);

		while (len > 0) {
			if (first_op == NULL) {
				rc = _idxd_prep_batch_cmd(chan, cb_fn, cb_arg, flags, &desc, &op);
				if (rc) {
					goto error;
				}

				first_op = op;
			} else {
				rc = _idxd_prep_batch_cmd(chan, NULL, NULL, flags, &desc, &op);
				if (rc) {
					goto error;
				}

				first_op->count++;
				op->parent = first_op;
			}

			count++;

			src_addr = 0;
			dst_addr = 0;
			seg_len = idxd_vtophys_iter_next(&vtophys_iter, &src_addr, &dst_addr);
			if (seg_len == SPDK_VTOPHYS_ERROR) {
				rc = -EFAULT;
				goto error;
			}

			desc->opcode = IDXD_OPCODE_COPY_CRC;
			desc->dst_addr = dst_addr;
			desc->src_addr = src_addr;
			_update_write_flags(chan, desc);
			if (op == first_op) {
				desc->crc32c.seed = seed;
			} else {
				desc->flags |= IDXD_FLAG_FENCE | IDXD_FLAG_CRC_READ_CRC_SEED;
				desc->crc32c.addr = prev_crc;
			}

			desc->xfer_size = seg_len;
			prev_crc = desc->completion_addr + offsetof(struct dsa_hw_comp_record, crc32c_val);

			len -= seg_len;
		}
	}

	/* Only the last op copies the crc to the destination */
	if (op) {
		op->crc_dst = crc_dst;
	}

	return _idxd_flush_batch(chan);

error:
	chan->batch->index -= count;
	return rc;
}

static inline int
_idxd_submit_compress_single(struct spdk_idxd_io_channel *chan, void *dst, const void *src,
			     uint64_t nbytes_dst, uint64_t nbytes_src, uint32_t *output_size,
			     int flags, spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_ops *op;
	uint64_t src_addr, dst_addr;
	int rc;

	/* Common prep. */
	rc = _idxd_prep_command(chan, cb_fn, cb_arg, flags, &desc, &op);
	if (rc) {
		return rc;
	}

	rc = _vtophys(chan, src, &src_addr, nbytes_src);
	if (rc) {
		goto error;
	}

	rc = _vtophys(chan, dst, &dst_addr, nbytes_dst);
	if (rc) {
		goto error;
	}

	/* Command specific. */
	desc->opcode = IDXD_OPCODE_COMPRESS;
	desc->src1_addr = src_addr;
	desc->dst_addr = dst_addr;
	desc->src1_size = nbytes_src;
	desc->iaa.max_dst_size = nbytes_dst;
	desc->iaa.src2_size = sizeof(struct iaa_aecs);
	desc->iaa.src2_addr = chan->idxd->aecs_addr;
	desc->flags |= IAA_FLAG_RD_SRC2_AECS;
	desc->compr_flags = IAA_COMP_FLAGS;
	op->output_size = output_size;

	_submit_to_hw(chan, op);
	return 0;
error:
	STAILQ_INSERT_TAIL(&chan->ops_pool, op, link);
	return rc;
}

int
spdk_idxd_submit_compress(struct spdk_idxd_io_channel *chan,
			  void *dst, uint64_t nbytes,
			  struct iovec *siov, uint32_t siovcnt, uint32_t *output_size,
			  int flags, spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	assert(chan != NULL);
	assert(dst != NULL);
	assert(siov != NULL);

	if (siovcnt == 1) {
		/* Simple case - copying one buffer to another */
		if (nbytes < siov[0].iov_len) {
			return -EINVAL;
		}

		return _idxd_submit_compress_single(chan, dst, siov[0].iov_base,
						    nbytes, siov[0].iov_len,
						    output_size, flags, cb_fn, cb_arg);
	}
	/* TODO: vectored support */
	return -EINVAL;
}

static inline int
_idxd_submit_decompress_single(struct spdk_idxd_io_channel *chan, void *dst, const void *src,
			       uint64_t nbytes_dst, uint64_t nbytes, int flags, spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_ops *op;
	uint64_t src_addr, dst_addr;
	int rc;

	/* Common prep. */
	rc = _idxd_prep_command(chan, cb_fn, cb_arg, flags, &desc, &op);
	if (rc) {
		return rc;
	}

	rc = _vtophys(chan, src, &src_addr, nbytes);
	if (rc) {
		goto error;
	}

	rc = _vtophys(chan, dst, &dst_addr, nbytes_dst);
	if (rc) {
		goto error;
	}

	/* Command specific. */
	desc->opcode = IDXD_OPCODE_DECOMPRESS;
	desc->src1_addr = src_addr;
	desc->dst_addr = dst_addr;
	desc->src1_size = nbytes;
	desc->iaa.max_dst_size = nbytes_dst;
	desc->decompr_flags = IAA_DECOMP_FLAGS;

	_submit_to_hw(chan, op);
	return 0;
error:
	STAILQ_INSERT_TAIL(&chan->ops_pool, op, link);
	return rc;
}

int
spdk_idxd_submit_decompress(struct spdk_idxd_io_channel *chan,
			    struct iovec *diov, uint32_t diovcnt,
			    struct iovec *siov, uint32_t siovcnt,
			    int flags, spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	assert(chan != NULL);
	assert(diov != NULL);
	assert(siov != NULL);

	if (diovcnt == 1 && siovcnt == 1) {
		/* Simple case - copying one buffer to another */
		if (diov[0].iov_len < siov[0].iov_len) {
			return -EINVAL;
		}

		return _idxd_submit_decompress_single(chan, diov[0].iov_base, siov[0].iov_base,
						      diov[0].iov_len, siov[0].iov_len,
						      flags, cb_fn, cb_arg);
	}
	/* TODO: vectored support */
	return -EINVAL;
}

int
spdk_idxd_submit_raw_desc(struct spdk_idxd_io_channel *chan,
			  struct idxd_hw_desc *_desc,
			  spdk_idxd_req_cb cb_fn, void *cb_arg)
{
	struct idxd_hw_desc *desc;
	struct idxd_ops *op;
	int rc, flags = 0;
	uint64_t comp_addr;

	assert(chan != NULL);
	assert(_desc != NULL);

	/* Common prep. */
	rc = _idxd_prep_command(chan, cb_fn, cb_arg, flags, &desc, &op);
	if (rc) {
		return rc;
	}

	/* Command specific. */
	flags = desc->flags;
	comp_addr = desc->completion_addr;
	memcpy(desc, _desc, sizeof(*desc));
	desc->flags |= flags;
	desc->completion_addr = comp_addr;

	/* Submit operation. */
	_submit_to_hw(chan, op);

	return 0;
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
	struct idxd_ops *op, *tmp, *parent_op;
	int status = 0;
	int rc2, rc = 0;
	void *cb_arg;
	spdk_idxd_req_cb cb_fn;

	assert(chan != NULL);

	STAILQ_FOREACH_SAFE(op, &chan->ops_outstanding, link, tmp) {
		if (!IDXD_COMPLETION(op->hw.status)) {
			/*
			 * oldest locations are at the head of the list so if
			 * we've polled a location that hasn't completed, bail
			 * now as there are unlikely to be any more completions.
			 */
			break;
		}

		STAILQ_REMOVE_HEAD(&chan->ops_outstanding, link);
		rc++;

		/* Status is in the same location for both IAA and DSA completion records. */
		if (spdk_unlikely(IDXD_FAILURE(op->hw.status))) {
			SPDK_ERRLOG("Completion status 0x%x\n", op->hw.status);
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
		case IDXD_OPCODE_COMPRESS:
			if (spdk_likely(status == 0 && op->output_size != NULL)) {
				*op->output_size = op->iaa_hw.output_size;
			}
			break;
		}

		/* TODO: WHAT IF THIS FAILED!? */
		op->hw.status = 0;

		assert(op->count > 0);
		op->count--;

		parent_op = op->parent;
		if (parent_op != NULL) {
			assert(parent_op->count > 0);
			parent_op->count--;

			if (parent_op->count == 0) {
				cb_fn = parent_op->cb_fn;
				cb_arg = parent_op->cb_arg;

				assert(parent_op->batch != NULL);

				/*
				 * Now that parent_op count is 0, we can release its ref
				 * to its batch. We have not released the ref to the batch
				 * that the op is pointing to yet, which will be done below.
				 */
				parent_op->batch->refcnt--;
				if (parent_op->batch->refcnt == 0) {
					_free_batch(parent_op->batch, chan);
				}

				if (cb_fn) {
					cb_fn(cb_arg, status);
				}
			}
		}

		if (op->count == 0) {
			cb_fn = op->cb_fn;
			cb_arg = op->cb_arg;

			if (op->batch != NULL) {
				assert(op->batch->refcnt > 0);
				op->batch->refcnt--;

				if (op->batch->refcnt == 0) {
					_free_batch(op->batch, chan);
				}
			} else {
				STAILQ_INSERT_HEAD(&chan->ops_pool, op, link);
			}

			if (cb_fn) {
				cb_fn(cb_arg, status);
			}
		}

		/* reset the status */
		status = 0;
		/* break the processing loop to prevent from starving the rest of the system */
		if (rc > IDXD_MAX_COMPLETIONS) {
			break;
		}
	}

	/* Submit any built-up batch */
	if (chan->batch) {
		rc2 = idxd_batch_submit(chan, NULL, NULL);
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
