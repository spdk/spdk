/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
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
};

enum ftl_io_type {
	FTL_IO_READ,
	FTL_IO_WRITE,
	FTL_IO_UNMAP,
};

#define FTL_IO_MAX_IOVEC 4

/* General IO descriptor */
struct ftl_io {
	/* Device */
	struct spdk_ftl_dev		*dev;

	/* IO channel */
	struct spdk_io_channel		*ioch;

	/* LBA address */
	uint64_t			lba;

	/* First address of write when sent to cache device */
	ftl_addr			addr;

	/* Number of processed blocks */
	size_t				pos;

	/* Number of blocks */
	size_t				num_blocks;

	/* IO vector pointer */
	struct iovec			*iov;

	/* Metadata */
	void				*md;

	/* Number of IO vectors */
	size_t				iov_cnt;

	/* Position within the io vector array */
	size_t				iov_pos;

	/* Offset within the iovec (in blocks) */
	size_t				iov_off;

	/* Request status */
	int				status;

	/* Number of split requests */
	size_t				req_cnt;

	/* Callback's context */
	void				*cb_ctx;

	/* User callback function */
	spdk_ftl_fn			user_fn;

	/* Flags */
	int				flags;

	/* IO type */
	enum ftl_io_type		type;

	/* Done flag */
	bool				done;

	/* Used by retry and write completion queues */
	TAILQ_ENTRY(ftl_io)		queue_entry;

	/* Reference to the chunk within NV cache */
	struct ftl_nv_cache_chunk	*nv_cache_chunk;

	/* Logical to physical mapping for this IO, number of entries equals to
	 * number of transfer blocks */
	ftl_addr			*map;

	struct spdk_bdev_io_wait_entry	bdev_io_wait;
};

void ftl_io_fail(struct ftl_io *io, int status);
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
int ftl_io_init(struct spdk_io_channel *ioch, struct ftl_io *io, uint64_t lba,
		size_t num_blocks, struct iovec *iov, size_t iov_cnt, spdk_ftl_fn cb_fn,
		void *cb_arg, int type);
void ftl_io_complete(struct ftl_io *io);

static inline bool
ftl_io_done(const struct ftl_io *io)
{
	return io->req_cnt == 0 && io->pos == io->num_blocks;
}

#endif /* FTL_IO_H */
