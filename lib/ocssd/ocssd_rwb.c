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

#include <spdk/stdinc.h>
#include <spdk/env.h>
#include <stdatomic.h>
#include <rte_common.h>
#include "ocssd_rwb.h"
#include "ocssd_core.h"

struct ocssd_rwb_batch {
	/* Parent RWB */
	struct ocssd_rwb			*rwb;

	/* Position within RWB */
	unsigned int				pos;

	/* Number of acquired entries */
	atomic_uint				num_acquired;

	/* Number of entries ready for submission */
	atomic_uint				num_ready;

	/* RWB entry list */
	LIST_HEAD(, ocssd_rwb_entry)		entry_list;

	/* Entry buffer */
	struct ocssd_rwb_entry			*entries;

	/* Data buffer */
	void					*buffer;

	/* Metadata buffer */
	void					*md_buffer;

	/* Queue entry */
	STAILQ_ENTRY(ocssd_rwb_batch)		stailq;
};

struct ocssd_rwb {
	/* Number of batches */
	size_t					num_batches;

	/* Number of entries per batch */
	size_t					xfer_size;

	/* Metadata's size */
	size_t					md_size;

	/* Number of acquired entries */
	atomic_uint				num_acquired[OCSSD_RWB_TYPE_MAX];

	/* User/internal limits */
	size_t					limits[OCSSD_RWB_TYPE_MAX];

	/* Current batch */
	struct ocssd_rwb_batch			*current;

	/* Free batch queue */
	STAILQ_HEAD(, ocssd_rwb_batch)		free_queue;

	/* Submission batch queue */
	struct spdk_ring			*submit_queue;

	/* Batch buffer */
	struct ocssd_rwb_batch			*batches;

	/* RWB lock */
	pthread_spinlock_t			lock;
};

static int
ocssd_rwb_batch_full(const struct ocssd_rwb_batch *batch, size_t batch_size)
{
	struct ocssd_rwb *rwb = batch->rwb;
	assert(batch_size <= rwb->xfer_size);
	return batch_size == rwb->xfer_size;
}

static void
ocssd_rwb_batch_init_entry(struct ocssd_rwb_batch *batch, size_t pos)
{
	struct ocssd_rwb *rwb = batch->rwb;
	struct ocssd_rwb_entry *entry, *prev;
	size_t batch_offset = pos % rwb->xfer_size;

	entry = &batch->entries[batch_offset];
	entry->pos = pos;
	entry->data = ((char *)batch->buffer) + OCSSD_BLOCK_SIZE * batch_offset;
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
ocssd_rwb_batch_init(struct ocssd_rwb *rwb, struct ocssd_rwb_batch *batch, unsigned int pos)
{
	size_t md_size = ocssd_div_up(rwb->md_size * rwb->xfer_size, OCSSD_BLOCK_SIZE) *
			 OCSSD_BLOCK_SIZE;

	batch->rwb = rwb;
	batch->pos = pos;

	batch->entries = calloc(rwb->xfer_size, sizeof(*batch->entries));
	if (!batch->entries) {
		return -1;
	}

	batch->buffer = spdk_dma_zmalloc(OCSSD_BLOCK_SIZE * rwb->xfer_size,
					 OCSSD_BLOCK_SIZE, NULL);
	if (!batch->buffer) {
		goto error;
	}

	if (md_size > 0) {
		batch->md_buffer = spdk_dma_zmalloc(md_size, OCSSD_BLOCK_SIZE, NULL);
		if (!batch->md_buffer) {
			goto error;
		}
	}

	LIST_INIT(&batch->entry_list);

	for (unsigned int i = 0; i < rwb->xfer_size; ++i) {
		ocssd_rwb_batch_init_entry(batch, pos * rwb->xfer_size + i);
	}

	return 0;
error:
	free(batch->entries);
	spdk_dma_free(batch->buffer);
	return -1;
}

struct ocssd_rwb *
ocssd_rwb_init(const struct ocssd_conf *conf, size_t xfer_size, size_t md_size)
{
	struct ocssd_rwb *rwb;
	struct ocssd_rwb_batch *batch;
	size_t ring_size;

	rwb = calloc(1, sizeof(*rwb));
	if (!rwb) {
		goto error;
	}

	assert(conf->rwb_size % xfer_size == 0);

	rwb->xfer_size = xfer_size;
	rwb->md_size = md_size;
	rwb->num_batches = conf->rwb_size / (OCSSD_BLOCK_SIZE * xfer_size);

	ring_size = rte_align32pow2(rwb->num_batches);

	rwb->batches = calloc(rwb->num_batches, sizeof(*rwb->batches));
	if (!rwb->batches) {
		goto error;
	}

	rwb->submit_queue = spdk_ring_create(SPDK_RING_TYPE_MP_SC, ring_size,
					     SPDK_ENV_SOCKET_ID_ANY);
	if (!rwb->submit_queue) {
		SPDK_ERRLOG("Failed to create submission queue\n");
		goto error;
	}

	/* TODO: use rte_ring with SP / MC */
	STAILQ_INIT(&rwb->free_queue);

	for (unsigned int i = 0; i < rwb->num_batches; ++i) {
		batch = &rwb->batches[i];

		if (ocssd_rwb_batch_init(rwb, batch, i)) {
			SPDK_ERRLOG("Failed to initialize RWB entry buffer\n");
			goto error;
		}

		STAILQ_INSERT_TAIL(&rwb->free_queue, batch, stailq);
	}

	for (unsigned int i = 0; i < OCSSD_RWB_TYPE_MAX; ++i) {
		rwb->limits[i] = ocssd_rwb_entry_cnt(rwb);
	}

	pthread_spin_init(&rwb->lock, PTHREAD_PROCESS_PRIVATE);
	return rwb;
error:
	ocssd_rwb_free(rwb);
	return NULL;
}

void
ocssd_rwb_free(struct ocssd_rwb *rwb)
{
	struct ocssd_rwb_entry *entry;
	struct ocssd_rwb_batch *batch;

	if (!rwb) {
		return;
	}

	for (size_t i = 0; i < rwb->num_batches; ++i) {
		batch = &rwb->batches[i];

		ocssd_rwb_foreach(entry, batch) {
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
ocssd_rwb_batch_release(struct ocssd_rwb_batch *batch)
{
	struct ocssd_rwb *rwb = batch->rwb;
	struct ocssd_rwb_entry *entry;
	unsigned int num_acquired __attribute__((unused));

	batch->num_ready = 0;
	batch->num_acquired = 0;

	ocssd_rwb_foreach(entry, batch) {
		num_acquired = atomic_fetch_sub(&rwb->num_acquired[ocssd_rwb_entry_type(entry)], 1);
		assert(num_acquired  > 0);
	}

	pthread_spin_lock(&rwb->lock);
	STAILQ_INSERT_TAIL(&rwb->free_queue, batch, stailq);
	pthread_spin_unlock(&rwb->lock);
}

size_t
ocssd_rwb_entry_cnt(const struct ocssd_rwb *rwb)
{
	return rwb->num_batches * rwb->xfer_size;
}

size_t
ocssd_rwb_num_batches(const struct ocssd_rwb *rwb)
{
	return rwb->num_batches;
}

size_t
ocssd_rwb_batch_get_offset(const struct ocssd_rwb_batch *batch)
{
	return batch->pos;
}

void
ocssd_rwb_set_limits(struct ocssd_rwb *rwb,
		     const size_t limit[OCSSD_RWB_TYPE_MAX])
{
	assert(limit[OCSSD_RWB_TYPE_USER] <= ocssd_rwb_entry_cnt(rwb));
	assert(limit[OCSSD_RWB_TYPE_INTERNAL] <= ocssd_rwb_entry_cnt(rwb));
	memcpy(rwb->limits, limit, sizeof(rwb->limits));
}

void
ocssd_rwb_get_limits(struct ocssd_rwb *rwb,
		     size_t limit[OCSSD_RWB_TYPE_MAX])
{
	memcpy(limit, rwb->limits, sizeof(rwb->limits));
}

size_t
ocssd_rwb_num_acquired(struct ocssd_rwb *rwb, enum ocssd_rwb_entry_type type)
{
	return atomic_load(&rwb->num_acquired[type]);
}

void
ocssd_rwb_batch_revert(struct ocssd_rwb_batch *batch)
{
	struct ocssd_rwb *rwb = batch->rwb;

	if (spdk_ring_enqueue(rwb->submit_queue, (void **)&batch, 1) != 1) {
		assert(0 && "Should never happen");
	}
}

void
ocssd_rwb_push(struct ocssd_rwb_entry *entry)
{
	struct ocssd_rwb_batch *batch = entry->batch;
	struct ocssd_rwb *rwb = batch->rwb;
	size_t batch_size;

	batch_size = atomic_fetch_add(&batch->num_ready, 1) + 1;

	/* Once all of the entries are put back, push the batch on the */
	/* submission queue */
	if (ocssd_rwb_batch_full(batch, batch_size)) {
		if (spdk_ring_enqueue(rwb->submit_queue, (void **)&batch, 1) != 1) {
			assert(0 && "Should never happen");
		}
	}
}

static int
ocssd_rwb_check_limits(struct ocssd_rwb *rwb, enum ocssd_rwb_entry_type type)
{
	return atomic_load(&rwb->num_acquired[type]) >= rwb->limits[type];
}

struct ocssd_rwb_entry *
ocssd_rwb_acquire(struct ocssd_rwb *rwb, enum ocssd_rwb_entry_type type)
{
	struct ocssd_rwb_entry *entry = NULL;
	struct ocssd_rwb_batch *current;

	if (ocssd_rwb_check_limits(rwb, type)) {
		return NULL;
	}

	pthread_spin_lock(&rwb->lock);

	current = rwb->current;
	if (!current) {
		current = STAILQ_FIRST(&rwb->free_queue);
		if (!current) {
			goto error;
		}

		STAILQ_REMOVE(&rwb->free_queue, current, ocssd_rwb_batch, stailq);
		rwb->current = current;
	}

	entry = &current->entries[current->num_acquired++];

	/* If the whole batch is filled, clear the current batch pointer */
	if (current->num_acquired >= rwb->xfer_size) {
		rwb->current = NULL;
	}

	pthread_spin_unlock(&rwb->lock);
	atomic_fetch_add(&rwb->num_acquired[type], 1);
	return entry;
error:
	pthread_spin_unlock(&rwb->lock);
	return NULL;
}

struct ocssd_rwb_batch *
ocssd_rwb_pop(struct ocssd_rwb *rwb)
{
	struct ocssd_rwb_batch *batch = NULL;

	if (spdk_ring_dequeue(rwb->submit_queue, (void **)&batch, 1) != 1) {
		return NULL;
	}

	return batch;
}

static struct ocssd_rwb_batch *
_ocssd_rwb_next_batch(struct ocssd_rwb *rwb, size_t pos)
{
	if (pos >= rwb->num_batches) {
		return NULL;
	}

	return &rwb->batches[pos];
}

struct ocssd_rwb_batch *
ocssd_rwb_next_batch(struct ocssd_rwb_batch *batch)
{
	return _ocssd_rwb_next_batch(batch->rwb, batch->pos + 1);
}

struct ocssd_rwb_batch *
ocssd_rwb_first_batch(struct ocssd_rwb *rwb)
{
	return _ocssd_rwb_next_batch(rwb, 0);
}

int
ocssd_rwb_batch_empty(struct ocssd_rwb_batch *batch)
{
	return atomic_load(&batch->num_ready) == 0;
}

void *
ocssd_rwb_batch_data(struct ocssd_rwb_batch *batch)
{
	return batch->buffer;
}

void *
ocssd_rwb_batch_md(struct ocssd_rwb_batch *batch)
{
	return batch->md_buffer;
}

struct ocssd_rwb_entry *
ocssd_rwb_entry_from_offset(struct ocssd_rwb *rwb, size_t offset)
{
	unsigned int b_off, e_off;

	b_off = offset / rwb->xfer_size;
	e_off = offset % rwb->xfer_size;

	assert(b_off < rwb->num_batches);

	return &rwb->batches[b_off].entries[e_off];
}

struct ocssd_rwb_entry *
ocssd_rwb_batch_first_entry(struct ocssd_rwb_batch *batch)
{
	return LIST_FIRST(&batch->entry_list);
}
