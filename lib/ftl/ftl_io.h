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

#include "ftl_addr.h"
#include "ftl_trace.h"

struct spdk_ftl_dev;
struct ftl_band;
struct ftl_batch;
struct ftl_io;

typedef int (*ftl_md_pack_fn)(struct ftl_band *);
typedef void (*ftl_io_fn)(struct ftl_io *, void *, int);

/* IO flags */
enum ftl_io_flags {
	/* Indicates whether IO is already initialized */
	FTL_IO_INITIALIZED	= (1 << 0),
	/* Internal based IO (defrag, metadata etc.) */
	FTL_IO_INTERNAL		= (1 << 1),
	/* Indicates that the IO should not go through if there's */
	/* already another one scheduled to the same LBA */
	FTL_IO_WEAK		= (1 << 2),
	/* Indicates that the IO is used for padding */
	FTL_IO_PAD		= (1 << 3),
	/* The IO operates on metadata */
	FTL_IO_MD		= (1 << 4),
	/* Using physical instead of logical address */
	FTL_IO_PHYSICAL_MODE	= (1 << 5),
	/* Indicates that IO contains noncontiguous LBAs */
	FTL_IO_VECTOR_LBA	= (1 << 6),
	/* The IO is directed to non-volatile cache */
	FTL_IO_CACHE		= (1 << 7),
	/* Indicates that physical address should be taken from IO struct, */
	/* not assigned by wptr, only works if wptr is also in direct mode */
	FTL_IO_DIRECT_ACCESS	= (1 << 8),
	/* Bypass the non-volatile cache */
	FTL_IO_BYPASS_CACHE	= (1 << 9),
};

enum ftl_io_type {
	FTL_IO_READ,
	FTL_IO_WRITE,
	FTL_IO_ERASE,
};

#define FTL_IO_MAX_IOVEC 64

struct ftl_io_init_opts {
	struct spdk_ftl_dev			*dev;

	/* IO descriptor */
	struct ftl_io				*io;

	/* Parent request */
	struct ftl_io				*parent;

	/* Size of IO descriptor */
	size_t                                  size;

	/* IO flags */
	int                                     flags;

	/* IO type */
	enum ftl_io_type			type;

	/* Transfer batch, set for IO going through the write buffer */
	struct ftl_batch			*batch;

	/* Band to which the IO is directed */
	struct ftl_band				*band;

	/* Number of logical blocks */
	size_t                                  num_blocks;

	/* Data */
	struct iovec				iovs[FTL_IO_MAX_IOVEC];
	int					iovcnt;

	/* Metadata */
	void                                    *md;

	/* Callback's function */
	ftl_io_fn				cb_fn;

	/* Callback's context */
	void					*cb_ctx;
};

struct ftl_io_channel;

struct ftl_wbuf_entry {
	/* IO channel that owns the write buffer entry */
	struct ftl_io_channel			*ioch;
	/* Data payload (single block) */
	void					*payload;
	/* Index within the IO channel's wbuf_entries array */
	uint32_t				index;
	uint32_t				io_flags;
	/* Points at the band the data is copied from.  Only valid for internal
	 * requests coming from reloc.
	 */
	struct ftl_band				*band;
	/* Physical address of that particular block.  Valid once the data has
	 * been written out.
	 */
	struct ftl_addr				addr;
	/* Logical block address */
	uint64_t				lba;

	/* Trace ID of the requests the entry is part of */
	uint64_t				trace;

	/* Indicates that the entry was written out and is still present in the
	 * L2P table.
	 */
	bool					valid;
	/* Lock that protects the entry from being evicted from the L2P */
	pthread_spinlock_t			lock;
	TAILQ_ENTRY(ftl_wbuf_entry)		tailq;
};

#define FTL_IO_CHANNEL_INDEX_INVALID ((uint64_t)-1)

struct ftl_io_channel {
	/* Device */
	struct spdk_ftl_dev			*dev;
	/* IO pool element size */
	size_t					elem_size;
	/* Index within the IO channel array */
	uint64_t				index;
	/* IO pool */
	struct spdk_mempool			*io_pool;
	/* Underlying device IO channel */
	struct spdk_io_channel			*base_ioch;
	/* Persistent cache IO channel */
	struct spdk_io_channel			*cache_ioch;
	/* Poller used for completing write requests and retrying IO */
	struct spdk_poller			*poller;
	/* Write completion queue */
	TAILQ_HEAD(, ftl_io)			write_cmpl_queue;
	TAILQ_HEAD(, ftl_io)			retry_queue;
	TAILQ_ENTRY(ftl_io_channel)		tailq;

	/* Array of write buffer entries */
	struct ftl_wbuf_entry			*wbuf_entries;
	/* Write buffer data payload */
	void					*wbuf_payload;
	/* Number of write buffer entries */
	uint32_t				num_entries;
	/* Write buffer queues */
	struct spdk_ring			*free_queue;
	struct spdk_ring			*submit_queue;
	/* Maximum number of concurrent user writes */
	uint32_t				qdepth_limit;
	/* Current number of concurrent user writes */
	uint32_t				qdepth_current;
	/* Means that the IO channel is being flushed */
	bool					flush;
};

/* General IO descriptor */
struct ftl_io {
	/* Device */
	struct spdk_ftl_dev			*dev;

	/* IO channel */
	struct spdk_io_channel			*ioch;

	union {
		/* LBA table */
		uint64_t			*vector;

		/* First LBA */
		uint64_t			single;
	} lba;

	/* First block address */
	struct ftl_addr				addr;

	/* Number of processed blocks */
	size_t					pos;

	/* Number of blocks */
	size_t					num_blocks;

	/* IO vector pointer */
	struct iovec				*iov;

	/* IO vector buffer for internal requests */
	struct iovec				iov_buf[FTL_IO_MAX_IOVEC];

	/* Metadata */
	void					*md;

	/* Number of IO vectors */
	size_t					iov_cnt;

	/* Position within the iovec */
	size_t					iov_pos;

	/* Offset within the iovec (in blocks) */
	size_t					iov_off;

	/* Transfer batch (valid only for writes going through the write buffer) */
	struct ftl_batch			*batch;

	/* Band this IO is being written to */
	struct ftl_band				*band;

	/* Request status */
	int					status;

	/* Number of split requests */
	size_t					req_cnt;

	/* Callback's function */
	ftl_io_fn				cb_fn;

	/* Callback's context */
	void					*cb_ctx;

	/* User callback function */
	spdk_ftl_fn				user_fn;

	/* Flags */
	int					flags;

	/* IO type */
	enum ftl_io_type			type;

	/* Done flag */
	bool					done;

	/* Parent request */
	struct ftl_io				*parent;
	/* Child requests list */
	LIST_HEAD(, ftl_io)			children;
	/* Child list link */
	LIST_ENTRY(ftl_io)			child_entry;
	/* Children lock */
	pthread_spinlock_t			lock;

	/* Trace group id */
	uint64_t				trace;

	/* Used by retry and write completion queues */
	TAILQ_ENTRY(ftl_io)			ioch_entry;
};

/* Metadata IO */
struct ftl_md_io {
	/* Parent IO structure */
	struct ftl_io				io;

	/* Serialization/deserialization callback */
	ftl_md_pack_fn				pack_fn;

	/* Callback's function */
	ftl_io_fn				cb_fn;

	/* Callback's context */
	void					*cb_ctx;
};

static inline bool
ftl_io_mode_physical(const struct ftl_io *io)
{
	return io->flags & FTL_IO_PHYSICAL_MODE;
}

static inline bool
ftl_io_mode_logical(const struct ftl_io *io)
{
	return !ftl_io_mode_physical(io);
}

static inline bool
ftl_io_done(const struct ftl_io *io)
{
	return io->req_cnt == 0 && io->pos == io->num_blocks;
}

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
struct ftl_io *ftl_io_wbuf_init(struct spdk_ftl_dev *dev, struct ftl_addr addr,
				struct ftl_band *band, struct ftl_batch *batch, ftl_io_fn cb);
struct ftl_io *ftl_io_erase_init(struct ftl_band *band, size_t num_blocks, ftl_io_fn cb);
struct ftl_io *ftl_io_user_init(struct spdk_io_channel *ioch, uint64_t lba, size_t num_blocks,
				struct iovec *iov, size_t iov_cnt, spdk_ftl_fn cb_fn,
				void *cb_arg, int type);
void *ftl_io_get_md(const struct ftl_io *io);
void ftl_io_complete(struct ftl_io *io);
void ftl_io_shrink_iovec(struct ftl_io *io, size_t num_blocks);
void ftl_io_process_error(struct ftl_io *io, const struct spdk_nvme_cpl *status);
void ftl_io_reset(struct ftl_io *io);
void ftl_io_call_foreach_child(struct ftl_io *io, int (*callback)(struct ftl_io *));

#endif /* FTL_IO_H */
