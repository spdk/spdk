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

	/* Entry buffer */
	struct ftl_rwb_entry			**entries;

	/* Queue entry */
	STAILQ_ENTRY(ftl_rwb_batch)		stailq;
};

struct ftl_rwb {
	/* Number of entries per batch */
	size_t					xfer_size;
	/* Metadata's size */
	size_t					md_size;
	/* Number of entires */
	size_t					num_entries;

	/* Number of acquired entries */
	unsigned int				num_acquired[FTL_RWB_TYPE_MAX];
	/* User/internal limits */
	size_t					limits[FTL_RWB_TYPE_MAX];

	/* Free batch queue */
	STAILQ_HEAD(, ftl_rwb_batch)		free_batch_queue;
	/* Retried batch queue */
	STAILQ_HEAD(, ftl_rwb_batch)		retry_batch_queue;
	/* Number of batches on the retry_batch_queue */
	unsigned int				retry_batch_count;

	/* Free entry queue */
	struct spdk_ring			*free_queue;
	/* Submission entryh queue */
	struct spdk_ring			*submit_queue;

	/* Entry buffer */
	struct ftl_rwb_entry			*entries;
	/* Batch buffer */
	struct ftl_rwb_batch			*batches;
};

static int
ftl_rwb_init_batch(struct ftl_rwb *rwb, struct ftl_rwb_batch *batch)
{
	batch->rwb = rwb;
	batch->entries = calloc(rwb->xfer_size, sizeof(*batch->entries));
	if (!batch->entries) {
		return -ENOMEM;
	}

	STAILQ_INSERT_TAIL(&rwb->free_batch_queue, batch, stailq);

	return 0;
}

static int
ftl_rwb_init_entry(struct ftl_rwb *rwb, struct ftl_rwb_entry *entry, unsigned int pos)
{
	int rc;

	entry->rwb = rwb;
	entry->pos = pos;

	/* The data should be aligned to a cc.mps, but since currently there's no way of retrieving
	 * this value at the FTL level, use FTL_BLOCK_SIZE instead. */
	entry->data = spdk_dma_zmalloc(FTL_BLOCK_SIZE, FTL_BLOCK_SIZE, NULL);
	if (!entry->data) {
		return -ENOMEM;
	}

	rc = pthread_spin_init(&entry->lock, PTHREAD_PROCESS_PRIVATE);
	if (spdk_unlikely(rc != 0)) {
		spdk_dma_free(entry->data);
		entry->data = NULL;
		return -ENOMEM;
	}

	return 0;
}

struct ftl_rwb *
ftl_rwb_init(const struct spdk_ftl_conf *conf, size_t xfer_size, size_t md_size, size_t num_punits)
{
	struct ftl_rwb *rwb;
	struct ftl_rwb_entry *entry;
	unsigned int i, num_batches;
	int rc;

	rwb = calloc(1, sizeof(*rwb));
	if (!rwb) {
		SPDK_ERRLOG("Memory allocation failure\n");
		return NULL;
	}

	STAILQ_INIT(&rwb->free_batch_queue);
	STAILQ_INIT(&rwb->retry_batch_queue);

	rwb->xfer_size = xfer_size;
	rwb->md_size = md_size;
	rwb->num_entries = conf->rwb_size / FTL_BLOCK_SIZE;

	assert(rwb->num_entries % xfer_size == 0);
	num_batches = rwb->num_entries / xfer_size;

	rwb->batches = calloc(num_batches, sizeof(*rwb->batches));
	if (!rwb->batches) {
		goto error;
	}

	rwb->entries = calloc(rwb->num_entries, sizeof(*rwb->entries));
	if (!rwb->entries) {
		goto error;
	}

	rwb->free_queue = spdk_ring_create(SPDK_RING_TYPE_MP_MC,
					   spdk_align32pow2(rwb->num_entries + 1),
					   SPDK_ENV_SOCKET_ID_ANY);
	if (!rwb->free_queue) {
		SPDK_ERRLOG("Failed to create free entry queue\n");
		goto error;
	}

	rwb->submit_queue = spdk_ring_create(SPDK_RING_TYPE_MP_SC,
					     spdk_align32pow2(rwb->num_entries + 1),
					     SPDK_ENV_SOCKET_ID_ANY);
	if (!rwb->submit_queue) {
		SPDK_ERRLOG("Failed to create entry submission queue\n");
		goto error;
	}

	for (i = 0; i < rwb->num_entries; ++i) {
		entry = &rwb->entries[i];

		rc = ftl_rwb_init_entry(rwb, entry, i);
		if (spdk_unlikely(rc != 0)) {
			SPDK_ERRLOG("Failed to initialize RWB entry %u\n", i);
			goto error;
		}

		assert(rwb->entries[i].pos < rwb->num_entries);
		rc = spdk_ring_enqueue(rwb->free_queue, (void **)&entry, 1, NULL);
		if (spdk_unlikely(rc != 1)) {
			SPDK_ERRLOG("Failed to populate free entry queue\n");
			goto error;
		}
	}

	for (i = 0; i < num_batches; ++i) {
		rc = ftl_rwb_init_batch(rwb, &rwb->batches[i]);
		if (spdk_unlikely(rc != 0)) {
			SPDK_ERRLOG("Failed to initialize RWB batch %u\n", i);
			goto error;
		}
	}

	for (i = 0; i < FTL_RWB_TYPE_MAX; ++i) {
		rwb->limits[i] = ftl_rwb_entry_cnt(rwb);
	}

	return rwb;
error:
	ftl_rwb_free(rwb);
	return NULL;
}

void
ftl_rwb_free(struct ftl_rwb *rwb)
{
	struct ftl_rwb_entry *entry;
	unsigned int i;

	if (!rwb) {
		return;
	}

	if (rwb->entries != NULL) {
		for (i = 0; i < rwb->num_entries; ++i) {
			entry = &rwb->entries[i];

			if (entry->data == NULL) {
				break;
			}

			spdk_dma_free(rwb->entries[i].data);
			pthread_spin_destroy(&entry->lock);
		}

		free(rwb->entries);
	}

	if (rwb->batches != NULL) {
		for (i = 0; i < rwb->num_entries / rwb->xfer_size; ++i) {
			free(rwb->batches[i].entries);
		}

		free(rwb->batches);
	}

	spdk_ring_free(rwb->free_queue);
	spdk_ring_free(rwb->submit_queue);
	free(rwb);
}

void
ftl_rwb_batch_release(struct ftl_rwb_batch *batch)
{
	struct ftl_rwb *rwb = batch->rwb;
	struct ftl_rwb_entry *entry;
	unsigned int num_acquired __attribute__((unused));
	int rc, num_entries = ftl_rwb_batch_get_entry_count(batch);

	ftl_rwb_foreach(entry, batch) {
		num_acquired = __atomic_fetch_sub(&rwb->num_acquired[ftl_rwb_entry_type(entry)], 1,
						  __ATOMIC_SEQ_CST);
		assert(num_acquired > 0);
		entry->band = NULL;
	}

	rc = spdk_ring_enqueue(rwb->free_queue, (void **)batch->entries, num_entries, NULL);
	assert(rc == num_entries);
	memset(batch->entries, 0, sizeof(*batch->entries) * num_entries);

	STAILQ_INSERT_TAIL(&rwb->free_batch_queue, batch, stailq);
}

size_t
ftl_rwb_entry_cnt(const struct ftl_rwb *rwb)
{
	return rwb->num_entries;
}

size_t
ftl_rwb_num_batches(const struct ftl_rwb *rwb)
{
	return rwb->num_entries / rwb->xfer_size;
}

size_t
ftl_rwb_size(const struct ftl_rwb *rwb)
{
	/* TODO: remove either this or ftl_rwb_entry_cnt */
	return rwb->num_entries;
}

size_t
ftl_rwb_batch_get_offset(const struct ftl_rwb_batch *batch)
{
	/* TODO: figure out what we should do here */
	return 0;
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
ftl_rwb_get_active_batches(const struct ftl_rwb *rwb)
{
	/* TODO: do we still need this function? For now let's  */
	return spdk_ring_count(rwb->submit_queue) % rwb->xfer_size;
}

void
ftl_rwb_batch_revert(struct ftl_rwb_batch *batch)
{
	struct ftl_rwb *rwb = batch->rwb;

	STAILQ_INSERT_TAIL(&rwb->retry_batch_queue, batch, stailq);
	rwb->retry_batch_count++;
}

unsigned int
ftl_rwb_num_pending(struct ftl_rwb *rwb)
{
	return spdk_ring_count(rwb->submit_queue) + rwb->retry_batch_count * rwb->xfer_size;
}

void
ftl_rwb_push(struct ftl_rwb_entry *entry)
{
	struct ftl_rwb *rwb = entry->rwb;
	int rc;

	rc = spdk_ring_enqueue(rwb->submit_queue, (void **)&entry, 1, NULL);
	if (spdk_unlikely(rc != 1)) {
		assert(0 && "Should never happen");
	}
}

static int
ftl_rwb_check_limits(struct ftl_rwb *rwb, enum ftl_rwb_entry_type type)
{
	return ftl_rwb_num_acquired(rwb, type) >= rwb->limits[type];
}

struct ftl_rwb_entry *
ftl_rwb_acquire(struct ftl_rwb *rwb, enum ftl_rwb_entry_type type)
{
	struct ftl_rwb_entry *entry = NULL;

	if (ftl_rwb_check_limits(rwb, type)) {
		return NULL;
	}

	if (spdk_ring_dequeue(rwb->free_queue, (void **)&entry, 1) != 1) {
		return NULL;
	}

	__atomic_fetch_add(&rwb->num_acquired[type], 1, __ATOMIC_SEQ_CST);

	return entry;
}

void
ftl_rwb_disable_interleaving(struct ftl_rwb *rwb)
{
	/* TODO: figure out what to do here */
}

struct ftl_rwb_batch *
ftl_rwb_pop(struct ftl_rwb *rwb)
{
	struct ftl_rwb_batch *batch;
	size_t num_entries;

	if (!STAILQ_EMPTY(&rwb->retry_batch_queue)) {
		batch = STAILQ_FIRST(&rwb->retry_batch_queue);
		STAILQ_REMOVE(&rwb->retry_batch_queue, batch, ftl_rwb_batch, stailq);
		return batch;
	}

	if (spdk_ring_count(rwb->submit_queue) < rwb->xfer_size) {
		return NULL;
	}

	batch = STAILQ_FIRST(&rwb->free_batch_queue);
	assert(batch != NULL);
	STAILQ_REMOVE(&rwb->free_batch_queue, batch, ftl_rwb_batch, stailq);

	num_entries = spdk_ring_dequeue(rwb->submit_queue, (void **)batch->entries, rwb->xfer_size);
	assert(num_entries == rwb->xfer_size);

	return batch;
}

size_t
ftl_rwb_batch_get_iovcnt(const struct ftl_rwb_batch *batch)
{
	return ftl_rwb_batch_get_entry_count(batch);
}

void
ftl_rwb_batch_get_iovs(struct ftl_rwb_batch *batch, struct iovec *iovs)
{
	size_t i;

	for (i = 0; i < ftl_rwb_batch_get_entry_count(batch); ++i) {
		iovs[i].iov_base = batch->entries[i]->data;
		iovs[i].iov_len = FTL_BLOCK_SIZE;
	}
}

void *
ftl_rwb_batch_get_md(struct ftl_rwb_batch *batch)
{
	/* TODO: add support for separate metadata buffers */
	return NULL;
}

struct ftl_rwb_entry *
ftl_rwb_entry_from_offset(struct ftl_rwb *rwb, size_t offset)
{
	assert(offset < rwb->num_entries);

	return &rwb->entries[offset];
}

struct ftl_rwb_entry *
ftl_rwb_batch_get_entry(struct ftl_rwb_batch *batch, size_t idx)
{
	if (idx >= ftl_rwb_batch_get_entry_count(batch)) {
		return NULL;
	}

	return batch->entries[idx];
}

size_t
ftl_rwb_batch_get_entry_count(const struct ftl_rwb_batch *batch)
{
	return batch->rwb->xfer_size;
}

struct ftl_rwb_entry *
ftl_rwb_batch_first_entry(struct ftl_rwb_batch *batch)
{
	/* TODO: we should get rid of this function */
	return batch->entries[0];
}



#if 0
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
		entry->band = NULL;
		assert(num_acquired > 0);
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
ftl_rwb_size(const struct ftl_rwb *rwb)
{
	return rwb->num_batches * rwb->xfer_size;
}

size_t
ftl_rwb_batch_get_offset(const struct ftl_rwb_batch *batch)
{
	/* TODO: figure out what we should do here */
	return 0;
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
ftl_rwb_get_active_batches(const struct ftl_rwb *rwb)
{
	return rwb->num_active_batches;
}

void
ftl_rwb_batch_revert(struct ftl_rwb_batch *batch)
{
	struct ftl_rwb *rwb = batch->rwb;

	if (spdk_ring_enqueue(rwb->prio_queue, (void **)&batch, 1, NULL) != 1) {
		assert(0 && "Should never happen");
	}

	__atomic_fetch_add(&rwb->num_pending, rwb->xfer_size, __ATOMIC_SEQ_CST);
}

/* TODO: use spdk_ring_count(submit_quue) + len(submit_batch_queue) */
unsigned int
ftl_rwb_num_pending(struct ftl_rwb *rwb)
{
	return __atomic_load_n(&rwb->num_pending, __ATOMIC_SEQ_CST);
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
		if (spdk_ring_enqueue(rwb->submit_queue, (void **)&batch, 1, NULL) != 1) {
			assert(0 && "Should never happen");
		}
	}
}

static int
ftl_rwb_check_limits(struct ftl_rwb *rwb, enum ftl_rwb_entry_type type)
{
	return ftl_rwb_num_acquired(rwb, type) >= rwb->limits[type];
}

static struct ftl_rwb_batch *
_ftl_rwb_acquire_batch(struct ftl_rwb *rwb)
{
	struct ftl_rwb_batch *batch;
	size_t i;

	if (rwb->num_free_batches < rwb->max_active_batches) {
		return NULL;
	}

	for (i = 0; i < rwb->max_active_batches; i++) {
		batch = STAILQ_FIRST(&rwb->free_queue);
		STAILQ_REMOVE(&rwb->free_queue, batch, ftl_rwb_batch, stailq);
		rwb->num_free_batches--;

		STAILQ_INSERT_TAIL(&rwb->active_queue, batch, stailq);
		rwb->num_active_batches++;
	}

	return STAILQ_FIRST(&rwb->active_queue);
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

	current = STAILQ_FIRST(&rwb->active_queue);
	if (!current) {
		current = _ftl_rwb_acquire_batch(rwb);
		if (!current) {
			goto error;
		}
	}

	entry = &current->entries[current->num_acquired++];

	if (current->num_acquired >= rwb->xfer_size) {
		/* If the whole batch is filled, */
		/* remove the current batch from active_queue */
		/* since it will need to move to submit_queue */
		STAILQ_REMOVE(&rwb->active_queue, current, ftl_rwb_batch, stailq);
		rwb->num_active_batches--;
	} else if (current->num_acquired % rwb->interleave_offset == 0) {
		/* If the current batch is filled by the interleaving offset, */
		/* move the current batch at the tail of active_queue */
		/* to place the next logical blocks into another batch. */
		STAILQ_REMOVE(&rwb->active_queue, current, ftl_rwb_batch, stailq);
		STAILQ_INSERT_TAIL(&rwb->active_queue, current, stailq);
	}

	pthread_spin_unlock(&rwb->lock);
	__atomic_fetch_add(&rwb->num_acquired[type], 1, __ATOMIC_SEQ_CST);
	__atomic_fetch_add(&rwb->num_pending, 1, __ATOMIC_SEQ_CST);
	return entry;
error:
	pthread_spin_unlock(&rwb->lock);
	return NULL;
}

void
ftl_rwb_disable_interleaving(struct ftl_rwb *rwb)
{
	struct ftl_rwb_batch *batch, *temp;

	pthread_spin_lock(&rwb->lock);
	rwb->max_active_batches = 1;
	rwb->interleave_offset = rwb->xfer_size;

	STAILQ_FOREACH_SAFE(batch, &rwb->active_queue, stailq, temp) {
		if (batch->num_acquired == 0) {
			STAILQ_REMOVE(&rwb->active_queue, batch, ftl_rwb_batch, stailq);
			rwb->num_active_batches--;

			assert(batch->num_ready == 0);
			assert(batch->num_acquired == 0);

			STAILQ_INSERT_TAIL(&rwb->free_queue, batch, stailq);
			rwb->num_free_batches++;
		}
	}
	pthread_spin_unlock(&rwb->lock);
}

struct ftl_rwb_batch *
ftl_rwb_pop(struct ftl_rwb *rwb)
{
	struct ftl_rwb_batch *batch = NULL;
	unsigned int num_pending __attribute__((unused));

	if (spdk_ring_dequeue(rwb->prio_queue, (void **)&batch, 1) == 1) {
		num_pending = __atomic_fetch_sub(&rwb->num_pending, rwb->xfer_size,
						 __ATOMIC_SEQ_CST);
		assert(num_pending > 0);
		return batch;
	}

	if (spdk_ring_dequeue(rwb->submit_queue, (void **)&batch, 1) == 1) {
		num_pending = __atomic_fetch_sub(&rwb->num_pending, rwb->xfer_size,
						 __ATOMIC_SEQ_CST);
		assert(num_pending > 0);
		return batch;
	}

	return NULL;
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
#endif
