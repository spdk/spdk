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

#include "spdk_cunit.h"
#include "spdk/blob.h"

#include "lib/test_env.c"
#include "../bs_dev_common.c"
#include "blobstore.c"
#include "request.c"

struct spdk_blob_store *g_bs;
spdk_blob_id g_blobid;
struct spdk_blob *g_blob;
int g_bserrno;
struct spdk_xattr_names *g_names;
int g_done;

bool g_scheduler_delay = false;

struct scheduled_ops {
	spdk_thread_fn	fn;
	void		*ctx;

	TAILQ_ENTRY(scheduled_ops)	ops_queue;
};

static TAILQ_HEAD(, scheduled_ops) g_scheduled_ops = TAILQ_HEAD_INITIALIZER(g_scheduled_ops);

struct spdk_bs_super_block_ver1 {
	uint8_t		signature[8];
	uint32_t        version;
	uint32_t        length;
	uint32_t	clean; /* If there was a clean shutdown, this is 1. */
	spdk_blob_id	super_blob;

	uint32_t	cluster_size; /* In bytes */

	uint32_t	used_page_mask_start; /* Offset from beginning of disk, in pages */
	uint32_t	used_page_mask_len; /* Count, in pages */

	uint32_t	used_cluster_mask_start; /* Offset from beginning of disk, in pages */
	uint32_t	used_cluster_mask_len; /* Count, in pages */

	uint32_t	md_start; /* Offset from beginning of disk, in pages */
	uint32_t	md_len; /* Count, in pages */

	uint8_t		reserved[4036];
	uint32_t	crc;
} __attribute__((packed));
SPDK_STATIC_ASSERT(sizeof(struct spdk_bs_super_block_ver1) == 0x1000, "Invalid super block size");

struct spdk_bs_init_ctx_ver1 {
	struct spdk_blob_store		*bs;
	struct spdk_bs_super_block_ver1	*super;
};

struct spdk_bs_unload_ctx_ver1 {
	struct spdk_blob_store		*bs;
	struct spdk_bs_super_block_ver1	*super;

	struct spdk_bs_md_mask		*mask;
};

static void
_spdk_bs_init_persist_super_cpl_ver1(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_init_ctx_ver1 *ctx = cb_arg;

	spdk_dma_free(ctx->super);
	free(ctx);

	spdk_bs_sequence_finish(seq, bserrno);
}

static void
_spdk_bs_init_trim_cpl_ver1(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_init_ctx_ver1 *ctx = cb_arg;

	/* Write super block */
	spdk_bs_sequence_write(seq, ctx->super, _spdk_bs_page_to_lba(ctx->bs, 0),
			       _spdk_bs_byte_to_lba(ctx->bs, sizeof(*ctx->super)),
			       _spdk_bs_init_persist_super_cpl_ver1, ctx);
}

static void
spdk_bs_init_ver1(struct spdk_bs_dev *dev, struct spdk_bs_opts *o,
		  spdk_bs_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_bs_init_ctx_ver1 *ctx;
	struct spdk_blob_store	*bs;
	struct spdk_bs_cpl	cpl;
	spdk_bs_sequence_t	*seq;
	uint64_t		num_md_pages;
	uint64_t		num_md_clusters;
	uint32_t		i;
	struct spdk_bs_opts	opts = {};
	int			rc;

	SPDK_DEBUGLOG(SPDK_TRACE_BLOB, "Initializing blobstore on dev %p\n", dev);

	if ((SPDK_BS_PAGE_SIZE % dev->blocklen) != 0) {
		SPDK_ERRLOG("unsupported dev block length of %d\n",
			    dev->blocklen);
		dev->destroy(dev);
		cb_fn(cb_arg, NULL, -EINVAL);
		return;
	}

	if (o) {
		opts = *o;
	} else {
		spdk_bs_opts_init(&opts);
	}

	if (_spdk_bs_opts_verify(&opts) != 0) {
		dev->destroy(dev);
		cb_fn(cb_arg, NULL, -EINVAL);
		return;
	}

	bs = _spdk_bs_alloc(dev, &opts);
	if (!bs) {
		dev->destroy(dev);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	if (opts.num_md_pages == SPDK_BLOB_OPTS_NUM_MD_PAGES) {
		/* By default, allocate 1 page per cluster.
		 * Technically, this over-allocates metadata
		 * because more metadata will reduce the number
		 * of usable clusters. This can be addressed with
		 * more complex math in the future.
		 */
		bs->md_len = bs->total_clusters;
	} else {
		bs->md_len = opts.num_md_pages;
	}

	rc = spdk_bit_array_resize(&bs->used_md_pages, bs->md_len);
	if (rc < 0) {
		_spdk_bs_free(bs);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		_spdk_bs_free(bs);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	ctx->bs = bs;

	/* Allocate memory for the super block */
	ctx->super = spdk_dma_zmalloc(sizeof(*ctx->super), 0x1000, NULL);
	if (!ctx->super) {
		free(ctx);
		_spdk_bs_free(bs);
		return;
	}
	memcpy(ctx->super->signature, SPDK_BS_SUPER_BLOCK_SIG,
	       sizeof(ctx->super->signature));
	ctx->super->version = SPDK_BS_VERSION;
	ctx->super->length = sizeof(*ctx->super);
	ctx->super->super_blob = bs->super_blob;
	ctx->super->clean = 0;
	ctx->super->cluster_size = bs->cluster_sz;

	/* Calculate how many pages the metadata consumes at the front
	 * of the disk.
	 */

	/* The super block uses 1 page */
	num_md_pages = 1;

	/* The used_md_pages mask requires 1 bit per metadata page, rounded
	 * up to the nearest page, plus a header.
	 */
	ctx->super->used_page_mask_start = num_md_pages;
	ctx->super->used_page_mask_len = divide_round_up(sizeof(struct spdk_bs_md_mask) +
					 divide_round_up(bs->md_len, 8),
					 SPDK_BS_PAGE_SIZE);
	num_md_pages += ctx->super->used_page_mask_len;

	/* The used_clusters mask requires 1 bit per cluster, rounded
	 * up to the nearest page, plus a header.
	 */
	ctx->super->used_cluster_mask_start = num_md_pages;
	ctx->super->used_cluster_mask_len = divide_round_up(sizeof(struct spdk_bs_md_mask) +
					    divide_round_up(bs->total_clusters, 8),
					    SPDK_BS_PAGE_SIZE);
	num_md_pages += ctx->super->used_cluster_mask_len;

	/* The metadata region size was chosen above */
	ctx->super->md_start = bs->md_start = num_md_pages;
	ctx->super->md_len = bs->md_len;
	num_md_pages += bs->md_len;

	ctx->super->crc = _spdk_blob_md_page_calc_crc(ctx->super);

	num_md_clusters = divide_round_up(num_md_pages, bs->pages_per_cluster);
	if (num_md_clusters > bs->total_clusters) {
		SPDK_ERRLOG("Blobstore metadata cannot use more clusters than is available, "
			    "please decrease number of pages reserved for metadata "
			    "or increase cluster size.\n");
		spdk_dma_free(ctx->super);
		free(ctx);
		_spdk_bs_free(bs);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}
	/* Claim all of the clusters used by the metadata */
	for (i = 0; i < num_md_clusters; i++) {
		_spdk_bs_claim_cluster(bs, i);
	}

	cpl.type = SPDK_BS_CPL_TYPE_BS_HANDLE;
	cpl.u.bs_handle.cb_fn = cb_fn;
	cpl.u.bs_handle.cb_arg = cb_arg;
	cpl.u.bs_handle.bs = bs;

	seq = spdk_bs_sequence_start(bs->md_target.md_channel, &cpl);
	if (!seq) {
		spdk_dma_free(ctx->super);
		free(ctx);
		_spdk_bs_free(bs);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	/* TRIM the entire device */
	spdk_bs_sequence_unmap(seq, 0, bs->dev->blockcnt, _spdk_bs_init_trim_cpl_ver1, ctx);
}

static void
_spdk_bs_unload_write_super_cpl_ver1(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_unload_ctx_ver1	*ctx = cb_arg;

	spdk_dma_free(ctx->super);

	/*
	 * We need to defer calling spdk_bs_call_cpl() until after
	 * dev destuction, so tuck these away for later use.
	 */
	ctx->bs->unload_err = bserrno;
	memcpy(&ctx->bs->unload_cpl, &seq->cpl, sizeof(struct spdk_bs_cpl));
	seq->cpl.type = SPDK_BS_CPL_TYPE_NONE;

	spdk_bs_sequence_finish(seq, bserrno);

	_spdk_bs_free(ctx->bs);
	free(ctx);
}

static void
_spdk_bs_unload_write_used_clusters_cpl_ver1(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_unload_ctx_ver1	*ctx = cb_arg;

	spdk_dma_free(ctx->mask);

	/* Update the values in the super block */
	ctx->super->super_blob = ctx->bs->super_blob;
	ctx->super->clean = 1;
	ctx->super->crc = _spdk_blob_md_page_calc_crc(ctx->super);
	spdk_bs_sequence_write(seq, ctx->super, _spdk_bs_page_to_lba(ctx->bs, 0),
			       _spdk_bs_byte_to_lba(ctx->bs, sizeof(*ctx->super)),
			       _spdk_bs_unload_write_super_cpl_ver1, ctx);
}

static void
_spdk_bs_unload_write_used_pages_cpl_ver1(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_unload_ctx_ver1	*ctx = cb_arg;
	uint64_t			lba, lba_count, mask_size;

	spdk_dma_free(ctx->mask);

	/* Write out the used clusters mask */
	mask_size = ctx->super->used_cluster_mask_len * SPDK_BS_PAGE_SIZE;
	ctx->mask = spdk_dma_zmalloc(mask_size, 0x1000, NULL);
	if (!ctx->mask) {
		spdk_dma_free(ctx->super);
		free(ctx);
		spdk_bs_sequence_finish(seq, -ENOMEM);
		return;
	}

	ctx->mask->type = SPDK_MD_MASK_TYPE_USED_CLUSTERS;
	ctx->mask->length = ctx->bs->total_clusters;
	assert(ctx->mask->length == spdk_bit_array_capacity(ctx->bs->used_clusters));

	_spdk_bs_set_mask(ctx->bs->used_clusters, ctx->mask);
	lba = _spdk_bs_page_to_lba(ctx->bs, ctx->super->used_cluster_mask_start);
	lba_count = _spdk_bs_page_to_lba(ctx->bs, ctx->super->used_cluster_mask_len);
	spdk_bs_sequence_write(seq, ctx->mask, lba, lba_count,
			       _spdk_bs_unload_write_used_clusters_cpl_ver1, ctx);
}

static void
_spdk_bs_unload_read_super_cpl_ver1(spdk_bs_sequence_t *seq, void *cb_arg, int bserrno)
{
	struct spdk_bs_unload_ctx_ver1	*ctx = cb_arg;
	uint64_t			lba, lba_count, mask_size;

	/* Write out the used page mask */
	mask_size = ctx->super->used_page_mask_len * SPDK_BS_PAGE_SIZE;
	ctx->mask = spdk_dma_zmalloc(mask_size, 0x1000, NULL);
	if (!ctx->mask) {
		spdk_dma_free(ctx->super);
		free(ctx);
		spdk_bs_sequence_finish(seq, -ENOMEM);
		return;
	}

	ctx->mask->type = SPDK_MD_MASK_TYPE_USED_PAGES;
	ctx->mask->length = ctx->super->md_len;
	assert(ctx->mask->length == spdk_bit_array_capacity(ctx->bs->used_md_pages));

	_spdk_bs_set_mask(ctx->bs->used_md_pages, ctx->mask);
	lba = _spdk_bs_page_to_lba(ctx->bs, ctx->super->used_page_mask_start);
	lba_count = _spdk_bs_page_to_lba(ctx->bs, ctx->super->used_page_mask_len);
	spdk_bs_sequence_write(seq, ctx->mask, lba, lba_count,
			       _spdk_bs_unload_write_used_pages_cpl_ver1, ctx);
}

static void
spdk_bs_unload_ver1(struct spdk_blob_store *bs, spdk_bs_op_complete cb_fn, void *cb_arg)
{
	struct spdk_bs_cpl	cpl;
	spdk_bs_sequence_t	*seq;
	struct spdk_bs_unload_ctx_ver1 *ctx;

	SPDK_DEBUGLOG(SPDK_TRACE_BLOB, "Syncing blobstore\n");

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->bs = bs;

	ctx->super = spdk_dma_zmalloc(sizeof(*ctx->super), 0x1000, NULL);
	if (!ctx->super) {
		free(ctx);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	cpl.type = SPDK_BS_CPL_TYPE_BS_BASIC;
	cpl.u.bs_basic.cb_fn = cb_fn;
	cpl.u.bs_basic.cb_arg = cb_arg;

	seq = spdk_bs_sequence_start(bs->md_target.md_channel, &cpl);
	if (!seq) {
		spdk_dma_free(ctx->super);
		free(ctx);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	assert(TAILQ_EMPTY(&bs->blobs));

	/* Read super block */
	spdk_bs_sequence_read(seq, ctx->super, _spdk_bs_page_to_lba(bs, 0),
			      _spdk_bs_byte_to_lba(bs, sizeof(*ctx->super)),
			      _spdk_bs_unload_read_super_cpl_ver1, ctx);
}

static void
_bs_send_msg(spdk_thread_fn fn, void *ctx, void *thread_ctx)
{
	if (g_scheduler_delay) {
		struct scheduled_ops *ops = calloc(1, sizeof(*ops));

		SPDK_CU_ASSERT_FATAL(ops != NULL);
		ops->fn = fn;
		ops->ctx = ctx;
		TAILQ_INSERT_TAIL(&g_scheduled_ops, ops, ops_queue);
	} else {
		fn(ctx);
	}
}

static void
_bs_flush_scheduler(void)
{
	struct scheduled_ops *ops, *tmp;

	TAILQ_FOREACH_SAFE(ops, &g_scheduled_ops, ops_queue, tmp) {
		ops->fn(ops->ctx);
		TAILQ_REMOVE(&g_scheduled_ops, ops, ops_queue);
		free(ops);
	}
}

static void
bs_op_complete(void *cb_arg, int bserrno)
{
	g_bserrno = bserrno;
}

static void
bs_op_with_handle_complete(void *cb_arg, struct spdk_blob_store *bs,
			   int bserrno)
{
	g_bs = bs;
	g_bserrno = bserrno;
}

static void
blob_op_complete(void *cb_arg, int bserrno)
{
	g_bserrno = bserrno;
}

static void
blob_op_with_id_complete(void *cb_arg, spdk_blob_id blobid, int bserrno)
{
	g_blobid = blobid;
	g_bserrno = bserrno;
}

static void
blob_op_with_handle_complete(void *cb_arg, struct spdk_blob *blb, int bserrno)
{
	g_blob = blb;
	g_bserrno = bserrno;
}

static void
blob_init(void)
{
	struct spdk_bs_dev *dev;

	dev = init_dev();

	/* should fail for an unsupported blocklen */
	dev->blocklen = 500;
	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == -EINVAL);

	dev = init_dev();
	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
blob_super(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	spdk_blob_id blobid;

	dev = init_dev();

	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	/* Get the super blob without having set one */
	spdk_bs_get_super(bs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == -ENOENT);
	CU_ASSERT(g_blobid == SPDK_BLOBID_INVALID);

	/* Create a blob */
	spdk_bs_md_create_blob(bs,
			       blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid !=  SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	/* Set the blob as the super blob */
	spdk_bs_set_super(bs, blobid, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* Get the super blob */
	spdk_bs_get_super(bs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(blobid == g_blobid);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
blob_open(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_blob *blob;
	spdk_blob_id blobid, blobid2;

	dev = init_dev();

	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	spdk_bs_md_create_blob(bs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_md_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;

	blobid2 = spdk_blob_get_id(blob);
	CU_ASSERT(blobid == blobid2);

	/* Try to open file again.  It should return success. */
	spdk_bs_md_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(blob == g_blob);

	spdk_bs_md_close_blob(&blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(blob == NULL);

	/*
	 * Close the file a second time, releasing the second reference.  This
	 *  should succeed.
	 */
	blob = g_blob;
	spdk_bs_md_close_blob(&blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/*
	 * Try to open file again.  It should succeed.  This tests the case
	 *  where the file is opened, closed, then re-opened again.
	 */
	spdk_bs_md_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;

	spdk_bs_md_close_blob(&blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
blob_delete(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	spdk_blob_id blobid;

	dev = init_dev();

	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	/* Create a blob and then delete it. */
	spdk_bs_md_create_blob(bs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid > 0);
	blobid = g_blobid;

	spdk_bs_md_delete_blob(bs, blobid, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* Try to open the blob */
	spdk_bs_md_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == -ENOENT);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
blob_resize(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_blob *blob;
	spdk_blob_id blobid;
	uint64_t free_clusters;
	int rc;

	dev = init_dev();

	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;
	free_clusters = spdk_bs_free_cluster_count(bs);

	spdk_bs_md_create_blob(bs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	blobid = g_blobid;

	spdk_bs_md_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;

	/* The blob started at 0 clusters. Resize it to be 5. */
	rc = spdk_bs_md_resize_blob(blob, 5);
	CU_ASSERT(rc == 0);
	CU_ASSERT((free_clusters - 5) == spdk_bs_free_cluster_count(bs));

	/* Shrink the blob to 3 clusters. This will not actually release
	 * the old clusters until the blob is synced.
	 */
	rc = spdk_bs_md_resize_blob(blob, 3);
	CU_ASSERT(rc == 0);
	/* Verify there are still 5 clusters in use */
	CU_ASSERT((free_clusters - 5) == spdk_bs_free_cluster_count(bs));

	spdk_bs_md_sync_blob(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	/* Now there are only 3 clusters in use */
	CU_ASSERT((free_clusters - 3) == spdk_bs_free_cluster_count(bs));

	/* Resize the blob to be 10 clusters. Growth takes effect immediately. */
	rc = spdk_bs_md_resize_blob(blob, 10);
	CU_ASSERT(rc == 0);
	CU_ASSERT((free_clusters - 10) == spdk_bs_free_cluster_count(bs));

	spdk_bs_md_close_blob(&blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_md_delete_blob(bs, blobid, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
channel_ops(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_io_channel *channel;

	dev = init_dev();

	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	channel = spdk_bs_alloc_io_channel(bs);
	CU_ASSERT(channel != NULL);

	spdk_bs_free_io_channel(channel);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
blob_write(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_blob *blob;
	struct spdk_io_channel *channel;
	spdk_blob_id blobid;
	uint64_t pages_per_cluster;
	uint8_t payload[10 * 4096];
	int rc;

	dev = init_dev();

	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	pages_per_cluster = spdk_bs_get_cluster_size(bs) / spdk_bs_get_page_size(bs);

	channel = spdk_bs_alloc_io_channel(bs);
	CU_ASSERT(channel != NULL);

	spdk_bs_md_create_blob(bs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_md_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;

	/* Write to a blob with 0 size */
	spdk_bs_io_write_blob(blob, channel, payload, 0, 1, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == -EINVAL);

	/* Resize the blob */
	rc = spdk_bs_md_resize_blob(blob, 5);
	CU_ASSERT(rc == 0);

	/* Write to the blob */
	spdk_bs_io_write_blob(blob, channel, payload, 0, 1, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* Write starting beyond the end */
	spdk_bs_io_write_blob(blob, channel, payload, 5 * pages_per_cluster, 1, blob_op_complete,
			      NULL);
	CU_ASSERT(g_bserrno == -EINVAL);

	/* Write starting at a valid location but going off the end */
	spdk_bs_io_write_blob(blob, channel, payload, 4 * pages_per_cluster, pages_per_cluster + 1,
			      blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == -EINVAL);

	spdk_bs_md_close_blob(&blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_free_io_channel(channel);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
blob_read(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_blob *blob;
	struct spdk_io_channel *channel;
	spdk_blob_id blobid;
	uint64_t pages_per_cluster;
	uint8_t payload[10 * 4096];
	int rc;

	dev = init_dev();

	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	pages_per_cluster = spdk_bs_get_cluster_size(bs) / spdk_bs_get_page_size(bs);

	channel = spdk_bs_alloc_io_channel(bs);
	CU_ASSERT(channel != NULL);

	spdk_bs_md_create_blob(bs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_md_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;

	/* Read from a blob with 0 size */
	spdk_bs_io_read_blob(blob, channel, payload, 0, 1, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == -EINVAL);

	/* Resize the blob */
	rc = spdk_bs_md_resize_blob(blob, 5);
	CU_ASSERT(rc == 0);

	/* Read from the blob */
	spdk_bs_io_read_blob(blob, channel, payload, 0, 1, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* Read starting beyond the end */
	spdk_bs_io_read_blob(blob, channel, payload, 5 * pages_per_cluster, 1, blob_op_complete,
			     NULL);
	CU_ASSERT(g_bserrno == -EINVAL);

	/* Read starting at a valid location but going off the end */
	spdk_bs_io_read_blob(blob, channel, payload, 4 * pages_per_cluster, pages_per_cluster + 1,
			     blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == -EINVAL);

	spdk_bs_md_close_blob(&blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_free_io_channel(channel);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
blob_rw_verify(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_blob *blob;
	struct spdk_io_channel *channel;
	spdk_blob_id blobid;
	uint8_t payload_read[10 * 4096];
	uint8_t payload_write[10 * 4096];
	int rc;

	dev = init_dev();

	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	channel = spdk_bs_alloc_io_channel(bs);
	CU_ASSERT(channel != NULL);

	spdk_bs_md_create_blob(bs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_md_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;

	rc = spdk_bs_md_resize_blob(blob, 32);
	CU_ASSERT(rc == 0);

	memset(payload_write, 0xE5, sizeof(payload_write));
	spdk_bs_io_write_blob(blob, channel, payload_write, 4, 10, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	memset(payload_read, 0x00, sizeof(payload_read));
	spdk_bs_io_read_blob(blob, channel, payload_read, 4, 10, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_write, payload_read, 4 * 4096) == 0);

	spdk_bs_md_close_blob(&blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_free_io_channel(channel);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
blob_rw_verify_iov(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_blob *blob;
	struct spdk_io_channel *channel;
	spdk_blob_id blobid;
	uint8_t payload_read[10 * 4096];
	uint8_t payload_write[10 * 4096];
	struct iovec iov_read[3];
	struct iovec iov_write[3];
	void *buf;
	int rc;

	dev = init_dev();
	memset(g_dev_buffer, 0, DEV_BUFFER_SIZE);

	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	channel = spdk_bs_alloc_io_channel(bs);
	CU_ASSERT(channel != NULL);

	spdk_bs_md_create_blob(bs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_md_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;

	rc = spdk_bs_md_resize_blob(blob, 2);
	CU_ASSERT(rc == 0);

	/*
	 * Manually adjust the offset of the blob's second cluster.  This allows
	 *  us to make sure that the readv/write code correctly accounts for I/O
	 *  that cross cluster boundaries.  Start by asserting that the allocated
	 *  clusters are where we expect before modifying the second cluster.
	 */
	CU_ASSERT(blob->active.clusters[0] == 1 * 256);
	CU_ASSERT(blob->active.clusters[1] == 2 * 256);
	blob->active.clusters[1] = 3 * 256;

	memset(payload_write, 0xE5, sizeof(payload_write));
	iov_write[0].iov_base = payload_write;
	iov_write[0].iov_len = 1 * 4096;
	iov_write[1].iov_base = payload_write + 1 * 4096;
	iov_write[1].iov_len = 5 * 4096;
	iov_write[2].iov_base = payload_write + 6 * 4096;
	iov_write[2].iov_len = 4 * 4096;
	/*
	 * Choose a page offset just before the cluster boundary.  The first 6 pages of payload
	 *  will get written to the first cluster, the last 4 to the second cluster.
	 */
	spdk_bs_io_writev_blob(blob, channel, iov_write, 3, 250, 10, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	memset(payload_read, 0xAA, sizeof(payload_read));
	iov_read[0].iov_base = payload_read;
	iov_read[0].iov_len = 3 * 4096;
	iov_read[1].iov_base = payload_read + 3 * 4096;
	iov_read[1].iov_len = 4 * 4096;
	iov_read[2].iov_base = payload_read + 7 * 4096;
	iov_read[2].iov_len = 3 * 4096;
	spdk_bs_io_readv_blob(blob, channel, iov_read, 3, 250, 10, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_write, payload_read, 10 * 4096) == 0);

	buf = calloc(1, 256 * 4096);
	SPDK_CU_ASSERT_FATAL(buf != NULL);
	/* Check that cluster 2 on "disk" was not modified. */
	CU_ASSERT(memcmp(buf, &g_dev_buffer[512 * 4096], 256 * 4096) == 0);
	free(buf);

	spdk_bs_md_close_blob(&blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_free_io_channel(channel);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static uint32_t
bs_channel_get_req_count(struct spdk_io_channel *_channel)
{
	struct spdk_bs_channel *channel = spdk_io_channel_get_ctx(_channel);
	struct spdk_bs_request_set *set;
	uint32_t count = 0;

	TAILQ_FOREACH(set, &channel->reqs, link) {
		count++;
	}

	return count;
}

static void
blob_rw_verify_iov_nomem(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_blob *blob;
	struct spdk_io_channel *channel;
	spdk_blob_id blobid;
	uint8_t payload_write[10 * 4096];
	struct iovec iov_write[3];
	uint32_t req_count;
	int rc;

	dev = init_dev();
	memset(g_dev_buffer, 0, DEV_BUFFER_SIZE);

	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	channel = spdk_bs_alloc_io_channel(bs);
	CU_ASSERT(channel != NULL);

	spdk_bs_md_create_blob(bs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_md_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;

	rc = spdk_bs_md_resize_blob(blob, 2);
	CU_ASSERT(rc == 0);

	/*
	 * Choose a page offset just before the cluster boundary.  The first 6 pages of payload
	 *  will get written to the first cluster, the last 4 to the second cluster.
	 */
	iov_write[0].iov_base = payload_write;
	iov_write[0].iov_len = 1 * 4096;
	iov_write[1].iov_base = payload_write + 1 * 4096;
	iov_write[1].iov_len = 5 * 4096;
	iov_write[2].iov_base = payload_write + 6 * 4096;
	iov_write[2].iov_len = 4 * 4096;
	MOCK_SET(calloc, void *, NULL);
	req_count = bs_channel_get_req_count(channel);
	spdk_bs_io_writev_blob(blob, channel, iov_write, 3, 250, 10, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno = -ENOMEM);
	CU_ASSERT(req_count == bs_channel_get_req_count(channel));
	MOCK_SET(calloc, void *, (void *)MOCK_PASS_THRU);

	spdk_bs_md_close_blob(&blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_free_io_channel(channel);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
blob_iter(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_blob *blob;
	spdk_blob_id blobid;

	dev = init_dev();

	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	spdk_bs_md_iter_first(bs, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_blob == NULL);
	CU_ASSERT(g_bserrno == -ENOENT);

	spdk_bs_md_create_blob(bs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_md_iter_first(bs, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_blob != NULL);
	CU_ASSERT(g_bserrno == 0);
	blob = g_blob;
	CU_ASSERT(spdk_blob_get_id(blob) == blobid);

	spdk_bs_md_iter_next(bs, &blob, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_blob == NULL);
	CU_ASSERT(g_bserrno == -ENOENT);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
blob_xattr(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_blob *blob;
	spdk_blob_id blobid;
	uint64_t length;
	int rc;
	const char *name1, *name2;
	const void *value;
	size_t value_len;
	struct spdk_xattr_names *names;

	dev = init_dev();

	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	spdk_bs_md_create_blob(bs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_md_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;

	rc = spdk_blob_md_set_xattr(blob, "name", "log.txt", strlen("log.txt") + 1);
	CU_ASSERT(rc == 0);

	length = 2345;
	rc = spdk_blob_md_set_xattr(blob, "length", &length, sizeof(length));
	CU_ASSERT(rc == 0);

	/* Overwrite "length" xattr. */
	length = 3456;
	rc = spdk_blob_md_set_xattr(blob, "length", &length, sizeof(length));
	CU_ASSERT(rc == 0);

	value = NULL;
	rc = spdk_bs_md_get_xattr_value(blob, "length", &value, &value_len);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(*(uint64_t *)value == length);
	CU_ASSERT(value_len == 8);

	rc = spdk_bs_md_get_xattr_value(blob, "foobar", &value, &value_len);
	CU_ASSERT(rc == -ENOENT);

	names = NULL;
	rc = spdk_bs_md_get_xattr_names(blob, &names);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(names != NULL);
	CU_ASSERT(spdk_xattr_names_get_count(names) == 2);
	name1 = spdk_xattr_names_get_name(names, 0);
	SPDK_CU_ASSERT_FATAL(name1 != NULL);
	CU_ASSERT(!strcmp(name1, "name") || !strcmp(name1, "length"));
	name2 = spdk_xattr_names_get_name(names, 1);
	SPDK_CU_ASSERT_FATAL(name2 != NULL);
	CU_ASSERT(!strcmp(name2, "name") || !strcmp(name2, "length"));
	CU_ASSERT(strcmp(name1, name2));
	spdk_xattr_names_free(names);

	rc = spdk_blob_md_remove_xattr(blob, "name");
	CU_ASSERT(rc == 0);

	rc = spdk_blob_md_remove_xattr(blob, "foobar");
	CU_ASSERT(rc == -ENOENT);

	spdk_bs_md_close_blob(&blob, blob_op_complete, NULL);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
bs_load(void)
{
	struct spdk_bs_dev *dev;
	spdk_blob_id blobid;
	struct spdk_blob *blob;
	struct spdk_bs_super_block *super_blob;
	uint64_t length;
	int rc;
	const void *value;
	size_t value_len;
	struct spdk_bs_opts opts;

	g_scheduler_delay = true;

	dev = init_dev();
	spdk_bs_opts_init(&opts);
	strncpy(opts.bstype.bstype, "TESTTYPE", SPDK_BLOBSTORE_TYPE_LENGTH);

	/* Initialize a new blob store */
	spdk_bs_init(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	/* Try to open a blobid that does not exist */
	spdk_bs_md_open_blob(g_bs, 0, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == -ENOENT);
	CU_ASSERT(g_blob == NULL);

	/* Create a blob */
	spdk_bs_md_create_blob(g_bs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_md_open_blob(g_bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;

	/* Try again to open valid blob but without the upper bit set */
	spdk_bs_md_open_blob(g_bs, blobid & 0xFFFFFFFF, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == -ENOENT);
	CU_ASSERT(g_blob == NULL);

	/* Set some xattrs */
	rc = spdk_blob_md_set_xattr(blob, "name", "log.txt", strlen("log.txt") + 1);
	CU_ASSERT(rc == 0);

	length = 2345;
	rc = spdk_blob_md_set_xattr(blob, "length", &length, sizeof(length));
	CU_ASSERT(rc == 0);

	/* Resize the blob */
	rc = spdk_bs_md_resize_blob(blob, 10);
	CU_ASSERT(rc == 0);

	spdk_bs_md_close_blob(&blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	blob = NULL;
	g_blob = NULL;
	g_blobid = SPDK_BLOBID_INVALID;

	/* Unload the blob store */
	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
	g_blob = NULL;
	g_blobid = 0;

	super_blob = (struct spdk_bs_super_block *)g_dev_buffer;
	CU_ASSERT(super_blob->clean == 1);


	/* Load an existing blob store */
	dev = init_dev();
	strncpy(opts.bstype.bstype, "TESTTYPE", SPDK_BLOBSTORE_TYPE_LENGTH);
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	super_blob = (struct spdk_bs_super_block *)g_dev_buffer;
	CU_ASSERT(super_blob->clean == 0);

	spdk_bs_md_open_blob(g_bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;

	/* Get the xattrs */
	value = NULL;
	rc = spdk_bs_md_get_xattr_value(blob, "length", &value, &value_len);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(*(uint64_t *)value == length);
	CU_ASSERT(value_len == 8);

	rc = spdk_bs_md_get_xattr_value(blob, "foobar", &value, &value_len);
	CU_ASSERT(rc == -ENOENT);

	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 10);

	spdk_bs_md_close_blob(&blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	blob = NULL;
	g_blob = NULL;
	g_blobid = SPDK_BLOBID_INVALID;

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;

	/* Load non existing blobstore type */
	dev = init_dev();
	strncpy(opts.bstype.bstype, "NONEXISTING", SPDK_BLOBSTORE_TYPE_LENGTH);
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno != 0);

	/* Load with empty blobstore type */
	dev = init_dev();
	strncpy(opts.bstype.bstype, "", SPDK_BLOBSTORE_TYPE_LENGTH);
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;

	/* Initialize a new blob store with empty bstype */
	dev = init_dev();
	strncpy(opts.bstype.bstype, "", SPDK_BLOBSTORE_TYPE_LENGTH);
	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;

	/* Load non existing blobstore type */
	dev = init_dev();
	strncpy(opts.bstype.bstype, "NONEXISTING", SPDK_BLOBSTORE_TYPE_LENGTH);
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno != 0);

	/* Load with empty blobstore type */
	dev = init_dev();
	strncpy(opts.bstype.bstype, "", SPDK_BLOBSTORE_TYPE_LENGTH);
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;

	/* Load an existing blob store with version newer than supported */
	super_blob = (struct spdk_bs_super_block *)g_dev_buffer;
	super_blob->version++;

	dev = init_dev();
	strncpy(opts.bstype.bstype, "", SPDK_BLOBSTORE_TYPE_LENGTH);
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno != 0);

	/* Initialize a new blob store with super block version 1 */
	dev = init_dev();
	spdk_bs_init_ver1(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	spdk_bs_unload_ver1(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;

	/* Load an existing blob store using current version */
	dev = init_dev();
	strncpy(opts.bstype.bstype, "", SPDK_BLOBSTORE_TYPE_LENGTH);
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
	g_scheduler_delay = false;
}

/*
 * Create a blobstore and then unload it.
 */
static void
bs_unload(void)
{
	struct spdk_bs_dev *dev;

	dev = init_dev();

	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	g_bserrno = -1;
	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

/*
 * Create a blobstore with a cluster size different than the default, and ensure it is
 *  persisted.
 */
static void
bs_cluster_sz(void)
{
	struct spdk_bs_dev *dev;
	struct spdk_bs_opts opts;
	uint32_t cluster_sz;

	/* Set cluster size to zero */
	dev = init_dev();
	spdk_bs_opts_init(&opts);
	opts.cluster_sz = 0;

	/* Initialize a new blob store */
	spdk_bs_init(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == -EINVAL);
	SPDK_CU_ASSERT_FATAL(g_bs == NULL);

	/*
	 * Set cluster size to blobstore page size,
	 * to work it is required to be at least twice the blobstore page size.
	 */
	dev = init_dev();
	spdk_bs_opts_init(&opts);
	opts.cluster_sz = SPDK_BS_PAGE_SIZE;

	/* Initialize a new blob store */
	spdk_bs_init(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == -ENOMEM);
	SPDK_CU_ASSERT_FATAL(g_bs == NULL);

	/* Set cluster size to twice the default */
	dev = init_dev();
	spdk_bs_opts_init(&opts);
	opts.cluster_sz *= 2;
	cluster_sz = opts.cluster_sz;

	/* Initialize a new blob store */
	spdk_bs_init(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	CU_ASSERT(spdk_bs_get_cluster_size(g_bs) == cluster_sz);

	/* Unload the blob store */
	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
	g_blob = NULL;
	g_blobid = 0;

	dev = init_dev();
	/* Load an existing blob store */
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	CU_ASSERT(spdk_bs_get_cluster_size(g_bs) == cluster_sz);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

/*
 * Test resizing of the metadata blob.  This requires creating enough blobs
 *  so that one cluster is not enough to fit the metadata for those blobs.
 *  To induce this condition to happen more quickly, we reduce the cluster
 *  size to 16KB, which means only 4 4KB blob metadata pages can fit.
 */
static void
bs_resize_md(void)
{
	const int CLUSTER_PAGE_COUNT = 4;
	const int NUM_BLOBS = CLUSTER_PAGE_COUNT * 4;
	struct spdk_bs_dev *dev;
	struct spdk_bs_opts opts;
	uint32_t cluster_sz;
	spdk_blob_id blobids[NUM_BLOBS];
	int i;


	dev = init_dev();
	spdk_bs_opts_init(&opts);
	opts.cluster_sz = CLUSTER_PAGE_COUNT * 4096;
	cluster_sz = opts.cluster_sz;

	/* Initialize a new blob store */
	spdk_bs_init(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	CU_ASSERT(spdk_bs_get_cluster_size(g_bs) == cluster_sz);

	for (i = 0; i < NUM_BLOBS; i++) {
		g_bserrno = -1;
		g_blobid = SPDK_BLOBID_INVALID;
		spdk_bs_md_create_blob(g_bs,
				       blob_op_with_id_complete, NULL);
		CU_ASSERT(g_bserrno == 0);
		CU_ASSERT(g_blobid !=  SPDK_BLOBID_INVALID);
		blobids[i] = g_blobid;
	}

	/* Unload the blob store */
	g_bserrno = -1;
	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* Load an existing blob store */
	g_bserrno = -1;
	g_bs = NULL;
	dev = init_dev();
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	CU_ASSERT(spdk_bs_get_cluster_size(g_bs) == cluster_sz);

	for (i = 0; i < NUM_BLOBS; i++) {
		g_bserrno = -1;
		g_blob = NULL;
		spdk_bs_md_open_blob(g_bs, blobids[i], blob_op_with_handle_complete, NULL);
		CU_ASSERT(g_bserrno == 0);
		CU_ASSERT(g_blob !=  NULL);
		g_bserrno = -1;
		spdk_bs_md_close_blob(&g_blob, blob_op_complete, NULL);
		CU_ASSERT(g_bserrno == 0);
	}

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

/* Try to hit all of the corner cases associated with serializing
 * a blob to disk
 */
static void
blob_serialize(void)
{
	struct spdk_bs_dev *dev;
	struct spdk_bs_opts opts;
	struct spdk_blob_store *bs;
	spdk_blob_id blobid[2];
	struct spdk_blob *blob[2];
	uint64_t i;
	char *value;
	int rc;

	dev = init_dev();

	/* Initialize a new blobstore with very small clusters */
	spdk_bs_opts_init(&opts);
	opts.cluster_sz = dev->blocklen * 8;
	spdk_bs_init(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	/* Create and open two blobs */
	for (i = 0; i < 2; i++) {
		spdk_bs_md_create_blob(bs, blob_op_with_id_complete, NULL);
		CU_ASSERT(g_bserrno == 0);
		CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
		blobid[i] = g_blobid;

		/* Open a blob */
		spdk_bs_md_open_blob(bs, blobid[i], blob_op_with_handle_complete, NULL);
		CU_ASSERT(g_bserrno == 0);
		CU_ASSERT(g_blob != NULL);
		blob[i] = g_blob;

		/* Set a fairly large xattr on both blobs to eat up
		 * metadata space
		 */
		value = calloc(dev->blocklen - 64, sizeof(char));
		SPDK_CU_ASSERT_FATAL(value != NULL);
		memset(value, i, dev->blocklen / 2);
		rc = spdk_blob_md_set_xattr(blob[i], "name", value, dev->blocklen - 64);
		CU_ASSERT(rc == 0);
		free(value);
	}

	/* Resize the blobs, alternating 1 cluster at a time.
	 * This thwarts run length encoding and will cause spill
	 * over of the extents.
	 */
	for (i = 0; i < 6; i++) {
		rc = spdk_bs_md_resize_blob(blob[i % 2], (i / 2) + 1);
		CU_ASSERT(rc == 0);
	}

	for (i = 0; i < 2; i++) {
		spdk_bs_md_sync_blob(blob[i], blob_op_complete, NULL);
		CU_ASSERT(g_bserrno == 0);
	}

	/* Close the blobs */
	for (i = 0; i < 2; i++) {
		spdk_bs_md_close_blob(&blob[i], blob_op_complete, NULL);
		CU_ASSERT(g_bserrno == 0);
	}

	/* Unload the blobstore */
	spdk_bs_unload(bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
	g_blob = NULL;
	g_blobid = 0;
	bs = NULL;

	dev = init_dev();
	/* Load an existing blob store */
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	for (i = 0; i < 2; i++) {
		blob[i] = NULL;

		spdk_bs_md_open_blob(bs, blobid[i], blob_op_with_handle_complete, NULL);
		CU_ASSERT(g_bserrno == 0);
		CU_ASSERT(g_blob != NULL);
		blob[i] = g_blob;

		CU_ASSERT(spdk_blob_get_num_clusters(blob[i]) == 3);

		spdk_bs_md_close_blob(&blob[i], blob_op_complete, NULL);
		CU_ASSERT(g_bserrno == 0);
	}

	spdk_bs_unload(bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
blob_crc(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_blob *blob;
	spdk_blob_id blobid;
	uint32_t page_num;
	int index;
	struct spdk_blob_md_page *page;

	dev = init_dev();

	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	spdk_bs_md_create_blob(bs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_md_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;

	spdk_bs_md_close_blob(&blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(blob == NULL);

	page_num = _spdk_bs_blobid_to_page(blobid);
	index = DEV_BUFFER_BLOCKLEN * (bs->md_start + page_num);
	page = (struct spdk_blob_md_page *)&g_dev_buffer[index];
	page->crc = 0;

	spdk_bs_md_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == -EINVAL);
	CU_ASSERT(g_blob == NULL);
	g_bserrno = 0;

	spdk_bs_md_delete_blob(bs, blobid, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == -EINVAL);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
super_block_crc(void)
{
	struct spdk_bs_dev *dev;
	struct spdk_bs_super_block *super_block;
	struct spdk_bs_opts opts;

	dev = init_dev();
	spdk_bs_opts_init(&opts);

	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;

	super_block = (struct spdk_bs_super_block *)g_dev_buffer;
	super_block->crc = 0;
	dev = init_dev();

	g_scheduler_delay = true;
	/* Load an existing blob store */
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);

	CU_ASSERT(g_bserrno == -EILSEQ);
	_bs_flush_scheduler();
	CU_ASSERT(TAILQ_EMPTY(&g_scheduled_ops));

	g_scheduler_delay = false;
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("blob", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "blob_init", blob_init) == NULL ||
		CU_add_test(suite, "blob_open", blob_open) == NULL ||
		CU_add_test(suite, "blob_delete", blob_delete) == NULL ||
		CU_add_test(suite, "blob_resize", blob_resize) == NULL ||
		CU_add_test(suite, "channel_ops", channel_ops) == NULL ||
		CU_add_test(suite, "blob_super", blob_super) == NULL ||
		CU_add_test(suite, "blob_write", blob_write) == NULL ||
		CU_add_test(suite, "blob_read", blob_read) == NULL ||
		CU_add_test(suite, "blob_rw_verify", blob_rw_verify) == NULL ||
		CU_add_test(suite, "blob_rw_verify_iov", blob_rw_verify_iov) == NULL ||
		CU_add_test(suite, "blob_rw_verify_iov_nomem", blob_rw_verify_iov_nomem) == NULL ||
		CU_add_test(suite, "blob_iter", blob_iter) == NULL ||
		CU_add_test(suite, "blob_xattr", blob_xattr) == NULL ||
		CU_add_test(suite, "bs_load", bs_load) == NULL ||
		CU_add_test(suite, "bs_unload", bs_unload) == NULL ||
		CU_add_test(suite, "bs_cluster_sz", bs_cluster_sz) == NULL ||
		CU_add_test(suite, "bs_resize_md", bs_resize_md) == NULL ||
		CU_add_test(suite, "blob_serialize", blob_serialize) == NULL ||
		CU_add_test(suite, "blob_crc", blob_crc) == NULL ||
		CU_add_test(suite, "super_block_crc", super_block_crc) == NULL
	) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	g_dev_buffer = calloc(1, DEV_BUFFER_SIZE);
	spdk_allocate_thread(_bs_send_msg, NULL, "thread0");
	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	spdk_free_thread();
	free(g_dev_buffer);
	return num_failures;
}
