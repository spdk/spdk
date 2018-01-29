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
#include "zeroes.c"
#include "blob_bs_dev.c"

struct spdk_blob_store *g_bs;
spdk_blob_id g_blobid;
struct spdk_blob *g_blob;
int g_bserrno;
struct spdk_xattr_names *g_names;
int g_done;
char *g_xattr_names[] = {"first", "second", "third"};
char *g_xattr_values[] = {"one", "two", "three"};
uint64_t g_ctx = 1729;

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
	spdk_bs_create_blob(bs, blob_op_with_id_complete, NULL);
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

	spdk_bs_create_blob(bs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;

	blobid2 = spdk_blob_get_id(blob);
	CU_ASSERT(blobid == blobid2);

	/* Try to open file again.  It should return success. */
	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(blob == g_blob);

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/*
	 * Close the file a second time, releasing the second reference.  This
	 *  should succeed.
	 */
	blob = g_blob;
	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/*
	 * Try to open file again.  It should succeed.  This tests the case
	 *  where the file is opened, closed, then re-opened again.
	 */
	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
blob_create(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_blob *blob;
	struct spdk_blob_opts opts;
	spdk_blob_id blobid;

	dev = init_dev();

	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	/* Create blob with 10 clusters */

	spdk_blob_opts_init(&opts);
	opts.num_clusters = 10;

	spdk_bs_create_blob_ext(bs, &opts, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 10)

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* Create blob with 0 clusters */

	spdk_blob_opts_init(&opts);
	opts.num_clusters = 0;

	spdk_bs_create_blob_ext(bs, &opts, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 0)

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* Create blob with default options (opts == NULL) */

	spdk_bs_create_blob_ext(bs, NULL, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 0)

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* Try to create blob with size larger than blobstore */

	spdk_blob_opts_init(&opts);
	opts.num_clusters = bs->total_clusters + 1;

	spdk_bs_create_blob_ext(bs, &opts, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == -ENOSPC);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;

}

static void
blob_thin_provision(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_blob *blob;
	struct spdk_blob_opts opts;
	struct spdk_bs_opts bs_opts;
	spdk_blob_id blobid;

	dev = init_dev();
	spdk_bs_opts_init(&bs_opts);
	strncpy(bs_opts.bstype.bstype, "TESTTYPE", SPDK_BLOBSTORE_TYPE_LENGTH);

	/* Initialize a new blob store */
	spdk_bs_init(dev, &bs_opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	bs = g_bs;

	/* Create blob with thin provisioning enabled */

	spdk_blob_opts_init(&opts);
	opts.thin_provision = true;
	opts.num_clusters = 10;

	spdk_bs_create_blob_ext(bs, &opts, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;
	CU_ASSERT(__blob_to_data(blob)->invalid_flags & SPDK_BLOB_THIN_PROV);

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;

	/* Load an existing blob store and check if invalid_flags is set */
	dev = init_dev();
	strncpy(bs_opts.bstype.bstype, "TESTTYPE", SPDK_BLOBSTORE_TYPE_LENGTH);
	spdk_bs_load(dev, &bs_opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	bs = g_bs;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;
	CU_ASSERT(__blob_to_data(blob)->invalid_flags & SPDK_BLOB_THIN_PROV);

	spdk_blob_close(blob, blob_op_complete, NULL);
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
	spdk_bs_create_blob(bs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid > 0);
	blobid = g_blobid;

	spdk_bs_delete_blob(bs, blobid, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* Try to open the blob */
	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
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

	spdk_bs_create_blob(bs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;

	/* Confirm that resize fails if blob is marked read-only. */
	__blob_to_data(blob)->md_ro = true;
	rc = spdk_blob_resize(blob, 5);
	CU_ASSERT(rc == -EPERM);
	__blob_to_data(blob)->md_ro = false;

	/* The blob started at 0 clusters. Resize it to be 5. */
	rc = spdk_blob_resize(blob, 5);
	CU_ASSERT(rc == 0);
	CU_ASSERT((free_clusters - 5) == spdk_bs_free_cluster_count(bs));

	/* Shrink the blob to 3 clusters. This will not actually release
	 * the old clusters until the blob is synced.
	 */
	rc = spdk_blob_resize(blob, 3);
	CU_ASSERT(rc == 0);
	/* Verify there are still 5 clusters in use */
	CU_ASSERT((free_clusters - 5) == spdk_bs_free_cluster_count(bs));

	spdk_blob_sync_md(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	/* Now there are only 3 clusters in use */
	CU_ASSERT((free_clusters - 3) == spdk_bs_free_cluster_count(bs));

	/* Resize the blob to be 10 clusters. Growth takes effect immediately. */
	rc = spdk_blob_resize(blob, 10);
	CU_ASSERT(rc == 0);
	CU_ASSERT((free_clusters - 10) == spdk_bs_free_cluster_count(bs));

	/* Try to resize the blob to size larger than blobstore. */
	rc = spdk_blob_resize(blob, bs->total_clusters + 1);
	CU_ASSERT(rc == -ENOSPC);

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_delete_blob(bs, blobid, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
blob_read_only(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_blob *blob;
	struct spdk_blob_data *blob_data;
	struct spdk_bs_opts opts;
	spdk_blob_id blobid;

	dev = init_dev();
	spdk_bs_opts_init(&opts);
	strncpy(opts.bstype.bstype, "TESTTYPE", SPDK_BLOBSTORE_TYPE_LENGTH);

	spdk_bs_init(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	spdk_bs_create_blob(bs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;

	spdk_blob_set_read_only(blob);

	blob_data = __blob_to_data(blob);
	CU_ASSERT(blob_data->data_ro == false);
	CU_ASSERT(blob_data->md_ro == false);

	spdk_blob_sync_md(blob, bs_op_complete, NULL);

	CU_ASSERT(blob_data->data_ro == true);
	CU_ASSERT(blob_data->md_ro == true);
	CU_ASSERT(blob_data->data_ro_flags & SPDK_BLOB_READ_ONLY);

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;

	blob_data = __blob_to_data(blob);
	CU_ASSERT(blob_data->data_ro == true);
	CU_ASSERT(blob_data->md_ro == true);
	CU_ASSERT(blob_data->data_ro_flags & SPDK_BLOB_READ_ONLY);

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
	g_blob = NULL;
	g_blobid = 0;

	/* Load an existing blob store */
	dev = init_dev();
	strncpy(opts.bstype.bstype, "TESTTYPE", SPDK_BLOBSTORE_TYPE_LENGTH);
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	spdk_bs_open_blob(g_bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;

	blob_data = __blob_to_data(blob);
	CU_ASSERT(blob_data->data_ro == true);
	CU_ASSERT(blob_data->md_ro == true);
	CU_ASSERT(blob_data->data_ro_flags & SPDK_BLOB_READ_ONLY);

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

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

	spdk_bs_create_blob(bs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;

	/* Write to a blob with 0 size */
	spdk_bs_io_write_blob(blob, channel, payload, 0, 1, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == -EINVAL);

	/* Resize the blob */
	rc = spdk_blob_resize(blob, 5);
	CU_ASSERT(rc == 0);

	/* Confirm that write fails if blob is marked read-only. */
	__blob_to_data(blob)->data_ro = true;
	spdk_bs_io_write_blob(blob, channel, payload, 0, 1, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == -EPERM);
	__blob_to_data(blob)->data_ro = false;

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

	spdk_blob_close(blob, blob_op_complete, NULL);
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

	spdk_bs_create_blob(bs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;

	/* Read from a blob with 0 size */
	spdk_bs_io_read_blob(blob, channel, payload, 0, 1, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == -EINVAL);

	/* Resize the blob */
	rc = spdk_blob_resize(blob, 5);
	CU_ASSERT(rc == 0);

	/* Confirm that read passes if blob is marked read-only. */
	__blob_to_data(blob)->data_ro = true;
	spdk_bs_io_read_blob(blob, channel, payload, 0, 1, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	__blob_to_data(blob)->data_ro = false;

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

	spdk_blob_close(blob, blob_op_complete, NULL);
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

	spdk_bs_create_blob(bs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;

	rc = spdk_blob_resize(blob, 32);
	CU_ASSERT(rc == 0);

	memset(payload_write, 0xE5, sizeof(payload_write));
	spdk_bs_io_write_blob(blob, channel, payload_write, 4, 10, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	memset(payload_read, 0x00, sizeof(payload_read));
	spdk_bs_io_read_blob(blob, channel, payload_read, 4, 10, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_write, payload_read, 4 * 4096) == 0);

	spdk_blob_close(blob, blob_op_complete, NULL);
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

	spdk_bs_create_blob(bs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;

	rc = spdk_blob_resize(blob, 2);
	CU_ASSERT(rc == 0);

	/*
	 * Manually adjust the offset of the blob's second cluster.  This allows
	 *  us to make sure that the readv/write code correctly accounts for I/O
	 *  that cross cluster boundaries.  Start by asserting that the allocated
	 *  clusters are where we expect before modifying the second cluster.
	 */
	CU_ASSERT(__blob_to_data(blob)->active.clusters[0] == 1 * 256);
	CU_ASSERT(__blob_to_data(blob)->active.clusters[1] == 2 * 256);
	__blob_to_data(blob)->active.clusters[1] = 3 * 256;

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

	spdk_blob_close(blob, blob_op_complete, NULL);
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

	spdk_bs_create_blob(bs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;

	rc = spdk_blob_resize(blob, 2);
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

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_free_io_channel(channel);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
blob_rw_iov_read_only(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_blob *blob;
	struct spdk_io_channel *channel;
	spdk_blob_id blobid;
	uint8_t payload_read[4096];
	uint8_t payload_write[4096];
	struct iovec iov_read;
	struct iovec iov_write;
	int rc;

	dev = init_dev();
	memset(g_dev_buffer, 0, DEV_BUFFER_SIZE);

	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	channel = spdk_bs_alloc_io_channel(bs);
	CU_ASSERT(channel != NULL);

	spdk_bs_create_blob(bs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;

	rc = spdk_blob_resize(blob, 2);
	CU_ASSERT(rc == 0);

	/* Verify that writev failed if read_only flag is set. */
	__blob_to_data(blob)->data_ro = true;
	iov_write.iov_base = payload_write;
	iov_write.iov_len = sizeof(payload_write);
	spdk_bs_io_writev_blob(blob, channel, &iov_write, 1, 0, 1, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == -EPERM);

	/* Verify that reads pass if data_ro flag is set. */
	iov_read.iov_base = payload_read;
	iov_read.iov_len = sizeof(payload_read);
	spdk_bs_io_readv_blob(blob, channel, &iov_read, 1, 0, 1, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_free_io_channel(channel);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
blob_unmap(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_blob *blob;
	struct spdk_blob_data *_blob;
	struct spdk_io_channel *channel;
	spdk_blob_id blobid;
	struct spdk_blob_opts opts;
	uint8_t payload[4096];
	int rc;
	int i;

	dev = init_dev();

	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	channel = spdk_bs_alloc_io_channel(bs);
	CU_ASSERT(channel != NULL);

	spdk_blob_opts_init(&opts);
	opts.num_clusters = 10;

	spdk_bs_create_blob_ext(bs, &opts, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;
	_blob = __blob_to_data(blob);

	rc = spdk_blob_resize(blob, 10);
	CU_ASSERT(rc == 0);

	memset(payload, 0, sizeof(payload));
	payload[0] = 0xFF;

	/*
	 * Set first byte of every cluster to 0xFF.
	 * First cluster on device is reserved so let's start from cluster number 1
	 */
	for (i = 1; i < 11; i++) {
		g_dev_buffer[i * SPDK_BLOB_OPTS_CLUSTER_SZ] = 0xFF;
	}

	/* Confirm writes */
	for (i = 0; i < 10; i++) {
		payload[0] = 0;
		spdk_bs_io_read_blob(blob, channel, &payload, i * SPDK_BLOB_OPTS_CLUSTER_SZ / 4096, 1,
				     blob_op_complete, NULL);
		CU_ASSERT(g_bserrno == 0);
		CU_ASSERT(payload[0] == 0xFF);
	}

	/* Mark some clusters as unallocated */
	_blob->active.clusters[1] = 0;
	_blob->active.clusters[2] = 0;
	_blob->active.clusters[3] = 0;
	_blob->active.clusters[6] = 0;
	_blob->active.clusters[8] = 0;

	/* Unmap clusters by resizing to 0 */
	rc = spdk_blob_resize(blob, 0);
	CU_ASSERT(rc == 0);

	spdk_blob_sync_md(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* Confirm that only 'allocated' clusters were unmaped */
	for (i = 1; i < 11; i++) {
		switch (i) {
		case 2:
		case 3:
		case 4:
		case 7:
		case 9:
			CU_ASSERT(g_dev_buffer[i * SPDK_BLOB_OPTS_CLUSTER_SZ] == 0xFF);
			break;
		default:
			CU_ASSERT(g_dev_buffer[i * SPDK_BLOB_OPTS_CLUSTER_SZ] == 0);
			break;
		}
	}

	spdk_blob_close(blob, blob_op_complete, NULL);
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

	spdk_bs_iter_first(bs, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_blob == NULL);
	CU_ASSERT(g_bserrno == -ENOENT);

	spdk_bs_create_blob(bs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_iter_first(bs, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_blob != NULL);
	CU_ASSERT(g_bserrno == 0);
	blob = g_blob;
	CU_ASSERT(spdk_blob_get_id(blob) == blobid);

	spdk_bs_iter_next(bs, blob, blob_op_with_handle_complete, NULL);
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

	spdk_bs_create_blob(bs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;

	/* Test that set_xattr fails if md_ro flag is set. */
	__blob_to_data(blob)->md_ro = true;
	rc = spdk_blob_set_xattr(blob, "name", "log.txt", strlen("log.txt") + 1);
	CU_ASSERT(rc == -EPERM);

	__blob_to_data(blob)->md_ro = false;
	rc = spdk_blob_set_xattr(blob, "name", "log.txt", strlen("log.txt") + 1);
	CU_ASSERT(rc == 0);

	length = 2345;
	rc = spdk_blob_set_xattr(blob, "length", &length, sizeof(length));
	CU_ASSERT(rc == 0);

	/* Overwrite "length" xattr. */
	length = 3456;
	rc = spdk_blob_set_xattr(blob, "length", &length, sizeof(length));
	CU_ASSERT(rc == 0);

	/* get_xattr should still work even if md_ro flag is set. */
	value = NULL;
	__blob_to_data(blob)->md_ro = true;
	rc = spdk_blob_get_xattr_value(blob, "length", &value, &value_len);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(*(uint64_t *)value == length);
	CU_ASSERT(value_len == 8);
	__blob_to_data(blob)->md_ro = false;

	rc = spdk_blob_get_xattr_value(blob, "foobar", &value, &value_len);
	CU_ASSERT(rc == -ENOENT);

	names = NULL;
	rc = spdk_blob_get_xattr_names(blob, &names);
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

	/* Confirm that remove_xattr fails if md_ro is set to true. */
	__blob_to_data(blob)->md_ro = true;
	rc = spdk_blob_remove_xattr(blob, "name");
	CU_ASSERT(rc == -EPERM);

	__blob_to_data(blob)->md_ro = false;
	rc = spdk_blob_remove_xattr(blob, "name");
	CU_ASSERT(rc == 0);

	rc = spdk_blob_remove_xattr(blob, "foobar");
	CU_ASSERT(rc == -ENOENT);

	/* Set internal xattr */
	length = 7898;
	rc = _spdk_blob_set_xattr(__blob_to_data(blob), "internal", &length, sizeof(length), true);
	CU_ASSERT(rc == 0);
	rc = _spdk_blob_get_xattr_value(__blob_to_data(blob), "internal", &value, &value_len, true);
	CU_ASSERT(rc == 0);
	CU_ASSERT(*(uint64_t *)value == length);
	/* try to get public xattr with same name */
	rc = spdk_blob_get_xattr_value(blob, "internal", &value, &value_len);
	CU_ASSERT(rc != 0);
	rc = _spdk_blob_get_xattr_value(__blob_to_data(blob), "internal", &value, &value_len, false);
	CU_ASSERT(rc != 0);
	/* Check if SPDK_BLOB_INTERNAL_XATTR is set */
	CU_ASSERT((__blob_to_data(blob)->invalid_flags & SPDK_BLOB_INTERNAL_XATTR) ==
		  SPDK_BLOB_INTERNAL_XATTR)

	spdk_blob_close(blob, blob_op_complete, NULL);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);

	/* Check if xattrs are persisted */
	dev = init_dev();

	spdk_bs_load(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	bs = g_bs;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;

	rc = _spdk_blob_get_xattr_value(__blob_to_data(blob), "internal", &value, &value_len, true);
	CU_ASSERT(rc == 0);
	CU_ASSERT(*(uint64_t *)value == length);

	/* try to get internal xattr trough public call */
	rc = spdk_blob_get_xattr_value(blob, "internal", &value, &value_len);
	CU_ASSERT(rc != 0);

	rc = _spdk_blob_remove_xattr(__blob_to_data(blob), "internal", true);
	CU_ASSERT(rc == 0);

	CU_ASSERT((__blob_to_data(blob)->invalid_flags & SPDK_BLOB_INTERNAL_XATTR) == 0);

	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
bs_load(void)
{
	struct spdk_bs_dev *dev;
	spdk_blob_id blobid;
	struct spdk_blob *blob;
	struct spdk_bs_super_block *super_block;
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
	spdk_bs_open_blob(g_bs, 0, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == -ENOENT);
	CU_ASSERT(g_blob == NULL);

	/* Create a blob */
	spdk_bs_create_blob(g_bs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_open_blob(g_bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;

	/* Try again to open valid blob but without the upper bit set */
	spdk_bs_open_blob(g_bs, blobid & 0xFFFFFFFF, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == -ENOENT);
	CU_ASSERT(g_blob == NULL);

	/* Set some xattrs */
	rc = spdk_blob_set_xattr(blob, "name", "log.txt", strlen("log.txt") + 1);
	CU_ASSERT(rc == 0);

	length = 2345;
	rc = spdk_blob_set_xattr(blob, "length", &length, sizeof(length));
	CU_ASSERT(rc == 0);

	/* Resize the blob */
	rc = spdk_blob_resize(blob, 10);
	CU_ASSERT(rc == 0);

	spdk_blob_close(blob, blob_op_complete, NULL);
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

	super_block = (struct spdk_bs_super_block *)g_dev_buffer;
	CU_ASSERT(super_block->clean == 1);


	/* Load an existing blob store */
	dev = init_dev();
	strncpy(opts.bstype.bstype, "TESTTYPE", SPDK_BLOBSTORE_TYPE_LENGTH);
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	super_block = (struct spdk_bs_super_block *)g_dev_buffer;
	CU_ASSERT(super_block->clean == 0);

	spdk_bs_open_blob(g_bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;

	/* Get the xattrs */
	value = NULL;
	rc = spdk_blob_get_xattr_value(blob, "length", &value, &value_len);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(*(uint64_t *)value == length);
	CU_ASSERT(value_len == 8);

	rc = spdk_blob_get_xattr_value(blob, "foobar", &value, &value_len);
	CU_ASSERT(rc == -ENOENT);

	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 10);

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	blob = NULL;
	g_blob = NULL;
	g_blobid = SPDK_BLOBID_INVALID;

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
	g_scheduler_delay = false;
}

static void
bs_type(void)
{
	struct spdk_bs_dev *dev;
	struct spdk_bs_opts opts;

	g_scheduler_delay = true;

	dev = init_dev();
	spdk_bs_opts_init(&opts);
	strncpy(opts.bstype.bstype, "TESTTYPE", SPDK_BLOBSTORE_TYPE_LENGTH);

	/* Initialize a new blob store */
	spdk_bs_init(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	/* Unload the blob store */
	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
	g_blob = NULL;
	g_blobid = 0;

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
	g_scheduler_delay = false;
}

static void
bs_super_block(void)
{
	struct spdk_bs_dev *dev;
	struct spdk_bs_super_block *super_block;
	struct spdk_bs_opts opts;
	struct spdk_bs_super_block_ver1 super_block_v1;

	g_scheduler_delay = true;

	dev = init_dev();
	spdk_bs_opts_init(&opts);
	strncpy(opts.bstype.bstype, "TESTTYPE", SPDK_BLOBSTORE_TYPE_LENGTH);

	/* Initialize a new blob store */
	spdk_bs_init(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	/* Unload the blob store */
	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
	g_blob = NULL;
	g_blobid = 0;

	/* Load an existing blob store with version newer than supported */
	super_block = (struct spdk_bs_super_block *)g_dev_buffer;
	super_block->version++;

	dev = init_dev();
	strncpy(opts.bstype.bstype, "", SPDK_BLOBSTORE_TYPE_LENGTH);
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno != 0);

	/* Create a new blob store with super block version 1 */
	dev = init_dev();
	super_block_v1.version = 1;
	strncpy(super_block_v1.signature, "SPDKBLOB", sizeof(super_block_v1.signature));
	super_block_v1.length = 0x1000;
	super_block_v1.clean = 1;
	super_block_v1.super_blob = 0xFFFFFFFFFFFFFFFF;
	super_block_v1.cluster_size = 0x100000;
	super_block_v1.used_page_mask_start = 0x01;
	super_block_v1.used_page_mask_len = 0x01;
	super_block_v1.used_cluster_mask_start = 0x02;
	super_block_v1.used_cluster_mask_len = 0x01;
	super_block_v1.md_start = 0x03;
	super_block_v1.md_len = 0x40;
	memset(super_block_v1.reserved, 0, 4036);
	super_block_v1.crc = _spdk_blob_md_page_calc_crc(&super_block_v1);
	memcpy(g_dev_buffer, &super_block_v1, sizeof(struct spdk_bs_super_block_ver1));

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
	struct spdk_blob_store *bs;
	spdk_blob_id blobid;
	struct spdk_blob *blob;

	dev = init_dev();

	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	/* Create a blob and open it. */
	g_bserrno = -1;
	spdk_bs_create_blob(bs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid > 0);
	blobid = g_blobid;

	g_bserrno = -1;
	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;

	/* Try to unload blobstore, should fail with open blob */
	g_bserrno = -1;
	spdk_bs_unload(bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == -EBUSY);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	/* Close the blob, then successfully unload blobstore */
	g_bserrno = -1;
	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

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

	/*
	 * Set cluster size to lower than page size,
	 * to work it is required to be at least twice the blobstore page size.
	 */
	dev = init_dev();
	spdk_bs_opts_init(&opts);
	opts.cluster_sz = SPDK_BS_PAGE_SIZE - 1;

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
 * Create a blobstore, reload it and ensure total usable cluster count
 *  stays the same.
 */
static void
bs_usable_clusters(void)
{
	struct spdk_bs_dev *dev;
	struct spdk_bs_opts opts;
	uint32_t clusters;
	int i, rc;

	/* Init blobstore */
	dev = init_dev();
	spdk_bs_opts_init(&opts);

	spdk_bs_init(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	clusters = spdk_bs_total_data_cluster_count(g_bs);

	/* Unload the blob store */
	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;

	dev = init_dev();
	/* Load an existing blob store */
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	CU_ASSERT(spdk_bs_total_data_cluster_count(g_bs) == clusters);

	/* Create and resize blobs to make sure that useable cluster count won't change */
	for (i = 0; i < 4; i++) {
		g_bserrno = -1;
		g_blobid = SPDK_BLOBID_INVALID;
		spdk_bs_create_blob(g_bs, blob_op_with_id_complete, NULL);
		CU_ASSERT(g_bserrno == 0);
		CU_ASSERT(g_blobid !=  SPDK_BLOBID_INVALID);

		g_bserrno = -1;
		g_blob = NULL;
		spdk_bs_open_blob(g_bs, g_blobid, blob_op_with_handle_complete, NULL);
		CU_ASSERT(g_bserrno == 0);
		CU_ASSERT(g_blob !=  NULL);

		rc = spdk_blob_resize(g_blob, 10);
		CU_ASSERT(rc == 0);

		g_bserrno = -1;
		spdk_blob_close(g_blob, blob_op_complete, NULL);
		CU_ASSERT(g_bserrno == 0);

		CU_ASSERT(spdk_bs_total_data_cluster_count(g_bs) == clusters);
	}

	/* Reload the blob store to make sure that nothing changed */
	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;

	dev = init_dev();
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	CU_ASSERT(spdk_bs_total_data_cluster_count(g_bs) == clusters);

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
		spdk_bs_create_blob(g_bs, blob_op_with_id_complete, NULL);
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
		spdk_bs_open_blob(g_bs, blobids[i], blob_op_with_handle_complete, NULL);
		CU_ASSERT(g_bserrno == 0);
		CU_ASSERT(g_blob !=  NULL);
		g_bserrno = -1;
		spdk_blob_close(g_blob, blob_op_complete, NULL);
		CU_ASSERT(g_bserrno == 0);
	}

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
bs_destroy(void)
{
	struct spdk_bs_dev *dev;
	struct spdk_bs_opts opts;

	g_scheduler_delay = true;

	_bs_flush_scheduler();
	CU_ASSERT(TAILQ_EMPTY(&g_scheduled_ops));

	/* Initialize a new blob store */
	dev = init_dev();
	spdk_bs_opts_init(&opts);
	spdk_bs_init(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	/* Destroy the blob store */
	g_bserrno = -1;
	spdk_bs_destroy(g_bs, bs_op_complete, NULL);
	/* Callback is called after device is destroyed in next scheduler run. */
	_bs_flush_scheduler();
	CU_ASSERT(TAILQ_EMPTY(&g_scheduled_ops));
	CU_ASSERT(g_bserrno == 0);

	/* Loading an non-existent blob store should fail. */
	g_bserrno = -1;
	g_bs = NULL;
	dev = init_dev();

	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno != 0);
	g_scheduler_delay = false;
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
		spdk_bs_create_blob(bs, blob_op_with_id_complete, NULL);
		CU_ASSERT(g_bserrno == 0);
		CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
		blobid[i] = g_blobid;

		/* Open a blob */
		spdk_bs_open_blob(bs, blobid[i], blob_op_with_handle_complete, NULL);
		CU_ASSERT(g_bserrno == 0);
		CU_ASSERT(g_blob != NULL);
		blob[i] = g_blob;

		/* Set a fairly large xattr on both blobs to eat up
		 * metadata space
		 */
		value = calloc(dev->blocklen - 64, sizeof(char));
		SPDK_CU_ASSERT_FATAL(value != NULL);
		memset(value, i, dev->blocklen / 2);
		rc = spdk_blob_set_xattr(blob[i], "name", value, dev->blocklen - 64);
		CU_ASSERT(rc == 0);
		free(value);
	}

	/* Resize the blobs, alternating 1 cluster at a time.
	 * This thwarts run length encoding and will cause spill
	 * over of the extents.
	 */
	for (i = 0; i < 6; i++) {
		rc = spdk_blob_resize(blob[i % 2], (i / 2) + 1);
		CU_ASSERT(rc == 0);
	}

	for (i = 0; i < 2; i++) {
		spdk_blob_sync_md(blob[i], blob_op_complete, NULL);
		CU_ASSERT(g_bserrno == 0);
	}

	/* Close the blobs */
	for (i = 0; i < 2; i++) {
		spdk_blob_close(blob[i], blob_op_complete, NULL);
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

		spdk_bs_open_blob(bs, blobid[i], blob_op_with_handle_complete, NULL);
		CU_ASSERT(g_bserrno == 0);
		CU_ASSERT(g_blob != NULL);
		blob[i] = g_blob;

		CU_ASSERT(spdk_blob_get_num_clusters(blob[i]) == 3);

		spdk_blob_close(blob[i], blob_op_complete, NULL);
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

	spdk_bs_create_blob(bs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	page_num = _spdk_bs_blobid_to_page(blobid);
	index = DEV_BUFFER_BLOCKLEN * (bs->md_start + page_num);
	page = (struct spdk_blob_md_page *)&g_dev_buffer[index];
	page->crc = 0;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == -EINVAL);
	CU_ASSERT(g_blob == NULL);
	g_bserrno = 0;

	spdk_bs_delete_blob(bs, blobid, blob_op_complete, NULL);
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

/* For blob dirty shutdown test case we do the following sub-test cases:
 * 1 Initialize new blob store and create 1 blob with some xattrs, then we
 *   dirty shutdown and reload the blob store and verify the xattrs.
 * 2 Resize the blob from 10 clusters to 20 clusters and then dirty shutdown,
 *   reload the blob store and verify the clusters number.
 * 3 Create the second blob and then dirty shutdown, reload the blob store
 *   and verify the second blob.
 * 4 Delete the second blob and then dirty shutdown, reload teh blob store
 *   and verify the second blob is invalid.
 * 5 Create the second blob again and also create the third blob, modify the
 *   md of second blob which makes the md invalid, and then dirty shutdown,
 *   reload the blob store verify the second blob, it should invalid and also
 *   verify the third blob, it should correct.
 */
static void
blob_dirty_shutdown(void)
{
	int rc;
	int index;
	struct spdk_bs_dev *dev;
	spdk_blob_id blobid1, blobid2, blobid3;
	struct spdk_blob *blob;
	uint64_t length;
	uint64_t free_clusters;
	const void *value;
	size_t value_len;
	uint32_t page_num;
	struct spdk_blob_md_page *page;
	struct spdk_bs_opts opts;

	dev = init_dev();
	spdk_bs_opts_init(&opts);
	/* Initialize a new blob store */
	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	/* Create first blob */
	spdk_bs_create_blob(g_bs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid1 = g_blobid;

	spdk_bs_open_blob(g_bs, blobid1, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;

	/* Set some xattrs */
	rc = spdk_blob_set_xattr(blob, "name", "log.txt", strlen("log.txt") + 1);
	CU_ASSERT(rc == 0);

	length = 2345;
	rc = spdk_blob_set_xattr(blob, "length", &length, sizeof(length));
	CU_ASSERT(rc == 0);

	/* Resize the blob */
	rc = spdk_blob_resize(blob, 10);
	CU_ASSERT(rc == 0);

	free_clusters = spdk_bs_free_cluster_count(g_bs);

	spdk_blob_close(blob, blob_op_complete, NULL);
	blob = NULL;
	g_blob = NULL;
	g_blobid = SPDK_BLOBID_INVALID;

	/* Dirty shutdown */
	_spdk_bs_free(g_bs);

	/* reload blobstore */
	dev = init_dev();
	spdk_bs_opts_init(&opts);
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_open_blob(g_bs, blobid1, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;

	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(g_bs));

	/* Get the xattrs */
	value = NULL;
	rc = spdk_blob_get_xattr_value(blob, "length", &value, &value_len);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(*(uint64_t *)value == length);
	CU_ASSERT(value_len == 8);
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 10);

	/* Resize the blob */
	rc = spdk_blob_resize(blob, 20);
	CU_ASSERT(rc == 0);

	free_clusters = spdk_bs_free_cluster_count(g_bs);

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	blob = NULL;
	g_blob = NULL;
	g_blobid = SPDK_BLOBID_INVALID;

	/* Dirty shutdown */
	_spdk_bs_free(g_bs);

	/* reload the blobstore */
	dev = init_dev();
	spdk_bs_opts_init(&opts);
	/* Load an existing blob store */
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	spdk_bs_open_blob(g_bs, blobid1, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 20);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(g_bs));

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	blob = NULL;
	g_blob = NULL;
	g_blobid = SPDK_BLOBID_INVALID;

	/* Create second blob */
	spdk_bs_create_blob(g_bs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid2 = g_blobid;

	spdk_bs_open_blob(g_bs, blobid2, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;

	/* Set some xattrs */
	rc = spdk_blob_set_xattr(blob, "name", "log1.txt", strlen("log1.txt") + 1);
	CU_ASSERT(rc == 0);

	length = 5432;
	rc = spdk_blob_set_xattr(blob, "length", &length, sizeof(length));
	CU_ASSERT(rc == 0);

	/* Resize the blob */
	rc = spdk_blob_resize(blob, 10);
	CU_ASSERT(rc == 0);

	free_clusters = spdk_bs_free_cluster_count(g_bs);

	spdk_blob_close(blob, blob_op_complete, NULL);
	blob = NULL;
	g_blob = NULL;
	g_blobid = SPDK_BLOBID_INVALID;

	/* Dirty shutdown */
	_spdk_bs_free(g_bs);

	/* reload the blobstore */
	dev = init_dev();
	spdk_bs_opts_init(&opts);
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_open_blob(g_bs, blobid2, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;

	/* Get the xattrs */
	value = NULL;
	rc = spdk_blob_get_xattr_value(blob, "length", &value, &value_len);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(*(uint64_t *)value == length);
	CU_ASSERT(value_len == 8);
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 10);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(g_bs));

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	spdk_bs_delete_blob(g_bs, blobid2, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	free_clusters = spdk_bs_free_cluster_count(g_bs);

	/* Dirty shutdown */
	_spdk_bs_free(g_bs);
	/* reload the blobstore */
	dev = init_dev();
	spdk_bs_opts_init(&opts);
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_open_blob(g_bs, blobid2, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno != 0);
	CU_ASSERT(g_blob == NULL);

	spdk_bs_open_blob(g_bs, blobid1, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(g_bs));
	spdk_blob_close(g_blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;

	/* reload the blobstore */
	dev = init_dev();
	spdk_bs_opts_init(&opts);
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* Create second blob */
	spdk_bs_create_blob(g_bs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid2 = g_blobid;

	/* Create third blob */
	spdk_bs_create_blob(g_bs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid3 = g_blobid;

	spdk_bs_open_blob(g_bs, blobid2, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;

	/* Set some xattrs for second blob */
	rc = spdk_blob_set_xattr(blob, "name", "log1.txt", strlen("log1.txt") + 1);
	CU_ASSERT(rc == 0);

	length = 5432;
	rc = spdk_blob_set_xattr(blob, "length", &length, sizeof(length));
	CU_ASSERT(rc == 0);

	spdk_blob_close(blob, blob_op_complete, NULL);
	blob = NULL;
	g_blob = NULL;
	g_blobid = SPDK_BLOBID_INVALID;

	spdk_bs_open_blob(g_bs, blobid3, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;

	/* Set some xattrs for third blob */
	rc = spdk_blob_set_xattr(blob, "name", "log2.txt", strlen("log2.txt") + 1);
	CU_ASSERT(rc == 0);

	length = 5432;
	rc = spdk_blob_set_xattr(blob, "length", &length, sizeof(length));
	CU_ASSERT(rc == 0);

	spdk_blob_close(blob, blob_op_complete, NULL);
	blob = NULL;
	g_blob = NULL;
	g_blobid = SPDK_BLOBID_INVALID;

	/* Mark second blob as invalid */
	page_num = _spdk_bs_blobid_to_page(blobid2);

	index = DEV_BUFFER_BLOCKLEN * (g_bs->md_start + page_num);
	page = (struct spdk_blob_md_page *)&g_dev_buffer[index];
	page->sequence_num = 1;
	page->crc = _spdk_blob_md_page_calc_crc(page);

	free_clusters = spdk_bs_free_cluster_count(g_bs);

	/* Dirty shutdown */
	_spdk_bs_free(g_bs);
	/* reload the blobstore */
	dev = init_dev();
	spdk_bs_opts_init(&opts);
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_open_blob(g_bs, blobid2, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno != 0);
	CU_ASSERT(g_blob == NULL);

	spdk_bs_open_blob(g_bs, blobid3, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;

	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(g_bs));

	spdk_blob_close(blob, blob_op_complete, NULL);
	blob = NULL;
	g_blob = NULL;
	g_blobid = SPDK_BLOBID_INVALID;

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
blob_flags(void)
{
	struct spdk_bs_dev *dev;
	spdk_blob_id blobid_invalid, blobid_data_ro, blobid_md_ro;
	struct spdk_blob *blob_invalid, *blob_data_ro, *blob_md_ro;
	struct spdk_bs_opts opts;
	int rc;

	dev = init_dev();
	spdk_bs_opts_init(&opts);

	/* Initialize a new blob store */
	spdk_bs_init(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	/* Create three blobs - one each for testing invalid, data_ro and md_ro flags. */
	spdk_bs_create_blob(g_bs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid_invalid = g_blobid;

	spdk_bs_create_blob(g_bs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid_data_ro = g_blobid;

	spdk_bs_create_blob(g_bs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid_md_ro = g_blobid;

	spdk_bs_open_blob(g_bs, blobid_invalid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob_invalid = g_blob;

	spdk_bs_open_blob(g_bs, blobid_data_ro, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob_data_ro = g_blob;

	spdk_bs_open_blob(g_bs, blobid_md_ro, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob_md_ro = g_blob;

	/* Change the size of blob_data_ro to check if flags are serialized
	 * when blob has non zero number of extents */
	rc = spdk_blob_resize(blob_data_ro, 10);
	CU_ASSERT(rc == 0);

	/* Set the xattr to check if flags are serialized
	 * when blob has non zero number of xattrs */
	rc = spdk_blob_set_xattr(blob_md_ro, "name", "log.txt", strlen("log.txt") + 1);
	CU_ASSERT(rc == 0);

	__blob_to_data(blob_invalid)->invalid_flags = (1ULL << 63);
	__blob_to_data(blob_invalid)->state = SPDK_BLOB_STATE_DIRTY;
	__blob_to_data(blob_data_ro)->data_ro_flags = (1ULL << 62);
	__blob_to_data(blob_data_ro)->state = SPDK_BLOB_STATE_DIRTY;
	__blob_to_data(blob_md_ro)->md_ro_flags = (1ULL << 61);
	__blob_to_data(blob_md_ro)->state = SPDK_BLOB_STATE_DIRTY;

	g_bserrno = -1;
	spdk_blob_sync_md(blob_invalid, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bserrno = -1;
	spdk_blob_sync_md(blob_data_ro, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bserrno = -1;
	spdk_blob_sync_md(blob_md_ro, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	g_bserrno = -1;
	spdk_blob_close(blob_invalid, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	blob_invalid = NULL;
	g_bserrno = -1;
	spdk_blob_close(blob_data_ro, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	blob_data_ro = NULL;
	g_bserrno = -1;
	spdk_blob_close(blob_md_ro, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	blob_md_ro = NULL;

	g_blob = NULL;
	g_blobid = SPDK_BLOBID_INVALID;

	/* Unload the blob store */
	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;

	/* Load an existing blob store */
	dev = init_dev();
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	g_blob = NULL;
	g_bserrno = 0;
	spdk_bs_open_blob(g_bs, blobid_invalid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno != 0);
	CU_ASSERT(g_blob == NULL);

	g_blob = NULL;
	g_bserrno = -1;
	spdk_bs_open_blob(g_bs, blobid_data_ro, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob_data_ro = g_blob;
	/* If an unknown data_ro flag was found, the blob should be marked both data and md read-only. */
	CU_ASSERT(__blob_to_data(blob_data_ro)->data_ro == true);
	CU_ASSERT(__blob_to_data(blob_data_ro)->md_ro == true);
	CU_ASSERT(spdk_blob_get_num_clusters(blob_data_ro) == 10);

	g_blob = NULL;
	g_bserrno = -1;
	spdk_bs_open_blob(g_bs, blobid_md_ro, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob_md_ro = g_blob;
	CU_ASSERT(__blob_to_data(blob_md_ro)->data_ro == false);
	CU_ASSERT(__blob_to_data(blob_md_ro)->md_ro == true);

	g_bserrno = -1;
	spdk_blob_sync_md(blob_md_ro, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_blob_close(blob_data_ro, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	spdk_blob_close(blob_md_ro, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
}

static void
bs_version(void)
{
	struct spdk_bs_super_block *super;
	struct spdk_bs_dev *dev;
	struct spdk_bs_opts opts;
	spdk_blob_id blobid;

	dev = init_dev();
	spdk_bs_opts_init(&opts);

	/* Initialize a new blob store */
	spdk_bs_init(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	/* Unload the blob store */
	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;

	/*
	 * Change the bs version on disk.  This will allow us to
	 *  test that the version does not get modified automatically
	 *  when loading and unloading the blobstore.
	 */
	super = (struct spdk_bs_super_block *)&g_dev_buffer[0];
	CU_ASSERT(super->version == SPDK_BS_VERSION);
	CU_ASSERT(super->clean == 1);
	super->version = 2;
	/*
	 * Version 2 metadata does not have a used blobid mask, so clear
	 *  those fields in the super block and zero the corresponding
	 *  region on "disk".  We will use this to ensure blob IDs are
	 *  correctly reconstructed.
	 */
	memset(&g_dev_buffer[super->used_blobid_mask_start * SPDK_BS_PAGE_SIZE], 0,
	       super->used_blobid_mask_len * SPDK_BS_PAGE_SIZE);
	super->used_blobid_mask_start = 0;
	super->used_blobid_mask_len = 0;
	super->crc = _spdk_blob_md_page_calc_crc(super);

	/* Load an existing blob store */
	dev = init_dev();
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	CU_ASSERT(super->clean == 0);

	/*
	 * Create a blob - just to make sure that when we unload it
	 *  results in writing the super block (since metadata pages
	 *  were allocated.
	 */
	spdk_bs_create_blob(g_bs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	/* Unload the blob store */
	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
	CU_ASSERT(super->version == 2);
	CU_ASSERT(super->used_blobid_mask_start == 0);
	CU_ASSERT(super->used_blobid_mask_len == 0);

	dev = init_dev();
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	g_blob = NULL;
	spdk_bs_open_blob(g_bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);

	spdk_blob_close(g_blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
	CU_ASSERT(super->version == 2);
	CU_ASSERT(super->used_blobid_mask_start == 0);
	CU_ASSERT(super->used_blobid_mask_len == 0);
}

static void
_get_xattr_value(void *arg, const char *name,
		 const void **value, size_t *value_len)
{
	uint64_t i;

	SPDK_CU_ASSERT_FATAL(value_len != NULL);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(arg == &g_ctx)

	for (i = 0; i < sizeof(g_xattr_names); i++) {
		if (!strcmp(name, g_xattr_names[i])) {
			*value_len = strlen(g_xattr_values[i]);
			*value = g_xattr_values[i];
			break;
		}
	}
}

static void
_get_xattr_value_null(void *arg, const char *name,
		      const void **value, size_t *value_len)
{
	SPDK_CU_ASSERT_FATAL(value_len != NULL);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(arg == NULL)

	*value_len = 0;
	*value = NULL;
}

static void
blob_set_xattrs(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_blob *blob;
	struct spdk_blob_opts opts;
	spdk_blob_id blobid;
	const void *value;
	size_t value_len;
	int rc;

	dev = init_dev();

	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	/* Create blob with extra attributes */
	spdk_blob_opts_init(&opts);

	opts.xattrs.names = g_xattr_names;
	opts.xattrs.get_value = _get_xattr_value;
	opts.xattrs.count = 3;
	opts.xattrs.ctx = &g_ctx;

	spdk_bs_create_blob_ext(bs, &opts, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;

	/* Get the xattrs */
	value = NULL;

	rc = spdk_blob_get_xattr_value(blob, g_xattr_names[0], &value, &value_len);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(value_len == strlen(g_xattr_values[0]));
	CU_ASSERT_NSTRING_EQUAL_FATAL(value, g_xattr_values[0], value_len);

	rc = spdk_blob_get_xattr_value(blob, g_xattr_names[1], &value, &value_len);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(value_len == strlen(g_xattr_values[1]));
	CU_ASSERT_NSTRING_EQUAL((char *)value, g_xattr_values[1], value_len);

	rc = spdk_blob_get_xattr_value(blob, g_xattr_names[2], &value, &value_len);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(value_len == strlen(g_xattr_values[2]));
	CU_ASSERT_NSTRING_EQUAL((char *)value, g_xattr_values[2], value_len);

	/* Try to get non existing attribute */

	rc = spdk_blob_get_xattr_value(blob, "foobar", &value, &value_len);
	CU_ASSERT(rc == -ENOENT);

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	blob = NULL;
	g_blob = NULL;
	g_blobid = SPDK_BLOBID_INVALID;

	/* NULL callback */
	spdk_blob_opts_init(&opts);
	opts.xattrs.names = g_xattr_names;
	opts.xattrs.get_value = NULL;
	opts.xattrs.count = 1;
	opts.xattrs.ctx = &g_ctx;

	spdk_bs_create_blob_ext(bs, &opts, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == -EINVAL);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);

	/* NULL values */
	spdk_blob_opts_init(&opts);
	opts.xattrs.names = g_xattr_names;
	opts.xattrs.get_value = _get_xattr_value_null;
	opts.xattrs.count = 1;
	opts.xattrs.ctx = NULL;

	spdk_bs_create_blob_ext(bs, &opts, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == -EINVAL);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;

}

static void
blob_thin_prov_alloc(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_blob *blob;
	struct spdk_blob_data *blob_data;
	struct spdk_blob_opts opts;
	spdk_blob_id blobid;
	uint64_t free_clusters;
	int rc;

	dev = init_dev();

	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;
	free_clusters = spdk_bs_free_cluster_count(bs);

	/* Set blob as thin provisioned */
	spdk_blob_opts_init(&opts);
	opts.thin_provision = true;

	spdk_bs_create_blob_ext(bs, &opts, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;
	blob_data = __blob_to_data(blob);

	CU_ASSERT(blob_data->active.num_clusters == 0);
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 0);

	/* The blob started at 0 clusters. Resize it to be 5, but still unallocated. */
	rc = spdk_blob_resize(blob, 5);
	CU_ASSERT(rc == 0);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(blob_data->active.num_clusters == 5);
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 5);

	/* Shrink the blob to 3 clusters - still unallocated */
	rc = spdk_blob_resize(blob, 3);
	CU_ASSERT(rc == 0);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(blob_data->active.num_clusters == 3);
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 3);

	spdk_blob_sync_md(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	/* Sync must not change anything */
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(blob_data->active.num_clusters == 3);
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 3);

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* Unload the blob store */
	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
	g_blob = NULL;
	g_blobid = 0;

	/* Load an existing blob store */
	dev = init_dev();
	spdk_bs_load(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	bs = g_bs;

	spdk_bs_open_blob(g_bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;
	blob_data = __blob_to_data(blob);

	/* Check that clusters allocation and size is still the same */
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(blob_data->active.num_clusters == 3);

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_delete_blob(bs, blobid, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
blob_insert_cluster_msg(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_blob *blob;
	struct spdk_blob_data *blob_data;
	struct spdk_blob_opts opts;
	spdk_blob_id blobid;
	uint64_t free_clusters;

	dev = init_dev();

	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;
	free_clusters = spdk_bs_free_cluster_count(bs);

	/* Set blob as thin provisioned */
	spdk_blob_opts_init(&opts);
	opts.thin_provision = true;
	opts.num_clusters = 4;

	spdk_bs_create_blob_ext(bs, &opts, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;
	blob_data = __blob_to_data(blob);

	CU_ASSERT(blob_data->active.num_clusters == 4);
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 4);
	CU_ASSERT(blob_data->active.clusters[1] == 0);

	_spdk_bs_claim_cluster(bs, 0xF);
	_spdk_blob_insert_cluster_on_md_thread(blob_data, 1, 0xF, blob_op_complete, NULL);

	CU_ASSERT(blob_data->active.clusters[1] != 0);

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* Unload the blob store */
	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
	g_blob = NULL;
	g_blobid = 0;

	/* Load an existing blob store */
	dev = init_dev();
	spdk_bs_load(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	bs = g_bs;

	spdk_bs_open_blob(g_bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;
	blob_data = __blob_to_data(blob);

	CU_ASSERT(blob_data->active.clusters[1] != 0);

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_delete_blob(bs, blobid, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
blob_thin_prov_rw(void)
{
	static const uint8_t zero[10 * 4096] = { 0 };
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_blob *blob;
	struct spdk_blob_data *blob_data;
	struct spdk_io_channel *channel;
	struct spdk_blob_opts 	opts;
	spdk_blob_id blobid;
	uint64_t free_clusters;
	uint8_t payload_read[10 * 4096];
	uint8_t payload_write[10 * 4096];
	int rc;

	dev = init_dev();

	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;
	free_clusters = spdk_bs_free_cluster_count(bs);

	channel = spdk_bs_alloc_io_channel(bs);
	CU_ASSERT(channel != NULL);

	spdk_blob_opts_init(&opts);
	opts.thin_provision = true;

	spdk_bs_create_blob_ext(bs, &opts, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;
	blob_data = __blob_to_data(blob);

	CU_ASSERT(blob_data->active.num_clusters == 0);

	/* The blob started at 0 clusters. Resize it to be 5, but still unallocated. */
	rc = spdk_blob_resize(blob, 5);
	CU_ASSERT(rc == 0);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(blob_data->active.num_clusters == 5);

	spdk_blob_sync_md(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	/* Sync must not change anything */
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(blob_data->active.num_clusters == 5);

	/* Payload should be all zeros from unallocated clusters */
	memset(payload_read, 0xFF, sizeof(payload_read));
	spdk_bs_io_read_blob(blob, channel, payload_read, 4, 10, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(zero, payload_read, 10 * 4096) == 0);

	memset(payload_write, 0xE5, sizeof(payload_write));
	spdk_bs_io_write_blob(blob, channel, payload_write, 4, 10, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(free_clusters != spdk_bs_free_cluster_count(bs));

	spdk_bs_io_read_blob(blob, channel, payload_read, 4, 10, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_write, payload_read, 10 * 4096) == 0);

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_delete_blob(bs, blobid, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));

	spdk_bs_free_io_channel(channel);

	/* Unload the blob store */
	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
	g_blob = NULL;
	g_blobid = 0;
}

static void
blob_thin_prov_rw_iov(void)
{
	static const uint8_t zero[10 * 4096] = { 0 };
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_blob *blob;
	struct spdk_blob_data *blob_data;
	struct spdk_io_channel *channel;
	struct spdk_blob_opts 	opts;
	spdk_blob_id blobid;
	uint64_t free_clusters;
	uint8_t payload_read[10 * 4096];
	uint8_t payload_write[10 * 4096];
	struct iovec iov_read[3];
	struct iovec iov_write[3];

	int rc;

	dev = init_dev();

	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;
	free_clusters = spdk_bs_free_cluster_count(bs);

	channel = spdk_bs_alloc_io_channel(bs);
	CU_ASSERT(channel != NULL);

	spdk_blob_opts_init(&opts);
	opts.thin_provision = true;

	spdk_bs_create_blob_ext(bs, &opts, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;
	blob_data = __blob_to_data(blob);

	CU_ASSERT(blob_data->active.num_clusters == 0);

	/* The blob started at 0 clusters. Resize it to be 5, but still unallocated. */
	rc = spdk_blob_resize(blob, 5);
	CU_ASSERT(rc == 0);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(blob_data->active.num_clusters == 5);

	spdk_blob_sync_md(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	/* Sync must not change anything */
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(blob_data->active.num_clusters == 5);

	/* Payload should be all zeros from unallocated clusters */
	memset(payload_read, 0xAA, sizeof(payload_read));
	iov_read[0].iov_base = payload_read;
	iov_read[0].iov_len = 3 * 4096;
	iov_read[1].iov_base = payload_read + 3 * 4096;
	iov_read[1].iov_len = 4 * 4096;
	iov_read[2].iov_base = payload_read + 7 * 4096;
	iov_read[2].iov_len = 3 * 4096;
	spdk_bs_io_readv_blob(blob, channel, iov_read, 3, 250, 10, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(zero, payload_read, 10 * 4096) == 0);

	memset(payload_write, 0xE5, sizeof(payload_write));
	iov_write[0].iov_base = payload_write;
	iov_write[0].iov_len = 1 * 4096;
	iov_write[1].iov_base = payload_write + 1 * 4096;
	iov_write[1].iov_len = 5 * 4096;
	iov_write[2].iov_base = payload_write + 6 * 4096;
	iov_write[2].iov_len = 4 * 4096;

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

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_free_io_channel(channel);

	/* Unload the blob store */
	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
	g_blob = NULL;
	g_blobid = 0;
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
		CU_add_test(suite, "blob_create", blob_create) == NULL ||
		CU_add_test(suite, "blob_thin_provision", blob_thin_provision) == NULL ||
		CU_add_test(suite, "blob_delete", blob_delete) == NULL ||
		CU_add_test(suite, "blob_resize", blob_resize) == NULL ||
		CU_add_test(suite, "blob_read_only", blob_read_only) == NULL ||
		CU_add_test(suite, "channel_ops", channel_ops) == NULL ||
		CU_add_test(suite, "blob_super", blob_super) == NULL ||
		CU_add_test(suite, "blob_write", blob_write) == NULL ||
		CU_add_test(suite, "blob_read", blob_read) == NULL ||
		CU_add_test(suite, "blob_rw_verify", blob_rw_verify) == NULL ||
		CU_add_test(suite, "blob_rw_verify_iov", blob_rw_verify_iov) == NULL ||
		CU_add_test(suite, "blob_rw_verify_iov_nomem", blob_rw_verify_iov_nomem) == NULL ||
		CU_add_test(suite, "blob_rw_iov_read_only", blob_rw_iov_read_only) == NULL ||
		CU_add_test(suite, "blob_unmap", blob_unmap) == NULL ||
		CU_add_test(suite, "blob_iter", blob_iter) == NULL ||
		CU_add_test(suite, "blob_xattr", blob_xattr) == NULL ||
		CU_add_test(suite, "bs_load", bs_load) == NULL ||
		CU_add_test(suite, "bs_unload", bs_unload) == NULL ||
		CU_add_test(suite, "bs_cluster_sz", bs_cluster_sz) == NULL ||
		CU_add_test(suite, "bs_usable_clusters", bs_usable_clusters) == NULL ||
		CU_add_test(suite, "bs_resize_md", bs_resize_md) == NULL ||
		CU_add_test(suite, "bs_destroy", bs_destroy) == NULL ||
		CU_add_test(suite, "bs_type", bs_type) == NULL ||
		CU_add_test(suite, "bs_super_block", bs_super_block) == NULL ||
		CU_add_test(suite, "blob_serialize", blob_serialize) == NULL ||
		CU_add_test(suite, "blob_crc", blob_crc) == NULL ||
		CU_add_test(suite, "super_block_crc", super_block_crc) == NULL ||
		CU_add_test(suite, "blob_dirty_shutdown", blob_dirty_shutdown) == NULL ||
		CU_add_test(suite, "blob_flags", blob_flags) == NULL ||
		CU_add_test(suite, "bs_version", bs_version) == NULL ||
		CU_add_test(suite, "blob_set_xattrs", blob_set_xattrs) == NULL ||
		CU_add_test(suite, "blob_thin_prov_alloc", blob_thin_prov_alloc) == NULL ||
		CU_add_test(suite, "blob_insert_cluster_msg", blob_insert_cluster_msg) == NULL ||
		CU_add_test(suite, "blob_thin_prov_rw", blob_thin_prov_rw) == NULL ||
		CU_add_test(suite, "blob_thin_prov_rw_iov", blob_thin_prov_rw_iov) == NULL
	) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	g_dev_buffer = calloc(1, DEV_BUFFER_SIZE);
	spdk_allocate_thread(_bs_send_msg, NULL, NULL, NULL, "thread0");
	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	spdk_free_thread();
	free(g_dev_buffer);
	return num_failures;
}
