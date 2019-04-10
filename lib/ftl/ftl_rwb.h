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

#ifndef FTL_RWB_H
#define FTL_RWB_H

#include "spdk/stdinc.h"
#include "spdk/queue.h"

#include "ftl_io.h"
#include "ftl_ppa.h"
#include "ftl_trace.h"

struct ftl_rwb;
struct spdk_ftl_conf;
struct ftl_rwb_batch;

enum ftl_rwb_entry_type {
	FTL_RWB_TYPE_INTERNAL,
	FTL_RWB_TYPE_USER,
	FTL_RWB_TYPE_MAX
};

/* Write buffer entry */
struct ftl_rwb_entry {
	/* Owner rwb */
	struct ftl_rwb				*rwb;

	/* Batch containing the entry */
	struct ftl_rwb_batch			*batch;

	/* Logical address */
	uint64_t				lba;

	/* Physical address */
	struct ftl_ppa				ppa;

	/* Position within the rwb's buffer */
	unsigned int				pos;

	/* Data pointer */
	void					*data;

	/* Metadata pointer */
	void					*md;

	/* Data/state lock */
	pthread_spinlock_t			lock;

	/* Flags */
	unsigned int				flags;

	/* Indicates whether the entry is part of cache and is assigned a PPA */
	bool					valid;

	/* Trace group id */
	uint64_t				trace;

	/* Batch list entry */
	LIST_ENTRY(ftl_rwb_entry)		list_entry;
};

struct ftl_rwb *ftl_rwb_init(const struct spdk_ftl_conf *conf, size_t xfer_size,
			     size_t md_size, size_t num_punits);
size_t ftl_rwb_get_active_batches_opt(const struct ftl_rwb *rwb);
size_t ftl_rwb_get_active_batches(const struct ftl_rwb *rwb);
void	ftl_rwb_free(struct ftl_rwb *rwb);
void	ftl_rwb_batch_release(struct ftl_rwb_batch *batch);
void	ftl_rwb_push(struct ftl_rwb_entry *entry);
size_t	ftl_rwb_entry_cnt(const struct ftl_rwb *rwb);
void	ftl_rwb_set_limits(struct ftl_rwb *rwb, const size_t limit[FTL_RWB_TYPE_MAX]);
void	ftl_rwb_get_limits(struct ftl_rwb *rwb, size_t limit[FTL_RWB_TYPE_MAX]);
size_t	ftl_rwb_num_acquired(struct ftl_rwb *rwb, enum ftl_rwb_entry_type type);
size_t	ftl_rwb_num_batches(const struct ftl_rwb *rwb);
struct ftl_rwb_entry *ftl_rwb_acquire(struct ftl_rwb *rwb, enum ftl_rwb_entry_type type);
struct ftl_rwb_batch *ftl_rwb_pop(struct ftl_rwb *rwb);
struct ftl_rwb_batch *ftl_rwb_first_batch(struct ftl_rwb *rwb);
struct ftl_rwb_batch *ftl_rwb_next_batch(struct ftl_rwb_batch *batch);
int	ftl_rwb_batch_empty(struct ftl_rwb_batch *batch);
struct ftl_rwb_entry *ftl_rwb_entry_from_offset(struct ftl_rwb *rwb, size_t offset);
size_t	ftl_rwb_batch_get_offset(const struct ftl_rwb_batch *batch);
void	ftl_rwb_batch_revert(struct ftl_rwb_batch *batch);
struct ftl_rwb_entry *ftl_rwb_batch_first_entry(struct ftl_rwb_batch *batch);
void	*ftl_rwb_batch_get_data(struct ftl_rwb_batch *batch);
void	*ftl_rwb_batch_get_md(struct ftl_rwb_batch *batch);
void   ftl_rwb_process_shutdown(struct ftl_rwb *rwb);

static inline void
_ftl_rwb_entry_set_valid(struct ftl_rwb_entry *entry, bool valid)
{
	__atomic_store_n(&entry->valid, valid, __ATOMIC_SEQ_CST);
}

static inline void
ftl_rwb_entry_set_valid(struct ftl_rwb_entry *entry)
{
	_ftl_rwb_entry_set_valid(entry, true);
}

static inline void
ftl_rwb_entry_invalidate(struct ftl_rwb_entry *entry)
{
	_ftl_rwb_entry_set_valid(entry, false);
}

static inline int
ftl_rwb_entry_valid(struct ftl_rwb_entry *entry)
{
	return __atomic_load_n(&entry->valid, __ATOMIC_SEQ_CST);
}

static inline enum ftl_rwb_entry_type
ftl_rwb_type_from_flags(int flags) {
	return (flags & FTL_IO_INTERNAL) ? FTL_RWB_TYPE_INTERNAL : FTL_RWB_TYPE_USER;
}

static inline enum ftl_rwb_entry_type
ftl_rwb_entry_type(const struct ftl_rwb_entry *entry) {
	return ftl_rwb_type_from_flags(entry->flags);
}

static inline int
ftl_rwb_entry_internal(const struct ftl_rwb_entry *entry)
{
	return ftl_rwb_entry_type(entry) == FTL_RWB_TYPE_INTERNAL;
}

#define ftl_rwb_foreach(entry, batch) \
	for (entry = ftl_rwb_batch_first_entry(batch); \
	     entry; entry = LIST_NEXT(entry, list_entry))

#define ftl_rwb_foreach_batch(batch, rwb) \
	for (batch = ftl_rwb_first_batch(rwb); batch; \
	     batch = ftl_rwb_next_batch(batch))

#endif /* FTL_RWB_H */
