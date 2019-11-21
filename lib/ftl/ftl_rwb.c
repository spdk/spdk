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
#include "ftl_io.h"

#define MAX_RWB_REGIONS 128

struct ftl_rwb_batch {
	/* Parent RWB */
	struct ftl_rwb				*rwb;

	/* Entry buffer */
	struct ftl_rwb_entry			**entries;

	size_t					num_entries;

	/* Queue entry */
	STAILQ_ENTRY(ftl_rwb_batch)		stailq;
};

struct ftl_rwb {
	struct spdk_ftl_dev *dev;
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

	/* Entry buffer */
	struct ftl_rwb_entry			**entries;
	/* Batch buffer */
	struct ftl_rwb_batch			*batches;

	struct ftl_rwb_batch			*current;

	size_t					current_region_pos;
	pthread_mutex_t				lock;
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

/* TODO ftl_rwb_free_entries */
int
ftl_rwb_init_entries(struct ftl_rwb *rwb, struct ftl_rwb_entry *entries, size_t count,
		     void *data, struct ftl_wbuf_io_channel *ioch)
{
	struct ftl_rwb_entry *entry;
	size_t i, region_offset;
	int rc = 0;

	pthread_mutex_lock(&rwb->lock);
	if (rwb->current_region_pos + count > rwb->num_entries) {
		pthread_mutex_unlock(&rwb->lock);
		return -ENOMEM;
	}

	region_offset = rwb->current_region_pos;
	rwb->current_region_pos += count;
	pthread_mutex_unlock(&rwb->lock);

	for (i = 0; i < count; ++i) {
		entry = &entries[i];

		entry->rwb = rwb;
		entry->pos = region_offset + i;
		entry->ioch = ioch;
		entry->data = (char *)data + i * FTL_BLOCK_SIZE;

		if (data == NULL) {
			/* The data should be aligned to a cc.mps, but since currently there's no way of
			 * retrieving this value at the FTL level, use FTL_BLOCK_SIZE instead. */
			entry->data = spdk_dma_zmalloc(FTL_BLOCK_SIZE, FTL_BLOCK_SIZE, NULL);
			if (!entry->data) {
				rc = -ENOMEM;
				goto finish;
			}
		}

		rc = pthread_spin_init(&entry->lock, PTHREAD_PROCESS_PRIVATE);
		if (spdk_unlikely(rc != 0)) {
			spdk_dma_free(entry->data);
			entry->data = NULL;
			goto finish;
		}

		rwb->entries[region_offset + i] = entry;
	}

	if (spdk_ring_enqueue(ioch->free_entry_queue, (void **)&rwb->entries[region_offset], count,
			      NULL) != count) {
		rc = -ENOMEM;
	}
finish:
	if (spdk_unlikely(rc != 0)) {
		for (i = 0; i < count; ++i) {
			entry = &entries[i];
			if (!entry->data) {
				break;
			}

			spdk_dma_free(entry->data);
		}
	}

	return rc;
}

struct ftl_rwb *
ftl_rwb_init(struct spdk_ftl_dev *dev, const struct spdk_ftl_conf *conf, size_t xfer_size,
	     size_t md_size, size_t num_punits)
{
	struct ftl_rwb *rwb;
	unsigned int i, num_batches;
	int rc;

	rwb = calloc(1, sizeof(*rwb));
	if (!rwb) {
		SPDK_ERRLOG("Memory allocation failure\n");
		return NULL;
	}

	rwb->dev = dev;

	STAILQ_INIT(&rwb->free_batch_queue);
	STAILQ_INIT(&rwb->retry_batch_queue);

	rwb->xfer_size = xfer_size;
	rwb->md_size = md_size;
	rwb->num_entries = MAX_RWB_REGIONS * conf->rwb_size / FTL_BLOCK_SIZE;

	assert(rwb->num_entries % xfer_size == 0);
	num_batches = rwb->num_entries / xfer_size;

	rwb->batches = calloc(num_batches, sizeof(*rwb->batches));
	if (!rwb->batches) {
		goto error;
	}

	if (pthread_mutex_init(&rwb->lock, NULL)) {
		goto error;
	}

	rwb->entries = calloc(rwb->num_entries, sizeof(*rwb->entries));
	if (!rwb->entries) {
		goto error;
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
	unsigned int i;

	if (!rwb) {
		return;
	}

	free(rwb->entries);

	if (rwb->batches != NULL) {
		for (i = 0; i < rwb->num_entries / rwb->xfer_size; ++i) {
			free(rwb->batches[i].entries);
		}

		free(rwb->batches);
	}

	free(rwb);
}

void
ftl_rwb_batch_release(struct ftl_rwb_batch *batch)
{
	struct ftl_rwb *rwb = batch->rwb;
	struct ftl_rwb_entry *entry;
	int rc, count = 0, num_entries = ftl_rwb_batch_get_entry_count(batch);

	ftl_rwb_foreach(entry, batch) {
		entry->band = NULL;
		rc = spdk_ring_enqueue(entry->ioch->free_entry_queue, (void **)&entry, 1, NULL);
		assert(rc == 1);
		count++;
	}

	assert(count == num_entries);
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
	/* TODO: we should probably iterate through all IO channels and return the sum of all
	 * acquired requests there */
	return 0;
}

size_t
ftl_rwb_get_active_batches(const struct ftl_rwb *rwb)
{
	/* TODO: do we still need this function?  */
	return rwb->current == NULL ? 0 : 1;
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
	/* TODO: this should take all the IO channels into account */
	return rwb->retry_batch_count * rwb->xfer_size;
}

void
ftl_rwb_push(struct ftl_rwb_entry *entry)
{
	struct ftl_wbuf_io_channel *ioch = entry->ioch;
	int rc;

	rc = spdk_ring_enqueue(ioch->submit_entry_queue, (void **)&entry, 1, NULL);
	if (spdk_unlikely(rc != 1)) {
		assert(0 && "Should never happen");
	}
}

struct ftl_rwb_entry *
ftl_rwb_acquire(struct ftl_rwb *rwb, struct ftl_wbuf_io_channel *ioch, enum ftl_rwb_entry_type type)
{
	struct ftl_rwb_entry *entry = NULL;

	/* TODO: limits */
	if (spdk_ring_dequeue(ioch->free_entry_queue, (void **)&entry, 1) != 1) {
		return NULL;
	}

	return entry;
}

void
ftl_rwb_disable_interleaving(struct ftl_rwb *rwb)
{
	/* TODO: figure out what to do here */
}

#if 0
void
dump_rwb_stats(struct spdk_ftl_dev *dev)
{
	struct ftl_wbuf_io_channel *ioch;

	pthread_spin_lock(&dev->lock);
	TAILQ_FOREACH(ioch, &dev->ioch_queue, tailq) {
		printf("ioch[%p]: %zu, %zu\n", ioch, spdk_ring_count(ioch->free_entry_queue),
		       spdk_ring_count(ioch->submit_entry_queue));
	}
	pthread_spin_unlock(&dev->lock);
}
#endif

struct ftl_rwb_batch *
ftl_rwb_pop(struct ftl_rwb *rwb)
{
	struct ftl_rwb_batch *batch = rwb->current;
	struct ftl_wbuf_io_channel *ioch, *tmp;
	size_t num_entries = 0, num_io_channels = 0;

	if (!STAILQ_EMPTY(&rwb->retry_batch_queue)) {
		batch = STAILQ_FIRST(&rwb->retry_batch_queue);
		STAILQ_REMOVE(&rwb->retry_batch_queue, batch, ftl_rwb_batch, stailq);
		return batch;
	}

	if (batch == NULL) {
		batch = rwb->current = STAILQ_FIRST(&rwb->free_batch_queue);
		if (batch == NULL) {
			return NULL;
		}

		STAILQ_REMOVE(&rwb->free_batch_queue, batch, ftl_rwb_batch, stailq);

		batch->num_entries = 0;
	}

	assert(batch->num_entries < rwb->xfer_size);

	pthread_spin_lock(&rwb->dev->lock);
	TAILQ_FOREACH_SAFE(ioch, &rwb->dev->ioch_queue, tailq, tmp) {
		num_entries = spdk_ring_dequeue(ioch->submit_entry_queue,
						(void **)&batch->entries[batch->num_entries],
						rwb->xfer_size - batch->num_entries);
		batch->num_entries += num_entries;

		/* Put the IO channel at the end of the queue to guarantee fairness */
		TAILQ_REMOVE(&rwb->dev->ioch_queue, ioch, tailq);
		TAILQ_INSERT_TAIL(&rwb->dev->ioch_queue, ioch, tailq);

		if (batch->num_entries == rwb->xfer_size) {
			break;
		}

		if (++num_io_channels == rwb->dev->num_io_channels) {
			break;
		}
	}
	pthread_spin_unlock(&rwb->dev->lock);

	for (size_t i = 0; i < batch->num_entries; ++i) {
		assert(batch->entries[i] != NULL);
	}

	if (batch->num_entries == rwb->xfer_size) {
		for (size_t i = 0; i < rwb->xfer_size; ++i) {
			assert(batch->entries[i] != NULL);
		}

		rwb->current = NULL;
		return batch;
	}

	return NULL;
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

	return rwb->entries[offset];
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
