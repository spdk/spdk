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

#include "ftl_ppa.h"
#include "ftl_trace.h"

struct spdk_ftl_dev;
struct ftl_rwb_batch;
struct ftl_band;
struct ftl_io;
struct ftl_md;

typedef int (*ftl_md_pack_fn)(struct spdk_ftl_dev *, struct ftl_md *, void *);

/* IO flags */
enum ftl_io_flags {
	/* Indicates whether IO is already initialized */
	FTL_IO_INITIALIZED	= (1 << 0),
	/* Keep the IO when done with the request */
	FTL_IO_KEEP_ALIVE	= (1 << 1),
	/* Internal based IO (defrag, metadata etc.) */
	FTL_IO_INTERNAL		= (1 << 2),
	/* Indicates that the IO should not go through if there's */
	/* already another one scheduled to the same LBA */
	FTL_IO_WEAK		= (1 << 3),
	/* Indicates that the IO is used for padding */
	FTL_IO_PAD		= (1 << 4),
	/* The IO operates on metadata */
	FTL_IO_MD		= (1 << 5),
	/* Using PPA instead of LBA */
	FTL_IO_PPA_MODE		= (1 << 6),
	/* Indicates that IO contains noncontiguous LBAs */
	FTL_IO_VECTOR_LBA	= (1 << 7),
	/* Indicates that IO is being retried */
	FTL_IO_RETRY		= (1 << 8),
};

enum ftl_io_type {
	FTL_IO_READ,
	FTL_IO_WRITE,
	FTL_IO_ERASE,
};

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

	/* Number of split requests */
	size_t                                  iov_cnt;

	/* RWB entry */
	struct ftl_rwb_batch			*rwb_batch;

	/* Band to which the IO is directed */
	struct ftl_band				*band;

	/* Request size */
	size_t                                  req_size;

	/* Data */
	void                                    *data;

	/* Metadata */
	void                                    *md;

	/* Callback */
	spdk_ftl_fn				fn;
};

struct ftl_cb {
	/* Callback function */
	spdk_ftl_fn				fn;

	/* Callback's context */
	void					*ctx;
};

struct ftl_io_channel {
	/* IO pool element size */
	size_t					elem_size;

	/* IO pool */
	struct spdk_mempool			*io_pool;
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

	/* First PPA */
	struct ftl_ppa				ppa;

	/* Number of processed lbks */
	size_t					pos;

	/* Number of lbks */
	size_t					lbk_cnt;

	union {
		/* IO vector table */
		struct iovec			*vector;

		/* Single iovec */
		struct iovec			single;
	} iov;

	/* Metadata */
	void					*md;

	/* Number of IO vectors */
	size_t					iov_cnt;

	/* Position within the iovec */
	size_t					iov_pos;

	/* Offset within the iovec (in lbks) */
	size_t					iov_off;

	/* RWB entry (valid only for RWB-based IO) */
	struct ftl_rwb_batch			*rwb_batch;

	/* Band this IO is being written to */
	struct ftl_band				*band;

	/* Request status */
	int					status;

	/* Number of split requests */
	size_t					req_cnt;

	/* Completion callback */
	struct ftl_cb				cb;

	/* Flags */
	int					flags;

	/* IO type */
	enum ftl_io_type			type;

	/* Done flag */
	bool					done;
	/* Children lock */
	pthread_spinlock_t			lock;

	/* Parent request */
	struct ftl_io				*parent;
	/* Child requests */
	LIST_HEAD(, ftl_io)			children;
	/* Child list link */
	LIST_ENTRY(ftl_io)			child_entry;

	/* Trace group id */
	uint64_t				trace;

	unsigned int				child_outstanding;

	TAILQ_ENTRY(ftl_io)			retry_entry;
};

/* Metadata IO */
struct ftl_md_io {
	/* Parent IO structure */
	struct ftl_io				io;

	/* Destination metadata pointer */
	struct ftl_md				*md;

	/* Metadata's buffer */
	void					*buf;

	/* Serialization/deserialization callback */
	ftl_md_pack_fn				pack_fn;

	/* User's callback */
	struct ftl_cb				cb;
};

static inline bool
ftl_io_mode_ppa(const struct ftl_io *io)
{
	return io->flags & FTL_IO_PPA_MODE;
}

static inline bool
ftl_io_mode_lba(const struct ftl_io *io)
{
	return !ftl_io_mode_ppa(io);
}

static inline bool
ftl_io_done(struct ftl_io *io)
{
	return io->req_cnt == 0 && !(io->flags & FTL_IO_RETRY);
}

struct ftl_io *ftl_io_alloc(struct spdk_io_channel *ch);
struct ftl_io *ftl_io_alloc_child(struct ftl_io *parent);
void ftl_io_free(struct ftl_io *io);
struct ftl_io *ftl_io_init_internal(const struct ftl_io_init_opts *opts);
void ftl_io_reinit(struct ftl_io *io, spdk_ftl_fn cb,
		   void *ctx, int flags, int type);
void ftl_io_clear(struct ftl_io *io);
void ftl_io_inc_req(struct ftl_io *io);
void ftl_io_dec_req(struct ftl_io *io);
struct iovec *ftl_io_iovec(struct ftl_io *io);
uint64_t ftl_io_current_lba(const struct ftl_io *io);
uint64_t ftl_io_next_lba(const struct ftl_io *io, size_t offset);
void ftl_io_update_iovec(struct ftl_io *io, size_t lbk_cnt);
size_t ftl_iovec_num_lbks(struct iovec *iov, size_t iov_cnt);
void *ftl_io_iovec_addr(struct ftl_io *io);
size_t ftl_io_iovec_len_left(struct ftl_io *io);
int ftl_io_init_iovec(struct ftl_io *io, void *buf,
		      size_t iov_cnt, size_t req_size);
struct ftl_io *ftl_io_init_internal(const struct ftl_io_init_opts *opts);
struct ftl_io *ftl_io_rwb_init(struct spdk_ftl_dev *dev, struct ftl_band *band,
			       struct ftl_rwb_batch *entry, spdk_ftl_fn cb);
struct ftl_io *ftl_io_erase_init(struct ftl_band *band, size_t lbk_cnt, spdk_ftl_fn cb);
void ftl_io_user_init(struct spdk_ftl_dev *dev, struct ftl_io *io, uint64_t lba, size_t lbk_cnt,
		      struct iovec *iov, size_t iov_cnt,
		      spdk_ftl_fn fn, void *cb_arg, int type);
void *ftl_io_get_md(const struct ftl_io *io);
void ftl_io_complete(struct ftl_io *io);
bool ftl_io_done(struct ftl_io *io);
void ftl_io_process_error(struct ftl_io *io, const struct spdk_nvme_cpl *status);

#endif /* FTL_IO_H */
