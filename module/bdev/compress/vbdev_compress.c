/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2021, 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "vbdev_compress.h"

#include "spdk/reduce.h"
#include "spdk/stdinc.h"
#include "spdk/rpc.h"
#include "spdk/env.h"
#include "spdk/endian.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/util.h"
#include "spdk/bdev_module.h"
#include "spdk/likely.h"
#include "spdk/log.h"
#include "spdk/accel.h"

#include "spdk/accel_module.h"

#define CHUNK_SIZE (1024 * 16)
#define COMP_BDEV_NAME "compress"
#define BACKING_IO_SZ (4 * 1024)

/* This namespace UUID was generated using uuid_generate() method. */
#define BDEV_COMPRESS_NAMESPACE_UUID "c3fad6da-832f-4cc0-9cdc-5c552b225e7b"

struct vbdev_comp_delete_ctx {
	spdk_delete_compress_complete	cb_fn;
	void				*cb_arg;
	int				cb_rc;
	struct spdk_thread		*orig_thread;
};

/* List of virtual bdevs and associated info for each. */
struct vbdev_compress {
	struct spdk_bdev		*base_bdev;	/* the thing we're attaching to */
	struct spdk_bdev_desc		*base_desc;	/* its descriptor we get from open */
	struct spdk_io_channel		*base_ch;	/* IO channel of base device */
	struct spdk_bdev		comp_bdev;	/* the compression virtual bdev */
	struct comp_io_channel		*comp_ch;	/* channel associated with this bdev */
	struct spdk_io_channel		*accel_channel;	/* to communicate with the accel framework */
	struct spdk_thread		*reduce_thread;
	pthread_mutex_t			reduce_lock;
	uint32_t			ch_count;
	TAILQ_HEAD(, spdk_bdev_io)	pending_comp_ios;	/* outstanding operations to a comp library */
	struct spdk_poller		*poller;	/* completion poller */
	struct spdk_reduce_vol_params	params;		/* params for the reduce volume */
	struct spdk_reduce_backing_dev	backing_dev;	/* backing device info for the reduce volume */
	struct spdk_reduce_vol		*vol;		/* the reduce volume */
	struct vbdev_comp_delete_ctx	*delete_ctx;
	bool				orphaned;	/* base bdev claimed but comp_bdev not registered */
	int				reduce_errno;
	TAILQ_HEAD(, vbdev_comp_op)	queued_comp_ops;
	TAILQ_ENTRY(vbdev_compress)	link;
	struct spdk_thread		*thread;	/* thread where base device is opened */
	enum spdk_accel_comp_algo       comp_algo;      /* compression algorithm for compress bdev */
	uint32_t                        comp_level;     /* compression algorithm level */
	bool				init_failed;	/* compress bdev initialization failed */
};
static TAILQ_HEAD(, vbdev_compress) g_vbdev_comp = TAILQ_HEAD_INITIALIZER(g_vbdev_comp);

/* The comp vbdev channel struct. It is allocated and freed on my behalf by the io channel code.
 */
struct comp_io_channel {
	struct spdk_io_channel_iter	*iter;	/* used with for_each_channel in reset */
};

/* Per I/O context for the compression vbdev. */
struct comp_bdev_io {
	struct comp_io_channel		*comp_ch;		/* used in completion handling */
	struct vbdev_compress		*comp_bdev;		/* vbdev associated with this IO */
	struct spdk_bdev_io_wait_entry	bdev_io_wait;		/* for bdev_io_wait */
	struct spdk_bdev_io		*orig_io;		/* the original IO */
	int				status;			/* save for completion on orig thread */
};

static void vbdev_compress_examine(struct spdk_bdev *bdev);
static int vbdev_compress_claim(struct vbdev_compress *comp_bdev);
struct vbdev_compress *_prepare_for_load_init(struct spdk_bdev_desc *bdev_desc, uint32_t lb_size,
		uint8_t comp_algo, uint32_t comp_level);
static void vbdev_compress_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io);
static void comp_bdev_ch_destroy_cb(void *io_device, void *ctx_buf);
static void vbdev_compress_delete_done(void *cb_arg, int bdeverrno);
static void _comp_reduce_resubmit_backing_io(void *_backing_io);

/* for completing rw requests on the orig IO thread. */
static void
_reduce_rw_blocks_cb(void *arg)
{
	struct comp_bdev_io *io_ctx = arg;

	if (spdk_likely(io_ctx->status == 0)) {
		spdk_bdev_io_complete(io_ctx->orig_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	} else if (io_ctx->status == -ENOMEM) {
		spdk_bdev_io_complete(io_ctx->orig_io, SPDK_BDEV_IO_STATUS_NOMEM);
	} else {
		SPDK_ERRLOG("Failed to execute reduce api. %s\n", spdk_strerror(-io_ctx->status));
		spdk_bdev_io_complete(io_ctx->orig_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

/* Completion callback for r/w that were issued via reducelib. */
static void
reduce_rw_blocks_cb(void *arg, int reduce_errno)
{
	struct spdk_bdev_io *bdev_io = arg;
	struct comp_bdev_io *io_ctx = (struct comp_bdev_io *)bdev_io->driver_ctx;
	struct spdk_io_channel *ch = spdk_io_channel_from_ctx(io_ctx->comp_ch);
	struct spdk_thread *orig_thread;

	/* TODO: need to decide which error codes are bdev_io success vs failure;
	 * example examine calls reading metadata */

	io_ctx->status = reduce_errno;

	/* Send this request to the orig IO thread. */
	orig_thread = spdk_io_channel_get_thread(ch);

	spdk_thread_exec_msg(orig_thread, _reduce_rw_blocks_cb, io_ctx);
}

static int
_compress_operation(struct spdk_reduce_backing_dev *backing_dev, struct iovec *src_iovs,
		    int src_iovcnt, struct iovec *dst_iovs,
		    int dst_iovcnt, bool compress, void *cb_arg)
{
	struct spdk_reduce_vol_cb_args *reduce_cb_arg = cb_arg;
	struct vbdev_compress *comp_bdev = SPDK_CONTAINEROF(backing_dev, struct vbdev_compress,
					   backing_dev);
	int rc;

	if (compress) {
		assert(dst_iovcnt == 1);
		rc = spdk_accel_submit_compress_ext(comp_bdev->accel_channel, dst_iovs[0].iov_base,
						    dst_iovs[0].iov_len, src_iovs, src_iovcnt,
						    comp_bdev->comp_algo, comp_bdev->comp_level,
						    &reduce_cb_arg->output_size, reduce_cb_arg->cb_fn,
						    reduce_cb_arg->cb_arg);
	} else {
		rc = spdk_accel_submit_decompress_ext(comp_bdev->accel_channel, dst_iovs, dst_iovcnt,
						      src_iovs, src_iovcnt, comp_bdev->comp_algo,
						      &reduce_cb_arg->output_size, reduce_cb_arg->cb_fn,
						      reduce_cb_arg->cb_arg);
	}

	return rc;
}

/* Entry point for reduce lib to issue a compress operation. */
static void
_comp_reduce_compress(struct spdk_reduce_backing_dev *dev,
		      struct iovec *src_iovs, int src_iovcnt,
		      struct iovec *dst_iovs, int dst_iovcnt,
		      struct spdk_reduce_vol_cb_args *cb_arg)
{
	int rc;

	rc = _compress_operation(dev, src_iovs, src_iovcnt, dst_iovs, dst_iovcnt, true, cb_arg);
	if (rc) {
		SPDK_ERRLOG("with compress operation code %d (%s)\n", rc, spdk_strerror(-rc));
		cb_arg->cb_fn(cb_arg->cb_arg, rc);
	}
}

/* Entry point for reduce lib to issue a decompress operation. */
static void
_comp_reduce_decompress(struct spdk_reduce_backing_dev *dev,
			struct iovec *src_iovs, int src_iovcnt,
			struct iovec *dst_iovs, int dst_iovcnt,
			struct spdk_reduce_vol_cb_args *cb_arg)
{
	int rc;

	rc = _compress_operation(dev, src_iovs, src_iovcnt, dst_iovs, dst_iovcnt, false, cb_arg);
	if (rc) {
		SPDK_ERRLOG("with decompress operation code %d (%s)\n", rc, spdk_strerror(-rc));
		cb_arg->cb_fn(cb_arg->cb_arg, rc);
	}
}

static void
_comp_submit_write(void *ctx)
{
	struct spdk_bdev_io *bdev_io = ctx;
	struct vbdev_compress *comp_bdev = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_compress,
					   comp_bdev);

	spdk_reduce_vol_writev(comp_bdev->vol, bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
			       bdev_io->u.bdev.offset_blocks, bdev_io->u.bdev.num_blocks,
			       reduce_rw_blocks_cb, bdev_io);
}

static void
_comp_submit_read(void *ctx)
{
	struct spdk_bdev_io *bdev_io = ctx;
	struct vbdev_compress *comp_bdev = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_compress,
					   comp_bdev);

	spdk_reduce_vol_readv(comp_bdev->vol, bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
			      bdev_io->u.bdev.offset_blocks, bdev_io->u.bdev.num_blocks,
			      reduce_rw_blocks_cb, bdev_io);
}


/* Callback for getting a buf from the bdev pool in the event that the caller passed
 * in NULL, we need to own the buffer so it doesn't get freed by another vbdev module
 * beneath us before we're done with it.
 */
static void
comp_read_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io, bool success)
{
	struct vbdev_compress *comp_bdev = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_compress,
					   comp_bdev);

	if (spdk_unlikely(!success)) {
		SPDK_ERRLOG("Failed to get data buffer\n");
		reduce_rw_blocks_cb(bdev_io, -ENOMEM);
		return;
	}

	spdk_thread_exec_msg(comp_bdev->reduce_thread, _comp_submit_read, bdev_io);
}

struct partial_chunk_info {
	uint64_t chunk_idx;
	uint64_t block_offset;
	uint64_t block_length;
};

/*
 * It's a structure used to hold information needed during the execution of an unmap operation.
 */
struct compress_unmap_split_ctx {
	struct spdk_bdev_io *bdev_io;
	int32_t status;
	uint32_t logical_blocks_per_chunk;
	/* The first chunk that can be fully covered by the unmap bdevio interval */
	uint64_t full_chunk_idx_b;
	/* The last chunk that can be fully covered by the unmap bdevio interval */
	uint64_t full_chunk_idx_e;
	uint64_t num_full_chunks;
	uint64_t num_full_chunks_consumed;
	uint32_t num_partial_chunks;
	uint32_t num_partial_chunks_consumed;
	/* Used to hold the partial chunk information. There will only be less than or equal to two,
	because chunks that cannot be fully covered will only appear at the beginning or end or both two. */
	struct partial_chunk_info partial_chunk_info[2];
};

static void _comp_unmap_subcmd_done_cb(void *ctx, int error);

/*
 * This function processes the unmap operation for both full and partial chunks in a
 * compressed block device. It iteratively submits unmap requests until all the chunks
 * have been unmapped or an error occurs.
 */
static void
_comp_submit_unmap_split(void *ctx)
{
	struct compress_unmap_split_ctx *split_ctx = ctx;
	struct spdk_bdev_io *bdev_io = split_ctx->bdev_io;
	struct vbdev_compress *comp_bdev = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_compress,
					   comp_bdev);
	struct partial_chunk_info *partial_chunk = NULL;
	uint64_t chunk_idx = 0;
	uint64_t block_offset = 0;
	uint64_t block_length = 0;

	if (split_ctx->status != 0 ||
	    (split_ctx->num_full_chunks_consumed == split_ctx->num_full_chunks &&
	     split_ctx->num_partial_chunks_consumed == split_ctx->num_partial_chunks)) {
		reduce_rw_blocks_cb(bdev_io, split_ctx->status);
		free(split_ctx);
		return;
	}

	if (split_ctx->num_full_chunks_consumed < split_ctx->num_full_chunks) {
		chunk_idx = split_ctx->full_chunk_idx_b + split_ctx->num_full_chunks_consumed;
		block_offset = chunk_idx * split_ctx->logical_blocks_per_chunk;
		block_length = split_ctx->logical_blocks_per_chunk;

		split_ctx->num_full_chunks_consumed++;
		spdk_reduce_vol_unmap(comp_bdev->vol,
				      block_offset, block_length,
				      _comp_unmap_subcmd_done_cb, split_ctx);
	} else if (split_ctx->num_partial_chunks_consumed < split_ctx->num_partial_chunks) {
		partial_chunk = &split_ctx->partial_chunk_info[split_ctx->num_partial_chunks_consumed];
		block_offset = partial_chunk->chunk_idx * split_ctx->logical_blocks_per_chunk +
			       partial_chunk->block_offset;
		block_length = partial_chunk->block_length;

		split_ctx->num_partial_chunks_consumed++;
		spdk_reduce_vol_unmap(comp_bdev->vol,
				      block_offset, block_length,
				      _comp_unmap_subcmd_done_cb, split_ctx);
	} else {
		assert(false);
	}
}

/*
 * When mkfs or fstrim, large unmap requests may be generated.
 * Large request will be split into multiple subcmds and processed recursively.
 * Run too many subcmds recursively may cause stack overflow or monopolize the thread,
 * delaying other tasks. To avoid this, next subcmd need to be processed asynchronously
 * by 'spdk_thread_send_msg'.
 */
static void
_comp_unmap_subcmd_done_cb(void *ctx, int error)
{
	struct compress_unmap_split_ctx *split_ctx = ctx;

	split_ctx->status = error;
	spdk_thread_send_msg(spdk_get_thread(), _comp_submit_unmap_split, split_ctx);
}

/*
 * This function splits the unmap operation into full and partial chunks based on the
 * block range specified in the 'spdk_bdev_io' structure. It calculates the start and end
 * chunks, as well as any partial chunks at the beginning or end of the range, and prepares
 * a context (compress_unmap_split_ctx) to handle these chunks. The unmap operation is
 * then submitted for processing through '_comp_submit_unmap_split'.
 * some cases to handle:
 * 1. start and end chunks are different
 * 1.1 start and end chunks are full
 * 1.2 start and end chunks are partial
 * 1.3 start or  end chunk  is full and the other is partial
 * 2. start and end chunks are the same
 * 2.1 full
 * 2.2 partial
 */
static void
_comp_submit_unmap(void *ctx)
{
	struct spdk_bdev_io *bdev_io = ctx;
	struct vbdev_compress *comp_bdev = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_compress,
					   comp_bdev);
	const struct spdk_reduce_vol_params *vol_params = spdk_reduce_vol_get_params(comp_bdev->vol);
	struct compress_unmap_split_ctx *split_ctx;
	struct partial_chunk_info *partial_chunk;
	uint32_t logical_blocks_per_chunk;
	uint64_t start_chunk, end_chunk, start_offset, end_tail;

	logical_blocks_per_chunk = vol_params->chunk_size / vol_params->logical_block_size;
	start_chunk = bdev_io->u.bdev.offset_blocks / logical_blocks_per_chunk;
	end_chunk = (bdev_io->u.bdev.offset_blocks + bdev_io->u.bdev.num_blocks - 1) /
		    logical_blocks_per_chunk;
	start_offset = bdev_io->u.bdev.offset_blocks % logical_blocks_per_chunk;
	end_tail = (bdev_io->u.bdev.offset_blocks + bdev_io->u.bdev.num_blocks) %
		   logical_blocks_per_chunk;

	split_ctx = calloc(1, sizeof(struct compress_unmap_split_ctx));
	if (split_ctx == NULL) {
		reduce_rw_blocks_cb(bdev_io, -ENOMEM);
		return;
	}
	partial_chunk = split_ctx->partial_chunk_info;
	split_ctx->bdev_io = bdev_io;
	split_ctx->logical_blocks_per_chunk = logical_blocks_per_chunk;

	if (start_chunk < end_chunk) {
		if (start_offset != 0) {
			partial_chunk[split_ctx->num_partial_chunks].chunk_idx = start_chunk;
			partial_chunk[split_ctx->num_partial_chunks].block_offset = start_offset;
			partial_chunk[split_ctx->num_partial_chunks].block_length = logical_blocks_per_chunk
					- start_offset;
			split_ctx->num_partial_chunks++;
			split_ctx->full_chunk_idx_b = start_chunk + 1;
		} else {
			split_ctx->full_chunk_idx_b = start_chunk;
		}

		if (end_tail != 0) {
			partial_chunk[split_ctx->num_partial_chunks].chunk_idx = end_chunk;
			partial_chunk[split_ctx->num_partial_chunks].block_offset = 0;
			partial_chunk[split_ctx->num_partial_chunks].block_length = end_tail;
			split_ctx->num_partial_chunks++;
			split_ctx->full_chunk_idx_e = end_chunk - 1;
		} else {
			split_ctx->full_chunk_idx_e = end_chunk;
		}

		split_ctx->num_full_chunks = end_chunk - start_chunk + 1 - split_ctx->num_partial_chunks;

		if (split_ctx->num_full_chunks) {
			assert(split_ctx->full_chunk_idx_b != UINT64_MAX && split_ctx->full_chunk_idx_e != UINT64_MAX);
			assert(split_ctx->full_chunk_idx_e - split_ctx->full_chunk_idx_b + 1 == split_ctx->num_full_chunks);
		} else {
			assert(split_ctx->full_chunk_idx_b - split_ctx->full_chunk_idx_e == 1);
		}
	} else if (start_offset != 0 || end_tail != 0) {
		partial_chunk[0].chunk_idx = start_chunk;
		partial_chunk[0].block_offset = start_offset;
		partial_chunk[0].block_length =
			bdev_io->u.bdev.num_blocks;
		split_ctx->num_partial_chunks = 1;
	} else {
		split_ctx->full_chunk_idx_b = start_chunk;
		split_ctx->full_chunk_idx_e = end_chunk;
		split_ctx->num_full_chunks = 1;
	}
	assert(split_ctx->num_partial_chunks <= SPDK_COUNTOF(split_ctx->partial_chunk_info));

	_comp_submit_unmap_split(split_ctx);
}

/* Called when someone above submits IO to this vbdev. */
static void
vbdev_compress_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct comp_bdev_io *io_ctx = (struct comp_bdev_io *)bdev_io->driver_ctx;
	struct vbdev_compress *comp_bdev = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_compress,
					   comp_bdev);
	struct comp_io_channel *comp_ch = spdk_io_channel_get_ctx(ch);

	memset(io_ctx, 0, sizeof(struct comp_bdev_io));
	io_ctx->comp_bdev = comp_bdev;
	io_ctx->comp_ch = comp_ch;
	io_ctx->orig_io = bdev_io;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, comp_read_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		return;
	case SPDK_BDEV_IO_TYPE_WRITE:
		spdk_thread_exec_msg(comp_bdev->reduce_thread, _comp_submit_write, bdev_io);
		return;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		spdk_thread_exec_msg(comp_bdev->reduce_thread, _comp_submit_unmap, bdev_io);
		return;
	/* TODO support RESET in future patch in the series */
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	case SPDK_BDEV_IO_TYPE_FLUSH:
	default:
		SPDK_ERRLOG("Unknown I/O type %d\n", bdev_io->type);
		spdk_bdev_io_complete(io_ctx->orig_io, SPDK_BDEV_IO_STATUS_FAILED);
		break;
	}
}

static bool
vbdev_compress_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	struct vbdev_compress *comp_bdev = (struct vbdev_compress *)ctx;

	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		return spdk_bdev_io_type_supported(comp_bdev->base_bdev, io_type);
	case SPDK_BDEV_IO_TYPE_UNMAP:
		return true;
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	default:
		return false;
	}
}

/* Callback for unregistering the IO device. */
static void
_device_unregister_cb(void *io_device)
{
	struct vbdev_compress *comp_bdev = io_device;

	/* Done with this comp_bdev. */
	pthread_mutex_destroy(&comp_bdev->reduce_lock);
	free(comp_bdev->comp_bdev.name);
	free(comp_bdev);
}

static void
_vbdev_compress_destruct_cb(void *ctx)
{
	struct vbdev_compress *comp_bdev = ctx;

	/* Close the underlying bdev on its same opened thread. */
	spdk_bdev_close(comp_bdev->base_desc);
	comp_bdev->vol = NULL;
	if (comp_bdev->init_failed) {
		free(comp_bdev);
		return;
	}

	TAILQ_REMOVE(&g_vbdev_comp, comp_bdev, link);
	spdk_bdev_module_release_bdev(comp_bdev->base_bdev);

	if (comp_bdev->orphaned == false) {
		spdk_io_device_unregister(comp_bdev, _device_unregister_cb);
	} else {
		vbdev_compress_delete_done(comp_bdev->delete_ctx, 0);
		_device_unregister_cb(comp_bdev);
	}
}

static void
vbdev_compress_destruct_cb(void *cb_arg, int reduce_errno)
{
	struct vbdev_compress *comp_bdev = (struct vbdev_compress *)cb_arg;

	if (reduce_errno) {
		SPDK_ERRLOG("number %d\n", reduce_errno);
	} else {
		if (comp_bdev->thread && comp_bdev->thread != spdk_get_thread()) {
			spdk_thread_send_msg(comp_bdev->thread,
					     _vbdev_compress_destruct_cb, comp_bdev);
		} else {
			_vbdev_compress_destruct_cb(comp_bdev);
		}
	}
}

static void
_reduce_destroy_cb(void *ctx, int reduce_errno)
{
	struct vbdev_compress *comp_bdev = (struct vbdev_compress *)ctx;

	if (reduce_errno) {
		SPDK_ERRLOG("number %d\n", reduce_errno);
	}

	comp_bdev->vol = NULL;
	spdk_put_io_channel(comp_bdev->base_ch);
	if (comp_bdev->init_failed || comp_bdev->orphaned) {
		vbdev_compress_destruct_cb((void *)comp_bdev, 0);
	} else {
		spdk_bdev_unregister(&comp_bdev->comp_bdev, vbdev_compress_delete_done,
				     comp_bdev->delete_ctx);
	}

}

static void
_delete_vol_unload_cb(void *ctx)
{
	struct vbdev_compress *comp_bdev = ctx;

	/* FIXME: Assert if these conditions are not satisfied for now. */
	assert(!comp_bdev->reduce_thread ||
	       comp_bdev->reduce_thread == spdk_get_thread());

	/* reducelib needs a channel to comm with the backing device */
	comp_bdev->base_ch = spdk_bdev_get_io_channel(comp_bdev->base_desc);

	/* Clean the device before we free our resources. */
	spdk_reduce_vol_destroy(&comp_bdev->backing_dev, _reduce_destroy_cb, comp_bdev);
}

/* Called by reduceLib after performing unload vol actions */
static void
delete_vol_unload_cb(void *cb_arg, int reduce_errno)
{
	struct vbdev_compress *comp_bdev = (struct vbdev_compress *)cb_arg;

	if (reduce_errno) {
		SPDK_ERRLOG("Failed to unload vol, error %s\n", spdk_strerror(-reduce_errno));
		vbdev_compress_delete_done(comp_bdev->delete_ctx, reduce_errno);
		return;
	}

	pthread_mutex_lock(&comp_bdev->reduce_lock);
	if (comp_bdev->reduce_thread && comp_bdev->reduce_thread != spdk_get_thread()) {
		spdk_thread_send_msg(comp_bdev->reduce_thread,
				     _delete_vol_unload_cb, comp_bdev);
		pthread_mutex_unlock(&comp_bdev->reduce_lock);
	} else {
		pthread_mutex_unlock(&comp_bdev->reduce_lock);

		_delete_vol_unload_cb(comp_bdev);
	}
}

const char *
compress_get_name(const struct vbdev_compress *comp_bdev)
{
	return comp_bdev->comp_bdev.name;
}

struct vbdev_compress *
compress_bdev_first(void)
{
	struct vbdev_compress *comp_bdev;

	comp_bdev = TAILQ_FIRST(&g_vbdev_comp);

	return comp_bdev;
}

struct vbdev_compress *
compress_bdev_next(struct vbdev_compress *prev)
{
	struct vbdev_compress *comp_bdev;

	comp_bdev = TAILQ_NEXT(prev, link);

	return comp_bdev;
}

bool
compress_has_orphan(const char *name)
{
	struct vbdev_compress *comp_bdev;

	TAILQ_FOREACH(comp_bdev, &g_vbdev_comp, link) {
		if (comp_bdev->orphaned && strcmp(name, comp_bdev->comp_bdev.name) == 0) {
			return true;
		}
	}
	return false;
}

/* Called after we've unregistered following a hot remove callback.
 * Our finish entry point will be called next.
 */
static int
vbdev_compress_destruct(void *ctx)
{
	struct vbdev_compress *comp_bdev = (struct vbdev_compress *)ctx;

	if (comp_bdev->vol != NULL) {
		/* Tell reducelib that we're done with this volume. */
		spdk_reduce_vol_unload(comp_bdev->vol, vbdev_compress_destruct_cb, comp_bdev);
	} else {
		vbdev_compress_destruct_cb(comp_bdev, 0);
	}

	return 0;
}

/* We supplied this as an entry point for upper layers who want to communicate to this
 * bdev.  This is how they get a channel.
 */
static struct spdk_io_channel *
vbdev_compress_get_io_channel(void *ctx)
{
	struct vbdev_compress *comp_bdev = (struct vbdev_compress *)ctx;

	/* The IO channel code will allocate a channel for us which consists of
	 * the SPDK channel structure plus the size of our comp_io_channel struct
	 * that we passed in when we registered our IO device. It will then call
	 * our channel create callback to populate any elements that we need to
	 * update.
	 */
	return spdk_get_io_channel(comp_bdev);
}

/* This is the output for bdev_get_bdevs() for this vbdev */
static int
vbdev_compress_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct vbdev_compress *comp_bdev = (struct vbdev_compress *)ctx;
	const struct spdk_reduce_vol_info *vol_info;
	char *comp_algo = NULL;

	if (comp_bdev->params.comp_algo == SPDK_ACCEL_COMP_ALGO_LZ4) {
		comp_algo = "lz4";
	} else if (comp_bdev->params.comp_algo == SPDK_ACCEL_COMP_ALGO_DEFLATE) {
		comp_algo = "deflate";
	} else {
		assert(false);
	}

	spdk_json_write_name(w, "compress");
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "name", spdk_bdev_get_name(&comp_bdev->comp_bdev));
	spdk_json_write_named_string(w, "base_bdev_name", spdk_bdev_get_name(comp_bdev->base_bdev));
	spdk_json_write_named_string(w, "pm_path", spdk_reduce_vol_get_pm_path(comp_bdev->vol));
	spdk_json_write_named_string(w, "comp_algo", comp_algo);
	spdk_json_write_named_uint32(w, "comp_level", comp_bdev->params.comp_level);
	spdk_json_write_named_uint32(w, "chunk_size", comp_bdev->params.chunk_size);
	spdk_json_write_named_uint32(w, "backing_io_unit_size", comp_bdev->params.backing_io_unit_size);
	vol_info = spdk_reduce_vol_get_info(comp_bdev->vol);
	spdk_json_write_named_uint64(w, "allocated_io_units", vol_info->allocated_io_units);
	spdk_json_write_object_end(w);

	return 0;
}

static int
vbdev_compress_config_json(struct spdk_json_write_ctx *w)
{
	/* Nothing to dump as compress bdev configuration is saved on physical device. */
	return 0;
}

struct vbdev_init_reduce_ctx {
	struct vbdev_compress   *comp_bdev;
	int                     status;
	bdev_compress_create_cb cb_fn;
	void                    *cb_ctx;
};

static void
_cleanup_vol_unload_cb(void *ctx)
{
	struct vbdev_compress *comp_bdev = ctx;

	assert(!comp_bdev->reduce_thread ||
	       comp_bdev->reduce_thread == spdk_get_thread());

	comp_bdev->base_ch = spdk_bdev_get_io_channel(comp_bdev->base_desc);

	spdk_reduce_vol_destroy(&comp_bdev->backing_dev, _reduce_destroy_cb, comp_bdev);
}

static void
init_vol_unload_cb(void *ctx, int reduce_errno)
{
	struct vbdev_compress *comp_bdev = (struct vbdev_compress *)ctx;

	if (reduce_errno) {
		SPDK_ERRLOG("Failed to unload vol, error %s\n", spdk_strerror(-reduce_errno));
	}

	pthread_mutex_lock(&comp_bdev->reduce_lock);
	if (comp_bdev->reduce_thread && comp_bdev->reduce_thread != spdk_get_thread()) {
		spdk_thread_send_msg(comp_bdev->reduce_thread,
				     _cleanup_vol_unload_cb, comp_bdev);
		pthread_mutex_unlock(&comp_bdev->reduce_lock);
	} else {
		pthread_mutex_unlock(&comp_bdev->reduce_lock);

		_cleanup_vol_unload_cb(comp_bdev);
	}
}

static void
_vbdev_reduce_init_cb(void *ctx)
{
	struct vbdev_init_reduce_ctx *init_ctx = ctx;
	struct vbdev_compress *comp_bdev = init_ctx->comp_bdev;
	int rc = init_ctx->status;

	assert(comp_bdev->base_desc != NULL);

	/* We're done with metadata operations */
	spdk_put_io_channel(comp_bdev->base_ch);

	if (rc != 0) {
		goto err;
	}

	assert(comp_bdev->vol != NULL);

	rc = vbdev_compress_claim(comp_bdev);
	if (rc != 0) {
		comp_bdev->init_failed = true;
		spdk_reduce_vol_unload(comp_bdev->vol, init_vol_unload_cb, comp_bdev);
	}

	init_ctx->cb_fn(init_ctx->cb_ctx, rc);
	free(init_ctx);
	return;

err:
	init_ctx->cb_fn(init_ctx->cb_ctx, rc);
	/* Close the underlying bdev on its same opened thread. */
	spdk_bdev_close(comp_bdev->base_desc);
	free(comp_bdev);
	free(init_ctx);
}

/* Callback from reduce for when init is complete. We'll pass the vbdev_comp struct
 * used for initial metadata operations to claim where it will be further filled out
 * and added to the global list.
 */
static void
vbdev_reduce_init_cb(void *cb_arg, struct spdk_reduce_vol *vol, int reduce_errno)
{
	struct vbdev_init_reduce_ctx *init_ctx = cb_arg;
	struct vbdev_compress *comp_bdev = init_ctx->comp_bdev;

	if (reduce_errno == 0) {
		comp_bdev->vol = vol;
	} else {
		SPDK_ERRLOG("for vol %s, error %s\n",
			    spdk_bdev_get_name(comp_bdev->base_bdev), spdk_strerror(-reduce_errno));
	}

	init_ctx->status = reduce_errno;

	if (comp_bdev->thread && comp_bdev->thread != spdk_get_thread()) {
		spdk_thread_send_msg(comp_bdev->thread, _vbdev_reduce_init_cb, init_ctx);
	} else {
		_vbdev_reduce_init_cb(init_ctx);
	}
}

/* Callback for the function used by reduceLib to perform IO to/from the backing device. We just
 * call the callback provided by reduceLib when it called the read/write/unmap function and
 * free the bdev_io.
 */
static void
comp_reduce_io_cb(struct spdk_bdev_io *bdev_io, bool success, void *arg)
{
	struct spdk_reduce_vol_cb_args *cb_args = arg;
	int reduce_errno;

	if (success) {
		reduce_errno = 0;
	} else {
		reduce_errno = -EIO;
	}
	spdk_bdev_free_io(bdev_io);
	cb_args->cb_fn(cb_args->cb_arg, reduce_errno);
}

static void
_comp_backing_bdev_queue_io_wait(struct vbdev_compress *comp_bdev,
				 struct spdk_reduce_backing_io *backing_io)
{
	struct spdk_bdev_io_wait_entry *waitq_entry;
	int rc;

	waitq_entry = (struct spdk_bdev_io_wait_entry *) &backing_io->user_ctx;
	waitq_entry->bdev = spdk_bdev_desc_get_bdev(comp_bdev->base_desc);
	waitq_entry->cb_fn = _comp_reduce_resubmit_backing_io;
	waitq_entry->cb_arg = backing_io;

	rc = spdk_bdev_queue_io_wait(waitq_entry->bdev, comp_bdev->base_ch, waitq_entry);
	if (rc) {
		SPDK_ERRLOG("Queue io failed in _comp_backing_bdev_queue_io_wait, rc=%d.\n", rc);
		assert(false);
		backing_io->backing_cb_args->cb_fn(backing_io->backing_cb_args->cb_arg, rc);
	}
}

static void
_comp_backing_bdev_read(struct spdk_reduce_backing_io *backing_io)
{
	struct spdk_reduce_vol_cb_args *backing_cb_args = backing_io->backing_cb_args;
	struct vbdev_compress *comp_bdev = SPDK_CONTAINEROF(backing_io->dev, struct vbdev_compress,
					   backing_dev);
	int rc;

	rc = spdk_bdev_readv_blocks(comp_bdev->base_desc, comp_bdev->base_ch,
				    backing_io->iov, backing_io->iovcnt,
				    backing_io->lba, backing_io->lba_count,
				    comp_reduce_io_cb,
				    backing_cb_args);

	if (rc) {
		if (rc == -ENOMEM) {
			_comp_backing_bdev_queue_io_wait(comp_bdev, backing_io);
			return;
		} else {
			SPDK_ERRLOG("submitting readv request, rc=%d\n", rc);
		}
		backing_cb_args->cb_fn(backing_cb_args->cb_arg, rc);
	}
}

static void
_comp_backing_bdev_write(struct spdk_reduce_backing_io  *backing_io)
{
	struct spdk_reduce_vol_cb_args *backing_cb_args = backing_io->backing_cb_args;
	struct vbdev_compress *comp_bdev = SPDK_CONTAINEROF(backing_io->dev, struct vbdev_compress,
					   backing_dev);
	int rc;

	rc = spdk_bdev_writev_blocks(comp_bdev->base_desc, comp_bdev->base_ch,
				     backing_io->iov, backing_io->iovcnt,
				     backing_io->lba, backing_io->lba_count,
				     comp_reduce_io_cb,
				     backing_cb_args);

	if (rc) {
		if (rc == -ENOMEM) {
			_comp_backing_bdev_queue_io_wait(comp_bdev, backing_io);
			return;
		} else {
			SPDK_ERRLOG("error submitting writev request, rc=%d\n", rc);
		}
		backing_cb_args->cb_fn(backing_cb_args->cb_arg, rc);
	}
}

static void
_comp_backing_bdev_unmap(struct spdk_reduce_backing_io *backing_io)
{
	struct spdk_reduce_vol_cb_args *backing_cb_args = backing_io->backing_cb_args;
	struct vbdev_compress *comp_bdev = SPDK_CONTAINEROF(backing_io->dev, struct vbdev_compress,
					   backing_dev);
	int rc;

	rc = spdk_bdev_unmap_blocks(comp_bdev->base_desc, comp_bdev->base_ch,
				    backing_io->lba, backing_io->lba_count,
				    comp_reduce_io_cb,
				    backing_cb_args);

	if (rc) {
		if (rc == -ENOMEM) {
			_comp_backing_bdev_queue_io_wait(comp_bdev, backing_io);
			return;
		} else {
			SPDK_ERRLOG("submitting unmap request, rc=%d\n", rc);
		}
		backing_cb_args->cb_fn(backing_cb_args->cb_arg, rc);
	}
}

/* This is the function provided to the reduceLib for sending reads/writes/unmaps
 * directly to the backing device.
 */
static void
_comp_reduce_submit_backing_io(struct spdk_reduce_backing_io *backing_io)
{
	switch (backing_io->backing_io_type) {
	case SPDK_REDUCE_BACKING_IO_WRITE:
		_comp_backing_bdev_write(backing_io);
		break;
	case SPDK_REDUCE_BACKING_IO_READ:
		_comp_backing_bdev_read(backing_io);
		break;
	case SPDK_REDUCE_BACKING_IO_UNMAP:
		_comp_backing_bdev_unmap(backing_io);
		break;
	default:
		SPDK_ERRLOG("Unknown I/O type %d\n", backing_io->backing_io_type);
		backing_io->backing_cb_args->cb_fn(backing_io->backing_cb_args->cb_arg, -EINVAL);
		break;
	}
}

static void
_comp_reduce_resubmit_backing_io(void *_backing_io)
{
	struct spdk_reduce_backing_io *backing_io = _backing_io;

	_comp_reduce_submit_backing_io(backing_io);
}

/* Called by reduceLib after performing unload vol actions following base bdev hotremove */
static void
bdev_hotremove_vol_unload_cb(void *cb_arg, int reduce_errno)
{
	struct vbdev_compress *comp_bdev = (struct vbdev_compress *)cb_arg;

	if (reduce_errno) {
		SPDK_ERRLOG("number %d\n", reduce_errno);
	}

	comp_bdev->vol = NULL;
	spdk_bdev_unregister(&comp_bdev->comp_bdev, NULL, NULL);
}

static void
vbdev_compress_base_bdev_hotremove_cb(struct spdk_bdev *bdev_find)
{
	struct vbdev_compress *comp_bdev, *tmp;

	TAILQ_FOREACH_SAFE(comp_bdev, &g_vbdev_comp, link, tmp) {
		if (bdev_find == comp_bdev->base_bdev) {
			/* Tell reduceLib that we're done with this volume. */
			spdk_reduce_vol_unload(comp_bdev->vol, bdev_hotremove_vol_unload_cb, comp_bdev);
		}
	}
}

/* Called when the underlying base bdev triggers asynchronous event such as bdev removal. */
static void
vbdev_compress_base_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
				  void *event_ctx)
{
	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		vbdev_compress_base_bdev_hotremove_cb(bdev);
		break;
	default:
		SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
		break;
	}
}

/* TODO: determine which parms we want user configurable, HC for now
 * params.vol_size
 * params.chunk_size
 * compression PMD, algorithm, window size, comp level, etc.
 * DEV_MD_PATH
 */

/* Common function for init and load to allocate and populate the minimal
 * information for reducelib to init or load.
 */
struct vbdev_compress *
_prepare_for_load_init(struct spdk_bdev_desc *bdev_desc, uint32_t lb_size, uint8_t comp_algo,
		       uint32_t comp_level)
{
	struct vbdev_compress *comp_bdev;
	struct spdk_bdev *bdev;

	comp_bdev = calloc(1, sizeof(struct vbdev_compress));
	if (comp_bdev == NULL) {
		SPDK_ERRLOG("failed to alloc comp_bdev\n");
		return NULL;
	}

	comp_bdev->backing_dev.submit_backing_io = _comp_reduce_submit_backing_io;
	comp_bdev->backing_dev.compress = _comp_reduce_compress;
	comp_bdev->backing_dev.decompress = _comp_reduce_decompress;

	comp_bdev->base_desc = bdev_desc;
	bdev = spdk_bdev_desc_get_bdev(bdev_desc);
	comp_bdev->base_bdev = bdev;

	comp_bdev->backing_dev.blocklen = bdev->blocklen;
	comp_bdev->backing_dev.blockcnt = bdev->blockcnt;

	comp_bdev->backing_dev.user_ctx_size = sizeof(struct spdk_bdev_io_wait_entry);

	comp_bdev->comp_algo = comp_algo;
	comp_bdev->comp_level = comp_level;
	comp_bdev->params.comp_algo = comp_algo;
	comp_bdev->params.comp_level = comp_level;
	comp_bdev->params.chunk_size = CHUNK_SIZE;
	if (lb_size == 0) {
		comp_bdev->params.logical_block_size = bdev->blocklen;
	} else {
		comp_bdev->params.logical_block_size = lb_size;
	}

	comp_bdev->params.backing_io_unit_size = BACKING_IO_SZ;
	return comp_bdev;
}

/* Call reducelib to initialize a new volume */
static int
vbdev_init_reduce(const char *bdev_name, const char *pm_path, uint32_t lb_size, uint8_t comp_algo,
		  uint32_t comp_level, bdev_compress_create_cb cb_fn, void *cb_arg)
{
	struct spdk_bdev_desc *bdev_desc = NULL;
	struct vbdev_init_reduce_ctx *init_ctx;
	struct vbdev_compress *comp_bdev;
	int rc;

	init_ctx = calloc(1, sizeof(*init_ctx));
	if (init_ctx == NULL) {
		SPDK_ERRLOG("failed to alloc init contexts\n");
		return - ENOMEM;
	}

	init_ctx->cb_fn = cb_fn;
	init_ctx->cb_ctx = cb_arg;

	rc = spdk_bdev_open_ext(bdev_name, true, vbdev_compress_base_bdev_event_cb,
				NULL, &bdev_desc);
	if (rc) {
		SPDK_ERRLOG("could not open bdev %s, error %s\n", bdev_name, spdk_strerror(-rc));
		free(init_ctx);
		return rc;
	}

	comp_bdev = _prepare_for_load_init(bdev_desc, lb_size, comp_algo, comp_level);
	if (comp_bdev == NULL) {
		free(init_ctx);
		spdk_bdev_close(bdev_desc);
		return -EINVAL;
	}

	init_ctx->comp_bdev = comp_bdev;

	/* Save the thread where the base device is opened */
	comp_bdev->thread = spdk_get_thread();

	comp_bdev->base_ch = spdk_bdev_get_io_channel(comp_bdev->base_desc);

	spdk_reduce_vol_init(&comp_bdev->params, &comp_bdev->backing_dev,
			     pm_path,
			     vbdev_reduce_init_cb,
			     init_ctx);
	return 0;
}

/* We provide this callback for the SPDK channel code to create a channel using
 * the channel struct we provided in our module get_io_channel() entry point. Here
 * we get and save off an underlying base channel of the device below us so that
 * we can communicate with the base bdev on a per channel basis.  If we needed
 * our own poller for this vbdev, we'd register it here.
 */
static int
comp_bdev_ch_create_cb(void *io_device, void *ctx_buf)
{
	struct vbdev_compress *comp_bdev = io_device;

	/* Now set the reduce channel if it's not already set. */
	pthread_mutex_lock(&comp_bdev->reduce_lock);
	if (comp_bdev->ch_count == 0) {
		/* We use this queue to track outstanding IO in our layer. */
		TAILQ_INIT(&comp_bdev->pending_comp_ios);

		/* We use this to queue up compression operations as needed. */
		TAILQ_INIT(&comp_bdev->queued_comp_ops);

		comp_bdev->base_ch = spdk_bdev_get_io_channel(comp_bdev->base_desc);
		comp_bdev->reduce_thread = spdk_get_thread();
		comp_bdev->accel_channel = spdk_accel_get_io_channel();
	}
	comp_bdev->ch_count++;
	pthread_mutex_unlock(&comp_bdev->reduce_lock);

	return 0;
}

static void
_channel_cleanup(struct vbdev_compress *comp_bdev)
{
	spdk_put_io_channel(comp_bdev->base_ch);
	spdk_put_io_channel(comp_bdev->accel_channel);
	comp_bdev->reduce_thread = NULL;
}

/* Used to reroute destroy_ch to the correct thread */
static void
_comp_bdev_ch_destroy_cb(void *arg)
{
	struct vbdev_compress *comp_bdev = arg;

	pthread_mutex_lock(&comp_bdev->reduce_lock);
	_channel_cleanup(comp_bdev);
	pthread_mutex_unlock(&comp_bdev->reduce_lock);
}

/* We provide this callback for the SPDK channel code to destroy a channel
 * created with our create callback. We just need to undo anything we did
 * when we created. If this bdev used its own poller, we'd unregister it here.
 */
static void
comp_bdev_ch_destroy_cb(void *io_device, void *ctx_buf)
{
	struct vbdev_compress *comp_bdev = io_device;

	pthread_mutex_lock(&comp_bdev->reduce_lock);
	comp_bdev->ch_count--;
	if (comp_bdev->ch_count == 0) {
		/* Send this request to the thread where the channel was created. */
		if (comp_bdev->reduce_thread != spdk_get_thread()) {
			spdk_thread_send_msg(comp_bdev->reduce_thread,
					     _comp_bdev_ch_destroy_cb, comp_bdev);
		} else {
			_channel_cleanup(comp_bdev);
		}
	}
	pthread_mutex_unlock(&comp_bdev->reduce_lock);
}

static int
_check_compress_bdev_comp_algo(enum spdk_accel_comp_algo algo, uint32_t comp_level)
{
	uint32_t min_level, max_level;
	int rc;

	rc = spdk_accel_get_compress_level_range(algo, &min_level, &max_level);
	if (rc != 0) {
		return rc;
	}

	/* If both min_level and max_level are 0, the compression level can be ignored.
	 * The back-end implementation hardcodes the compression level.
	 */
	if (min_level == 0 && max_level == 0) {
		return 0;
	}

	if (comp_level > max_level || comp_level < min_level) {
		return -EINVAL;
	}

	return 0;
}

/* RPC entry point for compression vbdev creation. */
int
create_compress_bdev(const char *bdev_name, const char *pm_path, uint32_t lb_size,
		     uint8_t comp_algo, uint32_t comp_level,
		     bdev_compress_create_cb cb_fn, void *cb_arg)
{
	struct vbdev_compress *comp_bdev = NULL;
	struct stat info;
	int rc;

	if (stat(pm_path, &info) != 0) {
		SPDK_ERRLOG("PM path %s does not exist.\n", pm_path);
		return -EINVAL;
	} else if (!S_ISDIR(info.st_mode)) {
		SPDK_ERRLOG("PM path %s is not a directory.\n", pm_path);
		return -EINVAL;
	}

	if ((lb_size != 0) && (lb_size != LB_SIZE_4K) && (lb_size != LB_SIZE_512B)) {
		SPDK_ERRLOG("Logical block size must be 512 or 4096\n");
		return -EINVAL;
	}

	rc = _check_compress_bdev_comp_algo(comp_algo, comp_level);
	if (rc != 0) {
		SPDK_ERRLOG("Compress bdev doesn't support compression algo(%u) or level(%u)\n",
			    comp_algo, comp_level);
		return rc;
	}

	TAILQ_FOREACH(comp_bdev, &g_vbdev_comp, link) {
		if (strcmp(bdev_name, comp_bdev->base_bdev->name) == 0) {
			SPDK_ERRLOG("Bass bdev %s already being used for a compress bdev\n", bdev_name);
			return -EBUSY;
		}
	}
	return vbdev_init_reduce(bdev_name, pm_path, lb_size, comp_algo, comp_level, cb_fn, cb_arg);
}

static int
vbdev_compress_init(void)
{
	return 0;
}

/* Called when the entire module is being torn down. */
static void
vbdev_compress_finish(void)
{
	/* TODO: unload vol in a future patch */
}

/* During init we'll be asked how much memory we'd like passed to us
 * in bev_io structures as context. Here's where we specify how
 * much context we want per IO.
 */
static int
vbdev_compress_get_ctx_size(void)
{
	return sizeof(struct comp_bdev_io);
}

/* When we register our bdev this is how we specify our entry points. */
static const struct spdk_bdev_fn_table vbdev_compress_fn_table = {
	.destruct		= vbdev_compress_destruct,
	.submit_request		= vbdev_compress_submit_request,
	.io_type_supported	= vbdev_compress_io_type_supported,
	.get_io_channel		= vbdev_compress_get_io_channel,
	.dump_info_json		= vbdev_compress_dump_info_json,
	.write_config_json	= NULL,
};

static struct spdk_bdev_module compress_if = {
	.name = "compress",
	.module_init = vbdev_compress_init,
	.get_ctx_size = vbdev_compress_get_ctx_size,
	.examine_disk = vbdev_compress_examine,
	.module_fini = vbdev_compress_finish,
	.config_json = vbdev_compress_config_json
};

SPDK_BDEV_MODULE_REGISTER(compress, &compress_if)

static int _set_compbdev_name(struct vbdev_compress *comp_bdev)
{
	struct spdk_bdev_alias *aliases;

	if (!TAILQ_EMPTY(spdk_bdev_get_aliases(comp_bdev->base_bdev))) {
		aliases = TAILQ_FIRST(spdk_bdev_get_aliases(comp_bdev->base_bdev));
		comp_bdev->comp_bdev.name = spdk_sprintf_alloc("COMP_%s", aliases->alias.name);
		if (!comp_bdev->comp_bdev.name) {
			SPDK_ERRLOG("could not allocate comp_bdev name for alias\n");
			return -ENOMEM;
		}
	} else {
		comp_bdev->comp_bdev.name = spdk_sprintf_alloc("COMP_%s", comp_bdev->base_bdev->name);
		if (!comp_bdev->comp_bdev.name) {
			SPDK_ERRLOG("could not allocate comp_bdev name for unique name\n");
			return -ENOMEM;
		}
	}
	return 0;
}

static int
vbdev_compress_claim(struct vbdev_compress *comp_bdev)
{
	struct spdk_uuid ns_uuid;
	int rc;

	if (_set_compbdev_name(comp_bdev)) {
		return -EINVAL;
	}

	/* Note: some of the fields below will change in the future - for example,
	 * blockcnt specifically will not match (the compressed volume size will
	 * be slightly less than the base bdev size)
	 */
	comp_bdev->comp_bdev.product_name = COMP_BDEV_NAME;
	comp_bdev->comp_bdev.write_cache = comp_bdev->base_bdev->write_cache;

	comp_bdev->comp_bdev.optimal_io_boundary =
		comp_bdev->params.chunk_size / comp_bdev->params.logical_block_size;

	comp_bdev->comp_bdev.split_on_optimal_io_boundary = true;

	comp_bdev->comp_bdev.blocklen = comp_bdev->params.logical_block_size;
	comp_bdev->comp_bdev.blockcnt = comp_bdev->params.vol_size / comp_bdev->comp_bdev.blocklen;
	assert(comp_bdev->comp_bdev.blockcnt > 0);

	/* This is the context that is passed to us when the bdev
	 * layer calls in so we'll save our comp_bdev node here.
	 */
	comp_bdev->comp_bdev.ctxt = comp_bdev;
	comp_bdev->comp_bdev.fn_table = &vbdev_compress_fn_table;
	comp_bdev->comp_bdev.module = &compress_if;

	/* Generate UUID based on namespace UUID + base bdev UUID. */
	spdk_uuid_parse(&ns_uuid, BDEV_COMPRESS_NAMESPACE_UUID);
	rc = spdk_uuid_generate_sha1(&comp_bdev->comp_bdev.uuid, &ns_uuid,
				     (const char *)&comp_bdev->base_bdev->uuid, sizeof(struct spdk_uuid));
	if (rc) {
		SPDK_ERRLOG("Unable to generate new UUID for compress bdev, error %s\n", spdk_strerror(-rc));
		return -EINVAL;
	}

	pthread_mutex_init(&comp_bdev->reduce_lock, NULL);

	/* Save the thread where the base device is opened */
	comp_bdev->thread = spdk_get_thread();

	spdk_io_device_register(comp_bdev, comp_bdev_ch_create_cb, comp_bdev_ch_destroy_cb,
				sizeof(struct comp_io_channel),
				comp_bdev->comp_bdev.name);

	rc = spdk_bdev_module_claim_bdev(comp_bdev->base_bdev, comp_bdev->base_desc,
					 comp_bdev->comp_bdev.module);
	if (rc) {
		SPDK_ERRLOG("could not claim bdev %s, error %s\n", spdk_bdev_get_name(comp_bdev->base_bdev),
			    spdk_strerror(-rc));
		goto error_claim;
	}

	rc = spdk_bdev_register(&comp_bdev->comp_bdev);
	if (rc < 0) {
		SPDK_ERRLOG("trying to register bdev, error %s\n", spdk_strerror(-rc));
		goto error_bdev_register;
	}

	TAILQ_INSERT_TAIL(&g_vbdev_comp, comp_bdev, link);

	SPDK_NOTICELOG("registered io_device and virtual bdev for: %s\n", comp_bdev->comp_bdev.name);

	return 0;

	/* Error cleanup paths. */
error_bdev_register:
	spdk_bdev_module_release_bdev(comp_bdev->base_bdev);
error_claim:
	spdk_io_device_unregister(comp_bdev, NULL);
	free(comp_bdev->comp_bdev.name);
	return rc;
}

static void
_vbdev_compress_delete_done(void *_ctx)
{
	struct vbdev_comp_delete_ctx *ctx = _ctx;

	ctx->cb_fn(ctx->cb_arg, ctx->cb_rc);

	free(ctx);
}

static void
vbdev_compress_delete_done(void *cb_arg, int bdeverrno)
{
	struct vbdev_comp_delete_ctx *ctx = cb_arg;

	ctx->cb_rc = bdeverrno;

	if (ctx->orig_thread != spdk_get_thread()) {
		spdk_thread_send_msg(ctx->orig_thread, _vbdev_compress_delete_done, ctx);
	} else {
		_vbdev_compress_delete_done(ctx);
	}
}

void
bdev_compress_delete(const char *name, spdk_delete_compress_complete cb_fn, void *cb_arg)
{
	struct vbdev_compress *comp_bdev = NULL;
	struct vbdev_comp_delete_ctx *ctx;

	TAILQ_FOREACH(comp_bdev, &g_vbdev_comp, link) {
		if (strcmp(name, comp_bdev->comp_bdev.name) == 0) {
			break;
		}
	}

	if (comp_bdev == NULL) {
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		SPDK_ERRLOG("Failed to allocate delete context\n");
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	/* Save these for after the vol is destroyed. */
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	ctx->orig_thread = spdk_get_thread();

	comp_bdev->delete_ctx = ctx;

	/* Tell reducelib that we're done with this volume. */
	if (comp_bdev->orphaned == false) {
		spdk_reduce_vol_unload(comp_bdev->vol, delete_vol_unload_cb, comp_bdev);
	} else {
		delete_vol_unload_cb(comp_bdev, 0);
	}
}

static void
_vbdev_reduce_load_unload_cb(void *ctx, int reduce_errno)
{
}

static void
_vbdev_reduce_load_cb(void *ctx)
{
	struct vbdev_compress *comp_bdev = ctx;
	int rc;

	assert(comp_bdev->base_desc != NULL);

	/* Done with metadata operations */
	spdk_put_io_channel(comp_bdev->base_ch);

	if (comp_bdev->reduce_errno == 0) {
		rc = vbdev_compress_claim(comp_bdev);
		if (rc != 0) {
			spdk_reduce_vol_unload(comp_bdev->vol, _vbdev_reduce_load_unload_cb, NULL);
			goto err;
		}
	} else if (comp_bdev->reduce_errno == -ENOENT) {
		if (_set_compbdev_name(comp_bdev)) {
			goto err;
		}

		/* Save the thread where the base device is opened */
		comp_bdev->thread = spdk_get_thread();

		comp_bdev->comp_bdev.module = &compress_if;
		pthread_mutex_init(&comp_bdev->reduce_lock, NULL);
		rc = spdk_bdev_module_claim_bdev(comp_bdev->base_bdev, comp_bdev->base_desc,
						 comp_bdev->comp_bdev.module);
		if (rc) {
			SPDK_ERRLOG("could not claim bdev %s, error %s\n", spdk_bdev_get_name(comp_bdev->base_bdev),
				    spdk_strerror(-rc));
			free(comp_bdev->comp_bdev.name);
			goto err;
		}

		comp_bdev->orphaned = true;
		TAILQ_INSERT_TAIL(&g_vbdev_comp, comp_bdev, link);
	} else {
		if (comp_bdev->reduce_errno != -EILSEQ) {
			SPDK_ERRLOG("for vol %s, error %s\n", spdk_bdev_get_name(comp_bdev->base_bdev),
				    spdk_strerror(-comp_bdev->reduce_errno));
		}
		goto err;
	}

	spdk_bdev_module_examine_done(&compress_if);
	return;

err:
	/* Close the underlying bdev on its same opened thread. */
	spdk_bdev_close(comp_bdev->base_desc);
	free(comp_bdev);
	spdk_bdev_module_examine_done(&compress_if);
}

/* Callback from reduce for then load is complete. We'll pass the vbdev_comp struct
 * used for initial metadata operations to claim where it will be further filled out
 * and added to the global list.
 */
static void
vbdev_reduce_load_cb(void *cb_arg, struct spdk_reduce_vol *vol, int reduce_errno)
{
	struct vbdev_compress *comp_bdev = cb_arg;

	if (reduce_errno == 0) {
		/* Update information following volume load. */
		comp_bdev->vol = vol;
		memcpy(&comp_bdev->params, spdk_reduce_vol_get_params(vol),
		       sizeof(struct spdk_reduce_vol_params));
		comp_bdev->comp_algo = comp_bdev->params.comp_algo;
		comp_bdev->comp_level = comp_bdev->params.comp_level;
	}

	comp_bdev->reduce_errno = reduce_errno;

	if (comp_bdev->thread && comp_bdev->thread != spdk_get_thread()) {
		spdk_thread_send_msg(comp_bdev->thread, _vbdev_reduce_load_cb, comp_bdev);
	} else {
		_vbdev_reduce_load_cb(comp_bdev);
	}

}

/* Examine_disk entry point: will do a metadata load to see if this is ours,
 * and if so will go ahead and claim it.
 */
static void
vbdev_compress_examine(struct spdk_bdev *bdev)
{
	struct spdk_bdev_desc *bdev_desc = NULL;
	struct vbdev_compress *comp_bdev;
	int rc;

	if (strcmp(bdev->product_name, COMP_BDEV_NAME) == 0) {
		spdk_bdev_module_examine_done(&compress_if);
		return;
	}

	rc = spdk_bdev_open_ext(spdk_bdev_get_name(bdev), false,
				vbdev_compress_base_bdev_event_cb, NULL, &bdev_desc);
	if (rc) {
		SPDK_ERRLOG("could not open bdev %s, error %s\n", spdk_bdev_get_name(bdev),
			    spdk_strerror(-rc));
		spdk_bdev_module_examine_done(&compress_if);
		return;
	}

	comp_bdev = _prepare_for_load_init(bdev_desc, 0, SPDK_ACCEL_COMP_ALGO_DEFLATE, 1);
	if (comp_bdev == NULL) {
		spdk_bdev_close(bdev_desc);
		spdk_bdev_module_examine_done(&compress_if);
		return;
	}

	/* Save the thread where the base device is opened */
	comp_bdev->thread = spdk_get_thread();

	comp_bdev->base_ch = spdk_bdev_get_io_channel(comp_bdev->base_desc);
	spdk_reduce_vol_load(&comp_bdev->backing_dev, vbdev_reduce_load_cb, comp_bdev);
}

SPDK_LOG_REGISTER_COMPONENT(vbdev_compress)
