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

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/util.h"

#include "ftl_rwb.h"
#include "ftl_core.h"

struct ftl_rwb_batch {
	/* Parent RWB */
	struct ftl_rwb				*rwb;

	/* Position within RWB */
	unsigned int				pos;

	/* Number of acquired entries */
	unsigned int				num_acquired;

	/* Number of entries ready for submission */
	unsigned int				num_ready;

	/* RWB entry list */
	LIST_HEAD(, ftl_rwb_entry)		entry_list;

	/* Entry buffer */
	struct ftl_rwb_entry			*entries;

	/* Data buffer */
	void					*buffer;

	/* Metadata buffer */
	void					*md_buffer;

	/* Queue entry */
	STAILQ_ENTRY(ftl_rwb_batch)		stailq;
};

struct ftl_rwb {
	/* Number of batches */
	size_t					num_batches;

	/* Optimal read size : information for a efficient data placement and interleaving */
	size_t                                  rs_opt;
	/* Optimal number of active batches */
	size_t                                  active_batches_opt;
	/* Indicates the rwb is about to be stopped */
	int                                     halt;

	/* Number of entries per batch */
	size_t					xfer_size;

	/* Metadata's size */
	size_t					md_size;

	/* Number of acquired entries */
	unsigned int				num_acquired[FTL_RWB_TYPE_MAX];

	/* User/internal limits */
	size_t					limits[FTL_RWB_TYPE_MAX];

	/* Current batch */
	struct ftl_rwb_batch			*current;

	/* Active batch queue */
	STAILQ_HEAD(, ftl_rwb_batch)            active_queue;
	/* Number of active batches */
	unsigned int                            num_active_batches;

	/* Free batch queue */
	STAILQ_HEAD(, ftl_rwb_batch)		free_queue;
	/* Number of active batches */
	unsigned int                            num_free_batches;

	/* Submission batch queue */
	struct spdk_ring			*submit_queue;

	/* Batch buffer */
	struct ftl_rwb_batch			*batches;

	/* RWB lock */
	pthread_spinlock_t			lock;
};

static int
ftl_rwb_batch_full(const struct ftl_rwb_batch *batch, size_t batch_size)
{
	struct ftl_rwb *rwb = batch->rwb;
	assert(batch_size <= rwb->xfer_size);
	return batch_size == rwb->xfer_size;
}

static void
ftl_rwb_batch_init_entry(struct ftl_rwb_batch *batch, size_t pos)
{
	struct ftl_rwb *rwb = batch->rwb;
	struct ftl_rwb_entry *entry, *prev;
	size_t batch_offset = pos % rwb->xfer_size;

	entry = &batch->entries[batch_offset];
	entry->pos = pos;
	entry->data = ((char *)batch->buffer) + FTL_BLOCK_SIZE * batch_offset;
	entry->md = rwb->md_size ? ((char *)batch->md_buffer) + rwb->md_size * batch_offset : NULL;
	entry->batch = batch;
	entry->rwb = batch->rwb;
	pthread_spin_init(&entry->lock, PTHREAD_PROCESS_PRIVATE);

	if (batch_offset > 0) {
		prev = &batch->entries[batch_offset - 1];
		LIST_INSERT_AFTER(prev, entry, list_entry);
	} else {
		LIST_INSERT_HEAD(&batch->entry_list, entry, list_entry);
	}
}

static int
ftl_rwb_batch_init(struct ftl_rwb *rwb, struct ftl_rwb_batch *batch, unsigned int pos)
{
	size_t md_size, i;

	md_size = spdk_divide_round_up(rwb->md_size * rwb->xfer_size, FTL_BLOCK_SIZE) *
		  FTL_BLOCK_SIZE;
	batch->rwb = rwb;
	batch->pos = pos;

	batch->entries = calloc(rwb->xfer_size, sizeof(*batch->entries));
	if (!batch->entries) {
		return -1;
	}

	batch->buffer = spdk_dma_zmalloc(FTL_BLOCK_SIZE * rwb->xfer_size,
					 FTL_BLOCK_SIZE, NULL);
	if (!batch->buffer) {
		goto error;
	}

	if (md_size > 0) {
		batch->md_buffer = spdk_dma_zmalloc(md_size, FTL_BLOCK_SIZE, NULL);
		if (!batch->md_buffer) {
			goto error;
		}
	}

	LIST_INIT(&batch->entry_list);

	for (i = 0; i < rwb->xfer_size; ++i) {
		ftl_rwb_batch_init_entry(batch, pos * rwb->xfer_size + i);
	}

	return 0;
error:
	free(batch->entries);
	batch->entries = NULL;
	spdk_dma_free(batch->buffer);
	batch->buffer = NULL;
	return -1;
}

struct ftl_rwb *
ftl_rwb_init(const struct spdk_ftl_conf *conf, size_t xfer_size, size_t md_size, size_t num_punits)
{
	struct ftl_rwb *rwb;
	struct ftl_rwb_batch *batch;
	size_t i;

	rwb = calloc(1, sizeof(*rwb));
	if (!rwb) {
		goto error;
	}

	assert(conf->rwb_size % xfer_size == 0);
	assert(xfer_size % conf->osp_num_pages == 0);

	rwb->xfer_size = xfer_size;
	rwb->rs_opt = xfer_size / conf->osp_num_pages;
	rwb->active_batches_opt = (conf->osp_num_pages == 1) ? 1 : num_punits;
	rwb->md_size = md_size;
	rwb->num_batches = conf->rwb_size / (FTL_BLOCK_SIZE * xfer_size) + rwb->active_batches_opt;

	rwb->batches = calloc(rwb->num_batches, sizeof(*rwb->batches));
	if (!rwb->batches) {
		goto error;
	}

	rwb->halt = 0;

	rwb->submit_queue = spdk_ring_create(SPDK_RING_TYPE_MP_SC,
					     spdk_align32pow2(rwb->num_batches + 1),
					     SPDK_ENV_SOCKET_ID_ANY);
	if (!rwb->submit_queue) {
		SPDK_ERRLOG("Failed to create submission queue\n");
		goto error;
	}

	/* TODO: use rte_ring with SP / MC */
	STAILQ_INIT(&rwb->free_queue);
	STAILQ_INIT(&rwb->active_queue);
	rwb->num_free_batches = 0;
	rwb->num_active_batches = 0;

	for (i = 0; i < rwb->num_batches; ++i) {
		batch = &rwb->batches[i];

		if (ftl_rwb_batch_init(rwb, batch, i)) {
			SPDK_ERRLOG("Failed to initialize RWB entry buffer\n");
			goto error;
		}

		STAILQ_INSERT_TAIL(&rwb->free_queue, batch, stailq);
		rwb->num_free_batches++;
	}

	for (unsigned int i = 0; i < FTL_RWB_TYPE_MAX; ++i) {
		rwb->limits[i] = ftl_rwb_entry_cnt(rwb);
	}

	pthread_spin_init(&rwb->lock, PTHREAD_PROCESS_PRIVATE);
	return rwb;
error:
	ftl_rwb_free(rwb);
	return NULL;
}

void
ftl_rwb_free(struct ftl_rwb *rwb)
{
	struct ftl_rwb_entry *entry;
	struct ftl_rwb_batch *batch;

	if (!rwb) {
		return;
	}

	for (size_t i = 0; i < rwb->num_batches; ++i) {
		batch = &rwb->batches[i];

		ftl_rwb_foreach(entry, batch) {
			pthread_spin_destroy(&entry->lock);
		}

		spdk_dma_free(batch->buffer);
		spdk_dma_free(batch->md_buffer);
		free(batch->entries);
	}

	pthread_spin_destroy(&rwb->lock);
	spdk_ring_free(rwb->submit_queue);
	free(rwb->batches);
	free(rwb);
}

void
ftl_rwb_batch_release(struct ftl_rwb_batch *batch)
{
	struct ftl_rwb *rwb = batch->rwb;
	struct ftl_rwb_entry *entry;
	unsigned int num_acquired __attribute__((unused));

	batch->num_ready = 0;
	batch->num_acquired = 0;

	ftl_rwb_foreach(entry, batch) {
		num_acquired = __atomic_fetch_sub(&rwb->num_acquired[ftl_rwb_entry_type(entry)], 1,
						  __ATOMIC_SEQ_CST);
		assert(num_acquired  > 0);
	}

	pthread_spin_lock(&rwb->lock);
	STAILQ_INSERT_TAIL(&rwb->free_queue, batch, stailq);
	rwb->num_free_batches++;
	pthread_spin_unlock(&rwb->lock);
}

size_t
ftl_rwb_entry_cnt(const struct ftl_rwb *rwb)
{
	return rwb->num_batches * rwb->xfer_size;
}

size_t
ftl_rwb_num_batches(const struct ftl_rwb *rwb)
{
	return rwb->num_batches;
}

size_t
ftl_rwb_batch_get_offset(const struct ftl_rwb_batch *batch)
{
	return batch->pos;
}

void
ftl_rwb_set_limits(struct ftl_rwb *rwb,
		   const size_t limit[FTL_RWB_TYPE_MAX])
{
	assert(limit[FTL_RWB_TYPE_USER] <= ftl_rwb_entry_cnt(rwb));
	assert(limit[FTL_RWB_TYPE_INTERNAL] <= ftl_rwb_entry_cnt(rwb));
	memcpy(rwb->limits, limit, sizeof(rwb->limits));
}

void
ftl_rwb_get_limits(struct ftl_rwb *rwb,
		   size_t limit[FTL_RWB_TYPE_MAX])
{
	memcpy(limit, rwb->limits, sizeof(rwb->limits));
}

size_t
ftl_rwb_num_acquired(struct ftl_rwb *rwb, enum ftl_rwb_entry_type type)
{
	return __atomic_load_n(&rwb->num_acquired[type], __ATOMIC_SEQ_CST);
}

size_t
ftl_rwb_get_active_batches_opt(const struct ftl_rwb *rwb)
{
	return rwb->active_batches_opt;
}

size_t
ftl_rwb_get_active_batches(const struct ftl_rwb *rwb)
{
	return (rwb->current) ? (rwb->num_active_batches + 1) : rwb->num_active_batches;
}

void
ftl_rwb_batch_revert(struct ftl_rwb_batch *batch)
{
	struct ftl_rwb *rwb = batch->rwb;

	if (spdk_ring_enqueue(rwb->submit_queue, (void **)&batch, 1) != 1) {
		assert(0 && "Should never happen");
	}
}

void
ftl_rwb_push(struct ftl_rwb_entry *entry)
{
	struct ftl_rwb_batch *batch = entry->batch;
	struct ftl_rwb *rwb = batch->rwb;
	size_t batch_size;

	batch_size = __atomic_fetch_add(&batch->num_ready, 1, __ATOMIC_SEQ_CST) + 1;

	/* Once all of the entries are put back, push the batch on the */
	/* submission queue */
	if (ftl_rwb_batch_full(batch, batch_size)) {
		if (spdk_ring_enqueue(rwb->submit_queue, (void **)&batch, 1) != 1) {
			assert(0 && "Should never happen");
		}
	}
}

static int
ftl_rwb_check_limits(struct ftl_rwb *rwb, enum ftl_rwb_entry_type type)
{
	return ftl_rwb_num_acquired(rwb, type) >= rwb->limits[type];
}

static int
_ftl_rwb_acquire_batch(struct ftl_rwb *rwb)
{
	struct ftl_rwb_batch *current = NULL;

	if (rwb->num_active_batches > 0) {
		return 0;
	}

	if (rwb->num_free_batches < rwb->active_batches_opt) {
		return -1;
	}

	for (size_t i = 0; i < rwb->active_batches_opt; i++) {
		current = STAILQ_FIRST(&rwb->free_queue);
		if (!current) {
			return -1;
		}

		STAILQ_REMOVE(&rwb->free_queue, current, ftl_rwb_batch, stailq);
		rwb->num_free_batches--;

		STAILQ_INSERT_TAIL(&rwb->active_queue, current, stailq);
		rwb->num_active_batches++;
	}

	return 0;
}

struct ftl_rwb_entry *
ftl_rwb_acquire(struct ftl_rwb *rwb, enum ftl_rwb_entry_type type)
{
	struct ftl_rwb_entry *entry = NULL;
	struct ftl_rwb_batch *current;

	if (ftl_rwb_check_limits(rwb, type)) {
		return NULL;
	}

	pthread_spin_lock(&rwb->lock);

	current = rwb->current;
	if (!current) {
		if (_ftl_rwb_acquire_batch(rwb)) {
			goto error;
		}

		current = STAILQ_FIRST(&rwb->active_queue);
		if (!current) {
			goto error;
		}

		assert(rwb->num_active_batches > 0);
		STAILQ_REMOVE(&rwb->active_queue, current, ftl_rwb_batch, stailq);
		rwb->num_active_batches--;

		rwb->current = current;
	}

	entry = &current->entries[current->num_acquired++];

	/* If buffer is full with the unit of rs_opt, clear the current batch pointer */
	/* It places the next rs_opt sized logical blocks into another PU for efficient */
	/* interleaving especially for sequential read in 3D NAND. */
	/* It should fill the whole batch before it switches to another batch */
	/* if rs_opt and ws_opt are the same. */
	/* The rwb is about to be stopped, fill the current whole batch. */
	if (current->num_acquired >= rwb->xfer_size ||
	    (!rwb->halt && current->num_acquired % rwb->rs_opt == 0)) {

		if (current->num_acquired < rwb->xfer_size) {
			STAILQ_INSERT_TAIL(&rwb->active_queue, current, stailq);
			rwb->num_active_batches++;
		}

		rwb->current = NULL;
	}

	pthread_spin_unlock(&rwb->lock);
	__atomic_fetch_add(&rwb->num_acquired[type], 1, __ATOMIC_SEQ_CST);
	return entry;
error:
	pthread_spin_unlock(&rwb->lock);
	return NULL;
}

void
ftl_rwb_process_shutdown(struct ftl_rwb *rwb)
{
	rwb->halt = 1;
	rwb->active_batches_opt = 1;
}

struct ftl_rwb_batch *
ftl_rwb_pop(struct ftl_rwb *rwb)
{
	struct ftl_rwb_batch *batch = NULL;

	if (spdk_ring_dequeue(rwb->submit_queue, (void **)&batch, 1) != 1) {
		return NULL;
	}

	return batch;
}

static struct ftl_rwb_batch *
_ftl_rwb_next_batch(struct ftl_rwb *rwb, size_t pos)
{
	if (pos >= rwb->num_batches) {
		return NULL;
	}

	return &rwb->batches[pos];
}

struct ftl_rwb_batch *
ftl_rwb_next_batch(struct ftl_rwb_batch *batch)
{
	return _ftl_rwb_next_batch(batch->rwb, batch->pos + 1);
}

struct ftl_rwb_batch *
ftl_rwb_first_batch(struct ftl_rwb *rwb)
{
	return _ftl_rwb_next_batch(rwb, 0);
}

int
ftl_rwb_batch_empty(struct ftl_rwb_batch *batch)
{
	return __atomic_load_n(&batch->num_ready, __ATOMIC_SEQ_CST) == 0;
}

void *
ftl_rwb_batch_get_data(struct ftl_rwb_batch *batch)
{
	return batch->buffer;
}

void *
ftl_rwb_batch_get_md(struct ftl_rwb_batch *batch)
{
	return batch->md_buffer;
}

struct ftl_rwb_entry *
ftl_rwb_entry_from_offset(struct ftl_rwb *rwb, size_t offset)
{
	unsigned int b_off, e_off;

	b_off = offset / rwb->xfer_size;
	e_off = offset % rwb->xfer_size;

	assert(b_off < rwb->num_batches);

	return &rwb->batches[b_off].entries[e_off];
}

struct ftl_rwb_entry *
ftl_rwb_batch_first_entry(struct ftl_rwb_batch *batch)
{
	return LIST_FIRST(&batch->entry_list);
}
