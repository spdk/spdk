/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "bdev_raid.h"

#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/likely.h"
#include "spdk/log.h"
#include "spdk/xor.h"

/* Maximum concurrent full stripe writes per io channel */
#define RAID5F_MAX_STRIPES 32

struct chunk {
	/* Corresponds to base_bdev index */
	uint8_t index;

	/* Array of iovecs */
	struct iovec *iovs;

	/* Number of used iovecs */
	int iovcnt;

	/* Total number of available iovecs in the array */
	int iovcnt_max;

	/* Pointer to buffer with I/O metadata */
	void *md_buf;

	/* Shallow copy of IO request parameters */
	struct spdk_bdev_ext_io_opts ext_opts;
};

struct stripe_request {
	struct raid5f_io_channel *r5ch;

	/* The associated raid_bdev_io */
	struct raid_bdev_io *raid_io;

	/* The stripe's index in the raid array. */
	uint64_t stripe_index;

	/* The stripe's parity chunk */
	struct chunk *parity_chunk;

	/* Buffer for stripe parity */
	void *parity_buf;

	/* Buffer for stripe io metadata parity */
	void *parity_md_buf;

	TAILQ_ENTRY(stripe_request) link;

	/* Array of chunks corresponding to base_bdevs */
	struct chunk chunks[0];
};

struct raid5f_info {
	/* The parent raid bdev */
	struct raid_bdev *raid_bdev;

	/* Number of data blocks in a stripe (without parity) */
	uint64_t stripe_blocks;

	/* Number of stripes on this array */
	uint64_t total_stripes;

	/* Alignment for buffer allocation */
	size_t buf_alignment;
};

struct raid5f_io_channel {
	/* All available stripe requests on this channel */
	TAILQ_HEAD(, stripe_request) free_stripe_requests;

	/* Array of iovec iterators for each data chunk */
	struct iov_iter {
		struct iovec *iovs;
		int iovcnt;
		int index;
		size_t offset;
	} *chunk_iov_iters;

	/* Array of source buffer pointers for parity calculation */
	void **chunk_xor_buffers;

	/* Array of source buffer pointers for parity calculation of io metadata */
	void **chunk_xor_md_buffers;

	/* Bounce buffers for parity calculation in case of unaligned source buffers */
	struct iovec *chunk_xor_bounce_buffers;
};

#define __CHUNK_IN_RANGE(req, c) \
	c < req->chunks + raid5f_ch_to_r5f_info(req->r5ch)->raid_bdev->num_base_bdevs

#define FOR_EACH_CHUNK_FROM(req, c, from) \
	for (c = from; __CHUNK_IN_RANGE(req, c); c++)

#define FOR_EACH_CHUNK(req, c) \
	FOR_EACH_CHUNK_FROM(req, c, req->chunks)

#define __NEXT_DATA_CHUNK(req, c) \
	c == req->parity_chunk ? c+1 : c

#define FOR_EACH_DATA_CHUNK(req, c) \
	for (c = __NEXT_DATA_CHUNK(req, req->chunks); __CHUNK_IN_RANGE(req, c); \
	     c = __NEXT_DATA_CHUNK(req, c+1))

static inline struct raid5f_info *
raid5f_ch_to_r5f_info(struct raid5f_io_channel *r5ch)
{
	return spdk_io_channel_get_io_device(spdk_io_channel_from_ctx(r5ch));
}

static inline struct stripe_request *
raid5f_chunk_stripe_req(struct chunk *chunk)
{
	return SPDK_CONTAINEROF((chunk - chunk->index), struct stripe_request, chunks);
}

static inline uint8_t
raid5f_stripe_data_chunks_num(const struct raid_bdev *raid_bdev)
{
	return raid_bdev->min_base_bdevs_operational;
}

static inline uint8_t
raid5f_stripe_parity_chunk_index(const struct raid_bdev *raid_bdev, uint64_t stripe_index)
{
	return raid5f_stripe_data_chunks_num(raid_bdev) - stripe_index % raid_bdev->num_base_bdevs;
}

static inline void
raid5f_stripe_request_release(struct stripe_request *stripe_req)
{
	TAILQ_INSERT_HEAD(&stripe_req->r5ch->free_stripe_requests, stripe_req, link);
}

static int
raid5f_xor_stripe(struct stripe_request *stripe_req)
{
	struct raid_bdev_io *raid_io = stripe_req->raid_io;
	struct raid5f_io_channel *r5ch = stripe_req->r5ch;
	struct raid_bdev *raid_bdev = raid_io->raid_bdev;
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(raid_io);
	size_t remaining = raid_bdev->strip_size << raid_bdev->blocklen_shift;
	uint8_t n_src = raid5f_stripe_data_chunks_num(raid_bdev);
	void *dest = stripe_req->parity_buf;
	size_t alignment_mask = spdk_xor_get_optimal_alignment() - 1;
	void *raid_md = spdk_bdev_io_get_md_buf(bdev_io);
	uint32_t raid_md_size = spdk_bdev_get_md_size(&raid_bdev->bdev);
	struct chunk *chunk;
	int ret;
	uint8_t c;

	c = 0;
	FOR_EACH_DATA_CHUNK(stripe_req, chunk) {
		struct iov_iter *iov_iter = &r5ch->chunk_iov_iters[c];
		bool aligned = true;
		int i;

		for (i = 0; i < chunk->iovcnt; i++) {
			if (((uintptr_t)chunk->iovs[i].iov_base & alignment_mask) ||
			    (chunk->iovs[i].iov_len & alignment_mask)) {
				aligned = false;
				break;
			}
		}

		if (aligned) {
			iov_iter->iovs = chunk->iovs;
			iov_iter->iovcnt = chunk->iovcnt;
		} else {
			iov_iter->iovs = &r5ch->chunk_xor_bounce_buffers[c];
			iov_iter->iovcnt = 1;
			spdk_iovcpy(chunk->iovs, chunk->iovcnt, iov_iter->iovs, iov_iter->iovcnt);
		}

		iov_iter->index = 0;
		iov_iter->offset = 0;

		c++;
	}

	while (remaining > 0) {
		size_t len = remaining;
		uint8_t i;

		for (i = 0; i < n_src; i++) {
			struct iov_iter *iov_iter = &r5ch->chunk_iov_iters[i];
			struct iovec *iov = &iov_iter->iovs[iov_iter->index];

			len = spdk_min(len, iov->iov_len - iov_iter->offset);
			r5ch->chunk_xor_buffers[i] = iov->iov_base + iov_iter->offset;
		}

		assert(len > 0);

		ret = spdk_xor_gen(dest, r5ch->chunk_xor_buffers, n_src, len);
		if (spdk_unlikely(ret)) {
			SPDK_ERRLOG("stripe xor failed\n");
			return ret;
		}

		for (i = 0; i < n_src; i++) {
			struct iov_iter *iov_iter = &r5ch->chunk_iov_iters[i];
			struct iovec *iov = &iov_iter->iovs[iov_iter->index];

			iov_iter->offset += len;
			if (iov_iter->offset == iov->iov_len) {
				iov_iter->offset = 0;
				iov_iter->index++;
			}
		}
		dest += len;

		remaining -= len;
	}

	if (raid_md != NULL) {
		uint64_t len = raid_bdev->strip_size * raid_md_size;
		c = 0;
		FOR_EACH_DATA_CHUNK(stripe_req, chunk) {
			r5ch->chunk_xor_md_buffers[c] = chunk->md_buf;
			c++;
		}
		ret = spdk_xor_gen(stripe_req->parity_md_buf, r5ch->chunk_xor_md_buffers, n_src, len);
		if (spdk_unlikely(ret)) {
			SPDK_ERRLOG("stripe io metadata xor failed\n");
			return ret;
		}
	}

	return 0;
}

static void
raid5f_chunk_write_complete(struct chunk *chunk, enum spdk_bdev_io_status status)
{
	struct stripe_request *stripe_req = raid5f_chunk_stripe_req(chunk);

	if (raid_bdev_io_complete_part(stripe_req->raid_io, 1, status)) {
		raid5f_stripe_request_release(stripe_req);
	}
}

static void
raid5f_chunk_write_complete_bdev_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct chunk *chunk = cb_arg;

	spdk_bdev_free_io(bdev_io);

	raid5f_chunk_write_complete(chunk, success ? SPDK_BDEV_IO_STATUS_SUCCESS :
				    SPDK_BDEV_IO_STATUS_FAILED);
}

static void raid5f_stripe_request_submit_chunks(struct stripe_request *stripe_req);

static void
raid5f_chunk_write_retry(void *_raid_io)
{
	struct raid_bdev_io *raid_io = _raid_io;
	struct stripe_request *stripe_req = raid_io->module_private;

	raid5f_stripe_request_submit_chunks(stripe_req);
}

static inline void
raid5f_init_ext_io_opts(struct spdk_bdev_io *bdev_io, struct spdk_bdev_ext_io_opts *opts)
{
	memset(opts, 0, sizeof(*opts));
	opts->size = sizeof(*opts);
	opts->memory_domain = bdev_io->u.bdev.memory_domain;
	opts->memory_domain_ctx = bdev_io->u.bdev.memory_domain_ctx;
	opts->metadata = bdev_io->u.bdev.md_buf;
}

static int
raid5f_chunk_write(struct chunk *chunk)
{
	struct stripe_request *stripe_req = raid5f_chunk_stripe_req(chunk);
	struct raid_bdev_io *raid_io = stripe_req->raid_io;
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(raid_io);
	struct raid_bdev *raid_bdev = raid_io->raid_bdev;
	struct raid_base_bdev_info *base_info = &raid_bdev->base_bdev_info[chunk->index];
	struct spdk_io_channel *base_ch = raid_io->raid_ch->base_channel[chunk->index];
	uint64_t base_offset_blocks = (stripe_req->stripe_index << raid_bdev->strip_size_shift);
	int ret;

	raid5f_init_ext_io_opts(bdev_io, &chunk->ext_opts);
	chunk->ext_opts.metadata = chunk->md_buf;

	ret = spdk_bdev_writev_blocks_ext(base_info->desc, base_ch, chunk->iovs, chunk->iovcnt,
					  base_offset_blocks, raid_bdev->strip_size, raid5f_chunk_write_complete_bdev_io,
					  chunk, &chunk->ext_opts);

	if (spdk_unlikely(ret)) {
		if (ret == -ENOMEM) {
			raid_bdev_queue_io_wait(raid_io, base_info->bdev, base_ch,
						raid5f_chunk_write_retry);
		} else {
			/*
			 * Implicitly complete any I/Os not yet submitted as FAILED. If completing
			 * these means there are no more to complete for the stripe request, we can
			 * release the stripe request as well.
			 */
			uint64_t base_bdev_io_not_submitted = raid_bdev->num_base_bdevs -
							      raid_io->base_bdev_io_submitted;

			if (raid_bdev_io_complete_part(stripe_req->raid_io, base_bdev_io_not_submitted,
						       SPDK_BDEV_IO_STATUS_FAILED)) {
				raid5f_stripe_request_release(stripe_req);
			}
		}
	}

	return ret;
}

static int
raid5f_stripe_request_map_iovecs(struct stripe_request *stripe_req)
{
	struct raid_bdev *raid_bdev = stripe_req->raid_io->raid_bdev;
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(stripe_req->raid_io);
	const struct iovec *raid_io_iovs = bdev_io->u.bdev.iovs;
	int raid_io_iovcnt = bdev_io->u.bdev.iovcnt;
	void *raid_io_md = spdk_bdev_io_get_md_buf(bdev_io);
	uint32_t raid_io_md_size = spdk_bdev_get_md_size(&raid_bdev->bdev);
	struct chunk *chunk;
	int raid_io_iov_idx = 0;
	size_t raid_io_offset = 0;
	size_t raid_io_iov_offset = 0;
	int i;

	FOR_EACH_DATA_CHUNK(stripe_req, chunk) {
		int chunk_iovcnt = 0;
		uint64_t len = raid_bdev->strip_size << raid_bdev->blocklen_shift;
		size_t off = raid_io_iov_offset;

		for (i = raid_io_iov_idx; i < raid_io_iovcnt; i++) {
			chunk_iovcnt++;
			off += raid_io_iovs[i].iov_len;
			if (off >= raid_io_offset + len) {
				break;
			}
		}

		assert(raid_io_iov_idx + chunk_iovcnt <= raid_io_iovcnt);

		if (chunk_iovcnt > chunk->iovcnt_max) {
			struct iovec *iovs = chunk->iovs;

			iovs = realloc(iovs, chunk_iovcnt * sizeof(*iovs));
			if (!iovs) {
				return -ENOMEM;
			}
			chunk->iovs = iovs;
			chunk->iovcnt_max = chunk_iovcnt;
		}
		chunk->iovcnt = chunk_iovcnt;

		if (raid_io_md) {
			chunk->md_buf = raid_io_md +
					(raid_io_offset >> raid_bdev->blocklen_shift) * raid_io_md_size;
		}

		for (i = 0; i < chunk_iovcnt; i++) {
			struct iovec *chunk_iov = &chunk->iovs[i];
			const struct iovec *raid_io_iov = &raid_io_iovs[raid_io_iov_idx];
			size_t chunk_iov_offset = raid_io_offset - raid_io_iov_offset;

			chunk_iov->iov_base = raid_io_iov->iov_base + chunk_iov_offset;
			chunk_iov->iov_len = spdk_min(len, raid_io_iov->iov_len - chunk_iov_offset);
			raid_io_offset += chunk_iov->iov_len;
			len -= chunk_iov->iov_len;

			if (raid_io_offset >= raid_io_iov_offset + raid_io_iov->iov_len) {
				raid_io_iov_idx++;
				raid_io_iov_offset += raid_io_iov->iov_len;
			}
		}

		if (spdk_unlikely(len > 0)) {
			return -EINVAL;
		}
	}

	stripe_req->parity_chunk->iovs[0].iov_base = stripe_req->parity_buf;
	stripe_req->parity_chunk->iovs[0].iov_len = raid_bdev->strip_size <<
			raid_bdev->blocklen_shift;
	stripe_req->parity_chunk->md_buf = stripe_req->parity_md_buf;
	stripe_req->parity_chunk->iovcnt = 1;

	return 0;
}

static void
raid5f_stripe_request_submit_chunks(struct stripe_request *stripe_req)
{
	struct raid_bdev_io *raid_io = stripe_req->raid_io;
	struct chunk *start = &stripe_req->chunks[raid_io->base_bdev_io_submitted];
	struct chunk *chunk;

	FOR_EACH_CHUNK_FROM(stripe_req, chunk, start) {
		if (spdk_unlikely(raid5f_chunk_write(chunk) != 0)) {
			break;
		}
		raid_io->base_bdev_io_submitted++;
	}
}

static void
raid5f_submit_stripe_request(struct stripe_request *stripe_req)
{
	if (spdk_unlikely(raid5f_xor_stripe(stripe_req) != 0)) {
		raid_bdev_io_complete(stripe_req->raid_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	raid5f_stripe_request_submit_chunks(stripe_req);
}

static int
raid5f_submit_write_request(struct raid_bdev_io *raid_io, uint64_t stripe_index)
{
	struct raid_bdev *raid_bdev = raid_io->raid_bdev;
	struct raid5f_io_channel *r5ch = spdk_io_channel_get_ctx(raid_io->raid_ch->module_channel);
	struct stripe_request *stripe_req;
	int ret;

	stripe_req = TAILQ_FIRST(&r5ch->free_stripe_requests);
	if (!stripe_req) {
		return -ENOMEM;
	}

	stripe_req->stripe_index = stripe_index;
	stripe_req->parity_chunk = stripe_req->chunks + raid5f_stripe_parity_chunk_index(raid_bdev,
				   stripe_req->stripe_index);
	stripe_req->raid_io = raid_io;

	ret = raid5f_stripe_request_map_iovecs(stripe_req);
	if (spdk_unlikely(ret)) {
		return ret;
	}

	TAILQ_REMOVE(&r5ch->free_stripe_requests, stripe_req, link);

	raid_io->module_private = stripe_req;
	raid_io->base_bdev_io_remaining = raid_bdev->num_base_bdevs;

	raid5f_submit_stripe_request(stripe_req);

	return 0;
}

static void
raid5f_chunk_read_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid_bdev_io *raid_io = cb_arg;

	spdk_bdev_free_io(bdev_io);

	raid_bdev_io_complete(raid_io, success ? SPDK_BDEV_IO_STATUS_SUCCESS :
			      SPDK_BDEV_IO_STATUS_FAILED);
}

static void raid5f_submit_rw_request(struct raid_bdev_io *raid_io);

static void
_raid5f_submit_rw_request(void *_raid_io)
{
	struct raid_bdev_io *raid_io = _raid_io;

	raid5f_submit_rw_request(raid_io);
}

static int
raid5f_submit_read_request(struct raid_bdev_io *raid_io, uint64_t stripe_index,
			   uint64_t stripe_offset)
{
	struct raid_bdev *raid_bdev = raid_io->raid_bdev;
	uint8_t chunk_data_idx = stripe_offset >> raid_bdev->strip_size_shift;
	uint8_t p_idx = raid5f_stripe_parity_chunk_index(raid_bdev, stripe_index);
	uint8_t chunk_idx = chunk_data_idx < p_idx ? chunk_data_idx : chunk_data_idx + 1;
	struct raid_base_bdev_info *base_info = &raid_bdev->base_bdev_info[chunk_idx];
	struct spdk_io_channel *base_ch = raid_io->raid_ch->base_channel[chunk_idx];
	uint64_t chunk_offset = stripe_offset - (chunk_data_idx << raid_bdev->strip_size_shift);
	uint64_t base_offset_blocks = (stripe_index << raid_bdev->strip_size_shift) + chunk_offset;
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(raid_io);
	struct spdk_bdev_ext_io_opts io_opts;
	int ret;

	raid5f_init_ext_io_opts(bdev_io, &io_opts);
	ret = spdk_bdev_readv_blocks_ext(base_info->desc, base_ch, bdev_io->u.bdev.iovs,
					 bdev_io->u.bdev.iovcnt,
					 base_offset_blocks, bdev_io->u.bdev.num_blocks, raid5f_chunk_read_complete, raid_io,
					 &io_opts);

	if (spdk_unlikely(ret == -ENOMEM)) {
		raid_bdev_queue_io_wait(raid_io, base_info->bdev, base_ch,
					_raid5f_submit_rw_request);
		return 0;
	}

	return ret;
}

static void
raid5f_submit_rw_request(struct raid_bdev_io *raid_io)
{
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(raid_io);
	struct raid_bdev *raid_bdev = raid_io->raid_bdev;
	struct raid5f_info *r5f_info = raid_bdev->module_private;
	uint64_t offset_blocks = bdev_io->u.bdev.offset_blocks;
	uint64_t stripe_index = offset_blocks / r5f_info->stripe_blocks;
	uint64_t stripe_offset = offset_blocks % r5f_info->stripe_blocks;
	int ret;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		assert(bdev_io->u.bdev.num_blocks <= raid_bdev->strip_size);
		ret = raid5f_submit_read_request(raid_io, stripe_index, stripe_offset);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		assert(stripe_offset == 0);
		assert(bdev_io->u.bdev.num_blocks == r5f_info->stripe_blocks);
		ret = raid5f_submit_write_request(raid_io, stripe_index);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	if (spdk_unlikely(ret)) {
		raid_bdev_io_complete(raid_io, ret == -ENOMEM ? SPDK_BDEV_IO_STATUS_NOMEM :
				      SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static void
raid5f_stripe_request_free(struct stripe_request *stripe_req)
{
	struct chunk *chunk;

	FOR_EACH_CHUNK(stripe_req, chunk) {
		free(chunk->iovs);
	}

	spdk_dma_free(stripe_req->parity_buf);
	spdk_dma_free(stripe_req->parity_md_buf);

	free(stripe_req);
}

static struct stripe_request *
raid5f_stripe_request_alloc(struct raid5f_io_channel *r5ch)
{
	struct raid5f_info *r5f_info = raid5f_ch_to_r5f_info(r5ch);
	struct raid_bdev *raid_bdev = r5f_info->raid_bdev;
	uint32_t raid_io_md_size = spdk_bdev_get_md_size(&raid_bdev->bdev);
	struct stripe_request *stripe_req;
	struct chunk *chunk;

	stripe_req = calloc(1, sizeof(*stripe_req) +
			    sizeof(struct chunk) * raid_bdev->num_base_bdevs);
	if (!stripe_req) {
		return NULL;
	}

	stripe_req->r5ch = r5ch;

	FOR_EACH_CHUNK(stripe_req, chunk) {
		chunk->index = chunk - stripe_req->chunks;
		chunk->iovcnt_max = 4;
		chunk->iovs = calloc(chunk->iovcnt_max, sizeof(chunk->iovs[0]));
		if (!chunk->iovs) {
			goto err;
		}
	}

	stripe_req->parity_buf = spdk_dma_malloc(raid_bdev->strip_size << raid_bdev->blocklen_shift,
				 r5f_info->buf_alignment, NULL);
	if (!stripe_req->parity_buf) {
		goto err;
	}

	if (raid_io_md_size != 0) {
		stripe_req->parity_md_buf = spdk_dma_malloc(raid_bdev->strip_size * raid_io_md_size,
					    r5f_info->buf_alignment, NULL);
		if (!stripe_req->parity_md_buf) {
			goto err;
		}
	}

	return stripe_req;
err:
	raid5f_stripe_request_free(stripe_req);
	return NULL;
}

static void
raid5f_ioch_destroy(void *io_device, void *ctx_buf)
{
	struct raid5f_io_channel *r5ch = ctx_buf;
	struct raid5f_info *r5f_info = io_device;
	struct raid_bdev *raid_bdev = r5f_info->raid_bdev;
	struct stripe_request *stripe_req;
	int i;

	while ((stripe_req = TAILQ_FIRST(&r5ch->free_stripe_requests))) {
		TAILQ_REMOVE(&r5ch->free_stripe_requests, stripe_req, link);
		raid5f_stripe_request_free(stripe_req);
	}

	if (r5ch->chunk_xor_bounce_buffers) {
		for (i = 0; i < raid5f_stripe_data_chunks_num(raid_bdev); i++) {
			free(r5ch->chunk_xor_bounce_buffers[i].iov_base);
		}
		free(r5ch->chunk_xor_bounce_buffers);
	}

	free(r5ch->chunk_xor_buffers);
	free(r5ch->chunk_xor_md_buffers);
	free(r5ch->chunk_iov_iters);
}

static int
raid5f_ioch_create(void *io_device, void *ctx_buf)
{
	struct raid5f_io_channel *r5ch = ctx_buf;
	struct raid5f_info *r5f_info = io_device;
	struct raid_bdev *raid_bdev = r5f_info->raid_bdev;
	size_t chunk_len = raid_bdev->strip_size << raid_bdev->blocklen_shift;
	int status = 0;
	int i;

	TAILQ_INIT(&r5ch->free_stripe_requests);

	for (i = 0; i < RAID5F_MAX_STRIPES; i++) {
		struct stripe_request *stripe_req;

		stripe_req = raid5f_stripe_request_alloc(r5ch);
		if (!stripe_req) {
			status = -ENOMEM;
			goto out;
		}

		TAILQ_INSERT_HEAD(&r5ch->free_stripe_requests, stripe_req, link);
	}

	r5ch->chunk_iov_iters = calloc(raid5f_stripe_data_chunks_num(raid_bdev),
				       sizeof(r5ch->chunk_iov_iters[0]));
	if (!r5ch->chunk_iov_iters) {
		status = -ENOMEM;
		goto out;
	}

	r5ch->chunk_xor_buffers = calloc(raid5f_stripe_data_chunks_num(raid_bdev),
					 sizeof(r5ch->chunk_xor_buffers[0]));
	if (!r5ch->chunk_xor_buffers) {
		status = -ENOMEM;
		goto out;
	}

	r5ch->chunk_xor_md_buffers = calloc(raid5f_stripe_data_chunks_num(raid_bdev),
					    sizeof(r5ch->chunk_xor_md_buffers[0]));
	if (!r5ch->chunk_xor_md_buffers) {
		status = -ENOMEM;
		goto out;
	}

	r5ch->chunk_xor_bounce_buffers = calloc(raid5f_stripe_data_chunks_num(raid_bdev),
						sizeof(r5ch->chunk_xor_bounce_buffers[0]));
	if (!r5ch->chunk_xor_bounce_buffers) {
		status = -ENOMEM;
		goto out;
	}

	for (i = 0; i < raid5f_stripe_data_chunks_num(raid_bdev); i++) {
		status = posix_memalign(&r5ch->chunk_xor_bounce_buffers[i].iov_base,
					spdk_xor_get_optimal_alignment(), chunk_len);
		if (status) {
			goto out;
		}
		r5ch->chunk_xor_bounce_buffers[i].iov_len = chunk_len;
	}
out:
	if (status) {
		SPDK_ERRLOG("Failed to initialize io channel\n");
		raid5f_ioch_destroy(r5f_info, r5ch);
	}
	return status;
}

static int
raid5f_start(struct raid_bdev *raid_bdev)
{
	uint64_t min_blockcnt = UINT64_MAX;
	struct raid_base_bdev_info *base_info;
	struct raid5f_info *r5f_info;
	size_t alignment;

	r5f_info = calloc(1, sizeof(*r5f_info));
	if (!r5f_info) {
		SPDK_ERRLOG("Failed to allocate r5f_info\n");
		return -ENOMEM;
	}
	r5f_info->raid_bdev = raid_bdev;

	alignment = spdk_xor_get_optimal_alignment();
	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		min_blockcnt = spdk_min(min_blockcnt, base_info->bdev->blockcnt);
		alignment = spdk_max(alignment, spdk_bdev_get_buf_align(base_info->bdev));
	}

	r5f_info->total_stripes = min_blockcnt / raid_bdev->strip_size;
	r5f_info->stripe_blocks = raid_bdev->strip_size * raid5f_stripe_data_chunks_num(raid_bdev);
	r5f_info->buf_alignment = alignment;

	raid_bdev->bdev.blockcnt = r5f_info->stripe_blocks * r5f_info->total_stripes;
	raid_bdev->bdev.optimal_io_boundary = raid_bdev->strip_size;
	raid_bdev->bdev.split_on_optimal_io_boundary = true;
	raid_bdev->bdev.write_unit_size = r5f_info->stripe_blocks;
	raid_bdev->bdev.split_on_write_unit = true;

	raid_bdev->module_private = r5f_info;

	spdk_io_device_register(r5f_info, raid5f_ioch_create, raid5f_ioch_destroy,
				sizeof(struct raid5f_io_channel), NULL);

	return 0;
}

static void
raid5f_io_device_unregister_done(void *io_device)
{
	struct raid5f_info *r5f_info = io_device;

	raid_bdev_module_stop_done(r5f_info->raid_bdev);

	free(r5f_info);
}

static bool
raid5f_stop(struct raid_bdev *raid_bdev)
{
	struct raid5f_info *r5f_info = raid_bdev->module_private;

	spdk_io_device_unregister(r5f_info, raid5f_io_device_unregister_done);

	return false;
}

static struct spdk_io_channel *
raid5f_get_io_channel(struct raid_bdev *raid_bdev)
{
	struct raid5f_info *r5f_info = raid_bdev->module_private;

	return spdk_get_io_channel(r5f_info);
}

static struct raid_bdev_module g_raid5f_module = {
	.level = RAID5F,
	.base_bdevs_min = 3,
	.base_bdevs_constraint = {CONSTRAINT_MAX_BASE_BDEVS_REMOVED, 1},
	.start = raid5f_start,
	.stop = raid5f_stop,
	.submit_rw_request = raid5f_submit_rw_request,
	.get_io_channel = raid5f_get_io_channel,
};
RAID_MODULE_REGISTER(&g_raid5f_module)

SPDK_LOG_REGISTER_COMPONENT(bdev_raid5f)
