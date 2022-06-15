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

#ifndef FTL_IO_H
#define FTL_IO_H

#include "spdk/stdinc.h"
#include "spdk/nvme.h"
#include "spdk/ftl.h"
#include "spdk/bdev.h"
#include "spdk/util.h"

#include "ftl_internal.h"
#include "utils/ftl_md.h"

struct spdk_ftl_dev;
struct ftl_io;

typedef void (*ftl_io_fn)(struct ftl_io *, void *, int);

/* IO flags */
enum ftl_io_flags {
	/* Indicates whether IO is already initialized */
	FTL_IO_INITIALIZED	= (1 << 0),
	/* Internal based IO (defrag, metadata etc.) */
	FTL_IO_INTERNAL		= (1 << 1),
	/* Indicated whether the user IO pinned LBA */
	FTL_IO_PINNED		= (1 << 2),
};

enum ftl_io_type {
	FTL_IO_READ,
	FTL_IO_WRITE,
	FTL_IO_ERASE,
	FTL_IO_UNMAP,
};

#define FTL_IO_MAX_IOVEC 4

struct ftl_io_init_opts {
	struct spdk_ftl_dev			*dev;

	/* IO descriptor */
	struct ftl_io				*io;

	/* Parent request */
	struct ftl_io				*parent;

	/* Size of IO descriptor */
	size_t						size;

	/* IO flags */
	int							flags;

	/* IO channel associated with IO */
	struct spdk_io_channel		*ioch;

	/* IO type */
	enum ftl_io_type			type;

	/* Number of logical blocks */
	size_t						num_blocks;

	/* Data */
	struct iovec				iovs[FTL_IO_MAX_IOVEC];
	int							iovcnt;

	/* Metadata */
	void						*md;

	/* Callback's function */
	ftl_io_fn				cb_fn;

	/* Callback's context */
	void					*cb_ctx;
};

/* General IO descriptor */
struct ftl_io {
	/* Device */
	struct spdk_ftl_dev		*dev;

	/* IO channel */
	struct spdk_io_channel	*ioch;

	/* LBA address */
	uint64_t				lba;

	/* First block address */
	ftl_addr				addr;

	/* Number of processed blocks */
	size_t					pos;

	/* Number of blocks */
	size_t					num_blocks;

	/* IO vector pointer */
	struct iovec			*iov;

	/* Metadata */
	void					*md;

	/* Number of IO vectors */
	size_t					iov_cnt;

	/* Position within the io vector array */
	size_t					iov_pos;

	/* Offset within the iovec (in blocks) */
	size_t					iov_off;

	/* Request status */
	int						status;

	/* Number of split requests */
	size_t					req_cnt;

	/* Callback's function */
	ftl_io_fn				cb_fn;

	/* Callback's context */
	void					*cb_ctx;

	/* User callback function */
	spdk_ftl_fn				user_fn;

	/* Flags */
	int						flags;

	/* IO type */
	enum ftl_io_type		type;

	/* Done flag */
	bool					done;

	/* Parent request */
	struct ftl_io			*parent;
	/* Child requests list */
	LIST_HEAD(, ftl_io)		children;
	/* Child list link */
	LIST_ENTRY(ftl_io)		child_entry;

	/* Used by retry and write completion queues */
	TAILQ_ENTRY(ftl_io)		queue_entry;

	/* Reference to the chunk within NV cache */
	struct ftl_nv_cache_chunk *nv_cache_chunk;

	/* Logical to physical mapping for this IO, number of entries equals to
	 * number of transfer blocks */
	ftl_addr				*map;

	struct spdk_bdev_io_wait_entry	bdev_io_wait;
};

struct ftl_rq_entry {
	/* Data payload of single entry (block) */
	void *io_payload;

	void *io_md;

	/*
	 * Physical address of that particular block.  Valid once the data has
	 * been written out.
	 */
	ftl_addr addr;

	/* Logical block address */
	uint64_t lba;

	/* Index of this entry within FTL request */
	const uint64_t index;

	struct {
		void *priv;
	} owner;

	struct {
		uint64_t offset_blocks;
		uint64_t num_blocks;
		struct spdk_bdev_io_wait_entry wait_entry;
	} bdev_io;
};

struct ftl_rq {
	struct spdk_ftl_dev *dev;

	/* Request queue entry */
	TAILQ_ENTRY(ftl_rq) qentry;

	/* Number of block within the request */
	uint64_t num_blocks;

	/* Extended metadata for IO. Its size is io_md_size * num_blocks */
	void *io_md;

	/* Size of extended metadata size for one entry */
	uint64_t io_md_size;

	/* Size of IO vector array */
	uint64_t io_vec_size;

	/* Array of IO vectors, its size equals to num_blocks */
	struct iovec *io_vec;

	/* Payload for IO */
	void *io_payload;

	/* Request result status */
	bool success;

	/* Fields for owner of this request */
	struct {
		/* End request callback */
		void (*cb)(struct ftl_rq *rq);

		/* Owner context */
		void *priv;

		/* This is user IO */
		bool uio;
	} owner;

	/* Iterator fields for processing state of the request */
	struct {
		uint32_t idx;

		uint32_t count;

		/* Queue depth on this request */
		uint32_t qd;

		uint32_t remaining;
		int status;
	} iter;

	/* Private fields for issuing IO */
	struct {
		/* Request physical address, on IO completion set for append device */
		ftl_addr addr;

		/* Zone to which IO is issued */
		struct ftl_zone *zone;

		struct spdk_bdev_io_wait_entry bdev_io_wait;
	} io;

	/* For writing metadata */
	struct ftl_md_io_entry_ctx md_persist_entry_ctx;

	struct ftl_rq_entry entries[];
};

struct ftl_basic_rq {
	struct spdk_ftl_dev *dev;

	/* Request queue entry */
	TAILQ_ENTRY(ftl_basic_rq) qentry;

	/* Number of block within the request */
	uint64_t num_blocks;

	/* Payload for IO */
	void *io_payload;

	/* Request result status */
	bool success;

	/* Fields for owner of this request */
	struct {
		/* End request callback */
		void (*cb)(struct ftl_basic_rq *brq);

		/* Owner context */
		void *priv;

		/* This is user IO */
		bool uio;
	} owner;

	/* Private fields for issuing IO */
	struct {
		/* Request physical address, on IO completion set for append device */
		ftl_addr addr;

		/* Zone to which IO is issued */
		struct ftl_zone *zone;

		/* Chunk to which IO is issued */
		struct ftl_nv_cache_chunk *chunk;

		struct spdk_bdev_io_wait_entry bdev_io_wait;
	} io;
};

struct ftl_io *ftl_io_alloc(struct spdk_io_channel *ch);
struct ftl_io *ftl_io_alloc_child(struct ftl_io *parent);
void ftl_io_fail(struct ftl_io *io, int status);
void ftl_io_free(struct ftl_io *io);
struct ftl_io *ftl_io_init_internal(const struct ftl_io_init_opts *opts);
void ftl_io_reinit(struct ftl_io *io, ftl_io_fn cb,
		   void *ctx, int flags, int type);
void ftl_io_clear(struct ftl_io *io);
void ftl_io_inc_req(struct ftl_io *io);
void ftl_io_dec_req(struct ftl_io *io);
struct iovec *ftl_io_iovec(struct ftl_io *io);
uint64_t ftl_io_current_lba(const struct ftl_io *io);
uint64_t ftl_io_get_lba(const struct ftl_io *io, size_t offset);
void ftl_io_advance(struct ftl_io *io, size_t num_blocks);
size_t ftl_iovec_num_blocks(struct iovec *iov, size_t iov_cnt);
void *ftl_io_iovec_addr(struct ftl_io *io);
size_t ftl_io_iovec_len_left(struct ftl_io *io);
int ftl_io_user_init(struct spdk_io_channel *ioch, struct ftl_io *io, uint64_t lba,
		     size_t num_blocks, struct iovec *iov, size_t iov_cnt, spdk_ftl_fn cb_fn,
		     void *cb_arg, int type);
void ftl_io_complete(struct ftl_io *io);
void ftl_io_reset(struct ftl_io *io);

static inline void
ftl_basic_rq_init(struct spdk_ftl_dev *dev, struct ftl_basic_rq *brq,
		  void *io_payload, uint64_t num_blocks)
{
	brq->dev = dev;
	brq->io_payload = io_payload;
	brq->num_blocks = num_blocks;
	brq->success = false;
}

static inline void
ftl_basic_rq_set_owner(struct ftl_basic_rq *brq, void (*cb)(struct ftl_basic_rq *brq), void *priv)
{
	brq->owner.cb = cb;
	brq->owner.priv = priv;
}

static inline void
ftl_rq_swap_payload(struct ftl_rq *a, uint32_t aidx,
		    struct ftl_rq *b, uint32_t bidx)
{
	assert(aidx < a->num_blocks);
	assert(bidx < b->num_blocks);

	void *a_payload = a->io_vec[aidx].iov_base;
	void *b_payload = b->io_vec[bidx].iov_base;

	a->io_vec[aidx].iov_base = b_payload;
	a->entries[aidx].io_payload = b_payload;

	b->io_vec[bidx].iov_base = a_payload;
	b->entries[bidx].io_payload = a_payload;
}

static inline struct ftl_rq *ftl_rq_from_entry(struct ftl_rq_entry *entry)
{
	uint64_t idx = entry->index;
	struct ftl_rq *rq = SPDK_CONTAINEROF(entry, struct ftl_rq, entries[idx]);
	return rq;
}


static inline bool
ftl_io_done(const struct ftl_io *io)
{
	return io->req_cnt == 0 && io->pos == io->num_blocks;
}

#endif /* FTL_IO_H */
