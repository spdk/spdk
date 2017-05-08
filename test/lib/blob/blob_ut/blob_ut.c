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
	struct spdk_bs_dev dev;

	init_dev(&dev);

	spdk_bs_init(&dev, NULL, bs_op_with_handle_complete, NULL);
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
	struct spdk_bs_dev dev;
	spdk_blob_id blobid;

	init_dev(&dev);

	spdk_bs_init(&dev, NULL, bs_op_with_handle_complete, NULL);
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
	struct spdk_bs_dev dev;
	struct spdk_blob *blob;
	spdk_blob_id blobid, blobid2;

	init_dev(&dev);

	spdk_bs_init(&dev, NULL, bs_op_with_handle_complete, NULL);
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
	struct spdk_bs_dev dev;
	spdk_blob_id blobid;

	init_dev(&dev);

	spdk_bs_init(&dev, NULL, bs_op_with_handle_complete, NULL);
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
	struct spdk_bs_dev dev;
	struct spdk_blob *blob;
	spdk_blob_id blobid;
	uint64_t free_clusters;
	int rc;

	init_dev(&dev);

	spdk_bs_init(&dev, NULL, bs_op_with_handle_complete, NULL);
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
	struct spdk_bs_dev dev;
	struct spdk_io_channel *channel;

	init_dev(&dev);

	spdk_bs_init(&dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	channel = spdk_bs_alloc_io_channel(bs, SPDK_IO_PRIORITY_DEFAULT, 32);
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
	struct spdk_bs_dev dev;
	struct spdk_blob *blob;
	struct spdk_io_channel *channel;
	spdk_blob_id blobid;
	uint64_t pages_per_cluster;
	uint8_t payload[10 * 4096];
	int rc;

	init_dev(&dev);

	spdk_bs_init(&dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	pages_per_cluster = spdk_bs_get_cluster_size(bs) / spdk_bs_get_page_size(bs);

	channel = spdk_bs_alloc_io_channel(bs, SPDK_IO_PRIORITY_DEFAULT, 32);
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
	struct spdk_bs_dev dev;
	struct spdk_blob *blob;
	struct spdk_io_channel *channel;
	spdk_blob_id blobid;
	uint64_t pages_per_cluster;
	uint8_t payload[10 * 4096];
	int rc;

	init_dev(&dev);

	spdk_bs_init(&dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	pages_per_cluster = spdk_bs_get_cluster_size(bs) / spdk_bs_get_page_size(bs);

	channel = spdk_bs_alloc_io_channel(bs, SPDK_IO_PRIORITY_DEFAULT, 32);
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
	struct spdk_bs_dev dev;
	struct spdk_blob *blob;
	struct spdk_io_channel *channel;
	spdk_blob_id blobid;
	uint8_t payload_read[10 * 4096];
	uint8_t payload_write[10 * 4096];
	int rc;

	init_dev(&dev);

	spdk_bs_init(&dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	channel = spdk_bs_alloc_io_channel(bs, SPDK_IO_PRIORITY_DEFAULT, 32);
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
blob_iter(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev dev;
	struct spdk_blob *blob;
	spdk_blob_id blobid;

	init_dev(&dev);

	spdk_bs_init(&dev, NULL, bs_op_with_handle_complete, NULL);
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
	struct spdk_bs_dev dev;
	struct spdk_blob *blob;
	spdk_blob_id blobid;
	uint64_t length;
	int rc;
	const char *name1, *name2;
	const void *value;
	size_t value_len;
	struct spdk_xattr_names *names;

	init_dev(&dev);

	spdk_bs_init(&dev, NULL, bs_op_with_handle_complete, NULL);
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
	struct spdk_bs_dev dev;
	spdk_blob_id blobid;
	struct spdk_blob *blob;
	uint64_t length;
	int rc;
	const void *value;
	size_t value_len;

	init_dev(&dev);

	/* Initialize a new blob store */
	spdk_bs_init(&dev, NULL, bs_op_with_handle_complete, NULL);
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

	/* Load an existing blob store */
	spdk_bs_load(&dev, bs_op_with_handle_complete, NULL);
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
 * Create a blobstore with a cluster size different than the default, and ensure it is
 *  persisted.
 */
static void
bs_cluster_sz(void)
{
	struct spdk_bs_dev dev;
	struct spdk_bs_opts opts;
	uint32_t cluster_sz;

	init_dev(&dev);
	spdk_bs_opts_init(&opts);
	opts.cluster_sz *= 2;
	cluster_sz = opts.cluster_sz;

	/* Initialize a new blob store */
	spdk_bs_init(&dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	CU_ASSERT(spdk_bs_get_cluster_size(g_bs) == cluster_sz);

	/* Unload the blob store */
	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
	g_blob = NULL;
	g_blobid = 0;

	/* Load an existing blob store */
	spdk_bs_load(&dev, bs_op_with_handle_complete, NULL);
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
	struct spdk_bs_dev dev;
	struct spdk_bs_opts opts;
	uint32_t cluster_sz;
	spdk_blob_id blobids[NUM_BLOBS];
	int i;


	init_dev(&dev);
	spdk_bs_opts_init(&opts);
	opts.cluster_sz = CLUSTER_PAGE_COUNT * 4096;
	cluster_sz = opts.cluster_sz;

	/* Initialize a new blob store */
	spdk_bs_init(&dev, &opts, bs_op_with_handle_complete, NULL);
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
	spdk_bs_load(&dev, bs_op_with_handle_complete, NULL);
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
	struct spdk_bs_dev dev;
	struct spdk_bs_opts opts;
	struct spdk_blob_store *bs;
	spdk_blob_id blobid[2];
	struct spdk_blob *blob[2];
	uint64_t i;
	char *value;
	int rc;

	init_dev(&dev);

	/* Initialize a new blobstore with very small clusters */
	spdk_bs_opts_init(&opts);
	opts.cluster_sz = dev.blocklen * 8;
	spdk_bs_init(&dev, &opts, bs_op_with_handle_complete, NULL);
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
		value = calloc(dev.blocklen - 64, sizeof(char));
		SPDK_CU_ASSERT_FATAL(value != NULL);
		memset(value, i, dev.blocklen / 2);
		rc = spdk_blob_md_set_xattr(blob[i], "name", value, dev.blocklen - 64);
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

	/* Load an existing blob store */
	spdk_bs_load(&dev, bs_op_with_handle_complete, NULL);
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
		CU_add_test(suite, "blob_iter", blob_iter) == NULL ||
		CU_add_test(suite, "blob_xattr", blob_xattr) == NULL ||
		CU_add_test(suite, "bs_load", bs_load) == NULL ||
		CU_add_test(suite, "bs_cluster_sz", bs_cluster_sz) == NULL ||
		CU_add_test(suite, "bs_resize_md", bs_resize_md) == NULL ||
		CU_add_test(suite, "blob_serialize", blob_serialize) == NULL
	) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	g_dev_buffer = calloc(1, DEV_BUFFER_SIZE);
	spdk_allocate_thread();
	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	spdk_free_thread();
	free(g_dev_buffer);
	return num_failures;
}
