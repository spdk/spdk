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

#ifndef OCSSD_IO_H
#define OCSSD_IO_H

#include <spdk/stdinc.h>
#include <spdk/nvme.h>
#include <spdk/ocssd.h>
#include "ocssd_utils.h"
#include "ocssd_ppa.h"
#include "ocssd_trace.h"

struct ocssd_dev;
struct ocssd_rwb_batch;
struct ocssd_band;
struct ocssd_io;

/* IO flags */
enum ocssd_io_flags {
	/* Indicates whether IO is already initialized */
	OCSSD_IO_INITIALIZED	= (1 << 0),
	/* Free the IO when done with the request */
	OCSSD_IO_MEMORY		= (1 << 1),
	/* Internal based IO (defrag, metadata etc.) */
	OCSSD_IO_INTERNAL	= (1 << 2),
	/* Indicates that the IO should not go through if there's */
	/* already another one scheduled to the same LBA */
	OCSSD_IO_WEAK		= (1 << 3),
	/* Indicates that the IO is used for padding */
	OCSSD_IO_PAD		= (1 << 4),
	/* The IO operates on metadata */
	OCSSD_IO_MD		= (1 << 5),
	/* Using PPA instead of LBA */
	OCSSD_IO_PPA_MODE	= (1 << 6),
	/* Indicates that IO contains noncontiguous LBAs */
	OCSSD_IO_VECTOR_LBA	= (1 << 7),
};

enum ocssd_io_type {
	OCSSD_IO_READ,
	OCSSD_IO_WRITE,
	OCSSD_IO_ERASE,
};

struct ocssd_io_init_opts {
	struct ocssd_dev                        *dev;

	/* IO descriptor */
	struct ocssd_io				*io;

	/* Size of IO descriptor */
	size_t                                  size;

	/* IO flags */
	int                                     flags;

	/* IO type */
	enum ocssd_io_type			type;

	/* Number of split requests */
	size_t                                  iov_cnt;

	/* RWB entry */
	struct ocssd_rwb_batch			*rwb_batch;

	/* Band to which the IO is directed */
	struct ocssd_band			*band;

	/* Request size */
	size_t                                  req_size;

	/* Data */
	void                                    *data;

	/* Metadata */
	void                                    *md;

	/* Callback */
	ocssd_fn                                fn;
};

/* General IO descriptor */
struct ocssd_io {
	/* Device */
	struct ocssd_dev			*dev;

	union {
		/* LBA table */
		uint64_t			*lbas;

		/* First LBA */
		uint64_t			lba;
	};

	/* First PPA */
	struct ocssd_ppa			ppa;

	/* Number of processed lbks */
	size_t					pos;

	/* Number of lbks */
	size_t					lbk_cnt;

	union {
		/* IO vector table */
		struct iovec			*iovs;

		/* Single iovec */
		struct iovec			iov;
	};

	/* Metadata */
	void					*md;

	/* Number of IO vectors */
	size_t					iov_cnt;

	/* Position within the iovec */
	size_t					iov_pos;

	/* Offset within the iovec (in lbks) */
	size_t					iov_off;

	/* RWB entry (valid only for RWB-based IO) */
	struct ocssd_rwb_batch			*rwb_batch;

	/* Band this IO is being written to */
	struct ocssd_band			*band;

	/* Request status */
	int					status;

	/* Number of split requests */
	size_t					req_cnt;

	/* Completion callback */
	struct ocssd_cb				cb;

	/* Flags */
	int					flags;

	/* IO type */
	enum ocssd_io_type			type;

	/* Trace group id */
	ocssd_trace_group_t			trace;
};

#define ocssd_io_clear(io) \
	do { \
		((struct ocssd_io *)(io))->pos = 0; \
		((struct ocssd_io *)(io))->req_cnt = 0; \
		((struct ocssd_io *)(io))->iov_pos = 0; \
		((struct ocssd_io *)(io))->iov_off = 0; \
		((struct ocssd_io *)(io))->flags = 0; \
		((struct ocssd_io *)(io))->rwb_batch = NULL; \
		((struct ocssd_io *)(io))->band = NULL; \
	} while (0)

#define ocssd_io_set_flags(io, _flags) \
	(((struct ocssd_io *)(io))->flags |= (_flags))

#define ocssd_io_clear_flags(io, _flags) \
	(((struct ocssd_io *)(io))->flags &= ~(_flags))

#define ocssd_io_check_flags(io, _flags) \
	(!!(((struct ocssd_io *)(io))->flags & (_flags)))

#define ocssd_io_initialized(io) \
	ocssd_io_check_flags(io, OCSSD_IO_INITIALIZED)

#define ocssd_io_internal(io) \
	ocssd_io_check_flags(io, OCSSD_IO_INTERNAL)

#define ocssd_io_weak(io) \
	ocssd_io_check_flags(io, OCSSD_IO_WEAK)

#define ocssd_io_mem_free(io) \
	ocssd_io_check_flags(io, OCSSD_IO_MEMORY)

#define ocssd_io_md(io) \
	ocssd_io_check_flags(io, OCSSD_IO_MD)

#define ocssd_io_vector_lba(io) \
	ocssd_io_check_flags(io, OCSSD_IO_VECTOR_LBA)

#define ocssd_io_mode_ppa(io) \
	ocssd_io_check_flags(io, OCSSD_IO_PPA_MODE)

#define ocssd_io_mode_lba(io) \
	(!ocssd_io_mode_ppa(io))

#define ocssd_io_set_type(io, _type) \
	(((struct ocssd_io *)(io))->type = _type)

#define ocssd_io_get_type(io) \
	(((struct ocssd_io *)(io))->type)

#define ocssd_io_done(io) \
	(!((struct ocssd_io *)(io))->req_cnt)

struct ocssd_io *ocssd_io_init_internal(const struct ocssd_io_init_opts *opts);
void	ocssd_io_reinit(struct ocssd_io *io, ocssd_fn cb,
			void *ctx, int flags, int type);
size_t	ocssd_io_inc_req(struct ocssd_io *io);
size_t	ocssd_io_dec_req(struct ocssd_io *io);
struct iovec *ocssd_io_iovec(struct ocssd_io *io);
uint64_t ocssd_io_current_lba(struct ocssd_io *io);
void	ocssd_io_update_iovec(struct ocssd_io *io, size_t lbk_cnt);
size_t	ocssd_iovec_num_lbks(struct iovec *iov, size_t iov_cnt);
void	*ocssd_io_iovec_addr(struct ocssd_io *io);
size_t	ocssd_io_iovec_len_left(struct ocssd_io *io);
int	ocssd_io_init_iovec(struct ocssd_io *io, void *buf,
			    size_t iov_cnt, size_t req_size);
void	ocssd_io_init(struct ocssd_io *io, struct ocssd_dev *dev,
		      ocssd_fn cb, void *ctx, int flags, int type);
struct ocssd_io *ocssd_io_init_internal(const struct ocssd_io_init_opts *opts);
struct ocssd_io *ocssd_io_rwb_init(struct ocssd_dev *dev, struct ocssd_band *band,
				   struct ocssd_rwb_batch *entry, ocssd_fn cb);
struct ocssd_io	*ocssd_io_erase_init(struct ocssd_band *band, size_t lbk_cnt, ocssd_fn cb);
void	ocssd_io_user_init(struct ocssd_io *io, uint64_t lba, size_t lbk_cnt,
			   struct iovec *iov, size_t iov_cnt,
			   const struct ocssd_cb *cb, int type);
void	*ocssd_io_get_md(const struct ocssd_io *io);
void	ocssd_io_complete(struct ocssd_io *io);
void	ocssd_io_process_error(struct ocssd_io *io, const struct spdk_nvme_cpl *status);

#endif /* OCSSD_IO_H */
