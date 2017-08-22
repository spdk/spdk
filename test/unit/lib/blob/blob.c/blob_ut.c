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
	/*
	 * Normally dev gets deleted as part of the dev->destroy callback.  But
	 *  that doesn't get invoked when init() fails.  So manually free it here
	 *  instead.  Probably blobstore should still destroy the dev when init
	 *  fails, but we'll do that in a separate patch.
	 */
	free(dev);

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
	uint64_t length;
	int rc;
	const void *value;
	size_t value_len;

	dev = init_dev();

	/* Initialize a new blob store */
	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	/* Create a blob */
	spdk_bs_md_create_blob(g_bs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_md_open_blob(g_bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;

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

	dev = init_dev();
	/* Load an existing blob store */
	spdk_bs_load(dev, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

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
}

/*
 * Create a blobstore and then unload it, while delaying all scheduled operations
 * until after spdk_bs_unload call has finished.  This ensures the memory associated
 * with the internal blobstore channels is not touched after it is freed.
 */
static void
bs_unload_delayed(void)
{
	struct spdk_bs_dev *dev;

	dev = init_dev();

	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	g_scheduler_delay = true;

	g_bserrno = -1;
	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;

	_bs_flush_scheduler();
	CU_ASSERT(TAILQ_EMPTY(&g_scheduled_ops));

	g_scheduler_delay = false;
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
	spdk_bs_load(dev, bs_op_with_handle_complete, NULL);
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
	spdk_bs_load(dev, bs_op_with_handle_complete, NULL);
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
	spdk_bs_load(dev, bs_op_with_handle_complete, NULL);
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
		CU_add_test(suite, "bs_unload_delayed", bs_unload_delayed) == NULL ||
		CU_add_test(suite, "bs_cluster_sz", bs_cluster_sz) == NULL ||
		CU_add_test(suite, "bs_resize_md", bs_resize_md) == NULL ||
		CU_add_test(suite, "blob_serialize", blob_serialize) == NULL
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
