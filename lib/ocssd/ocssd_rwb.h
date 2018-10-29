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

#ifndef OCSSD_RWB_H
#define OCSSD_RWB_H

#include <spdk/stdinc.h>
#include <stdatomic.h>
#include "ocssd_io.h"
#include "ocssd_ppa.h"
#include "ocssd_trace.h"

struct ocssd_rwb;
struct ocssd_conf;
struct ocssd_rwb_batch;

enum ocssd_rwb_entry_type {
	OCSSD_RWB_TYPE_INTERNAL,
	OCSSD_RWB_TYPE_USER,
	OCSSD_RWB_TYPE_MAX
};

/* Write buffer entry */
struct ocssd_rwb_entry {
	/* Owner rwb */
	struct ocssd_rwb			*rwb;

	/* Batch containing the entry */
	struct ocssd_rwb_batch			*batch;

	/* Logical address */
	uint64_t				lba;

	/* Physical address */
	struct ocssd_ppa			ppa;

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
	atomic_bool				valid;

	/* Trace group id */
	ocssd_trace_group_t			trace;

	/* Batch list entry */
	LIST_ENTRY(ocssd_rwb_entry)		list_entry;
};

struct ocssd_rwb *ocssd_rwb_init(const struct ocssd_conf *conf, size_t xfer_size, size_t md_size);
void	ocssd_rwb_free(struct ocssd_rwb *rwb);
void	ocssd_rwb_batch_release(struct ocssd_rwb_batch *batch);
void	ocssd_rwb_push(struct ocssd_rwb_entry *entry);
size_t	ocssd_rwb_entry_cnt(const struct ocssd_rwb *rwb);
void	ocssd_rwb_set_limits(struct ocssd_rwb *rwb, const size_t limit[OCSSD_RWB_TYPE_MAX]);
void	ocssd_rwb_get_limits(struct ocssd_rwb *rwb, size_t limit[OCSSD_RWB_TYPE_MAX]);
size_t	ocssd_rwb_num_acquired(struct ocssd_rwb *rwb, enum ocssd_rwb_entry_type type);
size_t	ocssd_rwb_num_batches(const struct ocssd_rwb *rwb);
struct ocssd_rwb_entry *ocssd_rwb_acquire(struct ocssd_rwb *rwb, enum ocssd_rwb_entry_type type);
struct ocssd_rwb_batch *ocssd_rwb_pop(struct ocssd_rwb *rwb);
struct ocssd_rwb_batch *ocssd_rwb_first_batch(struct ocssd_rwb *rwb);
struct ocssd_rwb_batch *ocssd_rwb_next_batch(struct ocssd_rwb_batch *batch);
int	ocssd_rwb_batch_empty(struct ocssd_rwb_batch *batch);
struct ocssd_rwb_entry *ocssd_rwb_entry_from_offset(struct ocssd_rwb *rwb, size_t offset);
size_t	ocssd_rwb_batch_get_offset(const struct ocssd_rwb_batch *batch);
void	ocssd_rwb_batch_revert(struct ocssd_rwb_batch *batch);
struct ocssd_rwb_entry *ocssd_rwb_batch_first_entry(struct ocssd_rwb_batch *batch);
void	*ocssd_rwb_batch_data(struct ocssd_rwb_batch *batch);
void	*ocssd_rwb_batch_md(struct ocssd_rwb_batch *batch);

static inline void
_ocssd_rwb_entry_set_valid(struct ocssd_rwb_entry *entry, int valid)
{
	atomic_store(&entry->valid, !!valid);
}

static inline void
ocssd_rwb_entry_set_valid(struct ocssd_rwb_entry *entry)
{
	_ocssd_rwb_entry_set_valid(entry, 1);
}

static inline void
ocssd_rwb_entry_invalidate(struct ocssd_rwb_entry *entry)
{
	_ocssd_rwb_entry_set_valid(entry, 0);
}

static inline int
ocssd_rwb_entry_valid(struct ocssd_rwb_entry *entry)
{
	return atomic_load(&entry->valid);
}

static inline enum ocssd_rwb_entry_type
ocssd_rwb_type_from_flags(int flags) {
	return (flags & OCSSD_IO_INTERNAL) ? OCSSD_RWB_TYPE_INTERNAL : OCSSD_RWB_TYPE_USER;
}

static inline enum ocssd_rwb_entry_type
ocssd_rwb_entry_type(const struct ocssd_rwb_entry *entry) {
	return ocssd_rwb_type_from_flags(entry->flags);
}

static inline int
ocssd_rwb_entry_internal(const struct ocssd_rwb_entry *entry)
{
	return ocssd_rwb_entry_type(entry) == OCSSD_RWB_TYPE_INTERNAL;
}

#define ocssd_rwb_foreach(entry, batch) \
	for (entry = ocssd_rwb_batch_first_entry(batch); \
	     entry; entry = LIST_NEXT(entry, list_entry))

#define ocssd_rwb_foreach_batch(batch, rwb) \
	for (batch = ocssd_rwb_first_batch(rwb); batch; \
	     batch = ocssd_rwb_next_batch(batch))

#endif /* OCSSD_RWB_H */
