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
#include "spdk/string.h"

#include "common/lib/test_env.c"
#include "../bs_dev_common.c"
#include "blob/blobstore.c"
#include "blob/request.c"
#include "blob/zeroes.c"
#include "blob/blob_bs_dev.c"

struct spdk_blob_store *g_bs;
spdk_blob_id g_blobid;
struct spdk_blob *g_blob;
int g_bserrno;
struct spdk_xattr_names *g_names;
int g_done;
char *g_xattr_names[] = {"first", "second", "third"};
char *g_xattr_values[] = {"one", "two", "three"};
uint64_t g_ctx = 1729;

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
blob_create_internal(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_blob *blob;
	struct spdk_blob_opts opts;
	struct spdk_blob_xattr_opts internal_xattrs;
	const void *value;
	size_t value_len;
	spdk_blob_id blobid;
	int rc;

	dev = init_dev();

	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	/* Create blob with custom xattrs */

	spdk_blob_opts_init(&opts);
	_spdk_blob_xattrs_init(&internal_xattrs);
	internal_xattrs.count = 3;
	internal_xattrs.names = g_xattr_names;
	internal_xattrs.get_value = _get_xattr_value;
	internal_xattrs.ctx = &g_ctx;

	_spdk_bs_create_blob(bs, &opts, &internal_xattrs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;

	rc = _spdk_blob_get_xattr_value(blob, g_xattr_names[0], &value, &value_len, true);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(value_len == strlen(g_xattr_values[0]));
	CU_ASSERT_NSTRING_EQUAL_FATAL(value, g_xattr_values[0], value_len);

	rc = _spdk_blob_get_xattr_value(blob, g_xattr_names[1], &value, &value_len, true);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(value_len == strlen(g_xattr_values[1]));
	CU_ASSERT_NSTRING_EQUAL((char *)value, g_xattr_values[1], value_len);

	rc = _spdk_blob_get_xattr_value(blob, g_xattr_names[2], &value, &value_len, true);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(value_len == strlen(g_xattr_values[2]));
	CU_ASSERT_NSTRING_EQUAL((char *)value, g_xattr_values[2], value_len);

	rc = spdk_blob_get_xattr_value(blob, g_xattr_names[0], &value, &value_len);
	CU_ASSERT(rc != 0);

	rc = spdk_blob_get_xattr_value(blob, g_xattr_names[1], &value, &value_len);
	CU_ASSERT(rc != 0);

	rc = spdk_blob_get_xattr_value(blob, g_xattr_names[2], &value, &value_len);
	CU_ASSERT(rc != 0);

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* Create blob with NULL internal options  */

	_spdk_bs_create_blob(bs, NULL, NULL, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	CU_ASSERT(TAILQ_FIRST(&g_blob->xattrs_internal) == NULL);

	blob = g_blob;

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

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
	snprintf(bs_opts.bstype.bstype, sizeof(bs_opts.bstype.bstype), "TESTTYPE");

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
	CU_ASSERT(blob->invalid_flags & SPDK_BLOB_THIN_PROV);

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* Do not shut down cleanly.  This makes sure that when we load again
	 *  and try to recover a valid used_cluster map, that blobstore will
	 *  ignore clusters with index 0 since these are unallocated clusters.
	 */

	/* Load an existing blob store and check if invalid_flags is set */
	dev = init_dev();
	snprintf(bs_opts.bstype.bstype, sizeof(bs_opts.bstype.bstype), "TESTTYPE");
	spdk_bs_load(dev, &bs_opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	bs = g_bs;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;
	CU_ASSERT(blob->invalid_flags & SPDK_BLOB_THIN_PROV);

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
blob_snapshot(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_blob *blob;
	struct spdk_blob *snapshot, *snapshot2;
	struct spdk_blob_bs_dev *blob_bs_dev;
	struct spdk_blob_opts opts;
	struct spdk_blob_xattr_opts xattrs;
	spdk_blob_id blobid;
	spdk_blob_id snapshotid;
	const void *value;
	size_t value_len;
	int rc;

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

	/* Create snapshot from blob */
	spdk_bs_create_snapshot(bs, blobid, NULL, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	snapshotid = g_blobid;

	spdk_bs_open_blob(bs, snapshotid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	snapshot = g_blob;
	CU_ASSERT(snapshot->data_ro == true)
	CU_ASSERT(snapshot->md_ro == true)
	CU_ASSERT(spdk_blob_get_num_clusters(snapshot) == 10)

	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 10)
	CU_ASSERT(blob->invalid_flags & SPDK_BLOB_THIN_PROV);
	CU_ASSERT(spdk_mem_all_zero(blob->active.clusters,
				    blob->active.num_clusters * sizeof(blob->active.clusters[0])));

	/* Try to create snapshot from clone with xattrs */
	xattrs.names = g_xattr_names;
	xattrs.get_value = _get_xattr_value;
	xattrs.count = 3;
	xattrs.ctx = &g_ctx;
	spdk_bs_create_snapshot(bs, blobid, &xattrs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	snapshot2 = g_blob;
	CU_ASSERT(snapshot2->data_ro == true)
	CU_ASSERT(snapshot2->md_ro == true)
	CU_ASSERT(spdk_blob_get_num_clusters(snapshot2) == 10)

	/* Confirm that blob is backed by snapshot2 and snapshot2 is backed by snapshot */
	CU_ASSERT(snapshot->back_bs_dev == NULL);
	SPDK_CU_ASSERT_FATAL(blob->back_bs_dev != NULL);
	SPDK_CU_ASSERT_FATAL(snapshot2->back_bs_dev != NULL);

	blob_bs_dev = (struct spdk_blob_bs_dev *)blob->back_bs_dev;
	CU_ASSERT(blob_bs_dev->blob == snapshot2);

	blob_bs_dev = (struct spdk_blob_bs_dev *)snapshot2->back_bs_dev;
	CU_ASSERT(blob_bs_dev->blob == snapshot);

	rc = spdk_blob_get_xattr_value(snapshot2, g_xattr_names[0], &value, &value_len);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(value_len == strlen(g_xattr_values[0]));
	CU_ASSERT_NSTRING_EQUAL_FATAL(value, g_xattr_values[0], value_len);

	rc = spdk_blob_get_xattr_value(snapshot2, g_xattr_names[1], &value, &value_len);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(value_len == strlen(g_xattr_values[1]));
	CU_ASSERT_NSTRING_EQUAL((char *)value, g_xattr_values[1], value_len);

	rc = spdk_blob_get_xattr_value(snapshot2, g_xattr_names[2], &value, &value_len);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(value_len == strlen(g_xattr_values[2]));
	CU_ASSERT_NSTRING_EQUAL((char *)value, g_xattr_values[2], value_len);

	/* Try to create snapshot from snapshot */
	spdk_bs_create_snapshot(bs, snapshotid, NULL, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == -EINVAL);
	CU_ASSERT(g_blobid == SPDK_BLOBID_INVALID);

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_blob_close(snapshot, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_blob_close(snapshot2, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
blob_snapshot_freeze_io(void)
{
	struct spdk_io_channel *channel;
	struct spdk_bs_channel *bs_channel;
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_blob *blob;
	struct spdk_blob_opts opts;
	spdk_blob_id blobid;
	uint32_t num_of_pages = 10;
	uint8_t payload_read[num_of_pages * SPDK_BS_PAGE_SIZE];
	uint8_t payload_write[num_of_pages * SPDK_BS_PAGE_SIZE];
	uint8_t payload_zero[num_of_pages * SPDK_BS_PAGE_SIZE];

	memset(payload_write, 0xE5, sizeof(payload_write));
	memset(payload_read, 0x00, sizeof(payload_read));
	memset(payload_zero, 0x00, sizeof(payload_zero));

	dev = init_dev();
	memset(g_dev_buffer, 0, DEV_BUFFER_SIZE);

	/* Test freeze I/O during snapshot */

	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	channel = spdk_bs_alloc_io_channel(bs);
	bs_channel = spdk_io_channel_get_ctx(channel);

	/* Create blob with 10 clusters */
	spdk_blob_opts_init(&opts);
	opts.num_clusters = 10;
	opts.thin_provision = false;

	spdk_bs_create_blob_ext(bs, &opts, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 10);

	/* Enable explicitly calling callbacks. On each read/write to back device
	 * execution will stop and wait until _bs_flush_scheduler is called */
	g_scheduler_delay = true;

	spdk_bs_create_snapshot(bs, blobid, NULL, blob_op_with_id_complete, NULL);

	/* This is implementation specific.
	 * Flag 'frozen_io' is set in _spdk_bs_snapshot_freeze_cpl callback.
	 * Four async I/O operations happen before that. */

	_bs_flush_scheduler(4);

	CU_ASSERT(TAILQ_EMPTY(&bs_channel->queued_io));

	/* Blob I/O should be frozen here */
	CU_ASSERT(blob->frozen_refcnt == 1);

	/* Write to the blob */
	spdk_blob_io_write(blob, channel, payload_write, 0, num_of_pages, blob_op_complete, NULL);

	/* Verify that I/O is queued */
	CU_ASSERT(!TAILQ_EMPTY(&bs_channel->queued_io));
	/* Verify that payload is not written to disk */
	CU_ASSERT(memcmp(payload_zero, &g_dev_buffer[blob->active.clusters[0]*SPDK_BS_PAGE_SIZE],
			 SPDK_BS_PAGE_SIZE) == 0);

	/* Disable scheduler delay.
	 * Finish all operations including spdk_bs_create_snapshot */
	g_scheduler_delay = false;
	_bs_flush_scheduler(1);

	/* Verify snapshot */
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);

	/* Verify that blob has unset frozen_io */
	CU_ASSERT(blob->frozen_refcnt == 0);

	/* Verify that postponed I/O completed successfully by comparing payload */
	spdk_blob_io_read(blob, channel, payload_read, 0, num_of_pages, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_write, payload_read, num_of_pages * SPDK_BS_PAGE_SIZE) == 0);

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_free_io_channel(channel);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
blob_clone(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_blob_opts opts;
	struct spdk_blob *blob, *snapshot, *clone;
	spdk_blob_id blobid, cloneid, snapshotid;
	struct spdk_blob_xattr_opts xattrs;
	const void *value;
	size_t value_len;
	int rc;

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

	/* Create snapshot */
	spdk_bs_create_snapshot(bs, blobid, NULL, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	snapshotid = g_blobid;

	spdk_bs_open_blob(bs, snapshotid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	snapshot = g_blob;
	CU_ASSERT(snapshot->data_ro == true)
	CU_ASSERT(snapshot->md_ro == true)
	CU_ASSERT(spdk_blob_get_num_clusters(snapshot) == 10);

	spdk_blob_close(snapshot, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* Create clone from snapshot with xattrs */
	xattrs.names = g_xattr_names;
	xattrs.get_value = _get_xattr_value;
	xattrs.count = 3;
	xattrs.ctx = &g_ctx;

	spdk_bs_create_clone(bs, snapshotid, &xattrs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	cloneid = g_blobid;

	spdk_bs_open_blob(bs, cloneid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	clone = g_blob;
	CU_ASSERT(clone->data_ro == false)
	CU_ASSERT(clone->md_ro == false)
	CU_ASSERT(spdk_blob_get_num_clusters(clone) == 10);

	rc = spdk_blob_get_xattr_value(clone, g_xattr_names[0], &value, &value_len);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(value_len == strlen(g_xattr_values[0]));
	CU_ASSERT_NSTRING_EQUAL_FATAL(value, g_xattr_values[0], value_len);

	rc = spdk_blob_get_xattr_value(clone, g_xattr_names[1], &value, &value_len);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(value_len == strlen(g_xattr_values[1]));
	CU_ASSERT_NSTRING_EQUAL((char *)value, g_xattr_values[1], value_len);

	rc = spdk_blob_get_xattr_value(clone, g_xattr_names[2], &value, &value_len);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(value_len == strlen(g_xattr_values[2]));
	CU_ASSERT_NSTRING_EQUAL((char *)value, g_xattr_values[2], value_len);


	spdk_blob_close(clone, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* Try to create clone from not read only blob */
	spdk_bs_create_clone(bs, blobid, NULL, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == -EINVAL);
	CU_ASSERT(g_blobid == SPDK_BLOBID_INVALID);

	/* Mark blob as read only */
	spdk_blob_set_read_only(blob);
	spdk_blob_sync_md(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* Create clone from read only blob */
	spdk_bs_create_clone(bs, blobid, NULL, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	cloneid = g_blobid;

	spdk_bs_open_blob(bs, cloneid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	clone = g_blob;
	CU_ASSERT(clone->data_ro == false)
	CU_ASSERT(clone->md_ro == false)
	CU_ASSERT(spdk_blob_get_num_clusters(clone) == 10);

	spdk_blob_close(clone, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;

}

static void
_blob_inflate(bool decouple_parent)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_blob_opts opts;
	struct spdk_blob *blob, *snapshot;
	spdk_blob_id blobid, snapshotid;
	struct spdk_io_channel *channel;
	uint64_t free_clusters;

	dev = init_dev();

	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	channel = spdk_bs_alloc_io_channel(bs);
	SPDK_CU_ASSERT_FATAL(channel != NULL);

	/* Create blob with 10 clusters */

	spdk_blob_opts_init(&opts);
	opts.num_clusters = 10;
	opts.thin_provision = true;

	spdk_bs_create_blob_ext(bs, &opts, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;

	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 10)
	CU_ASSERT(spdk_blob_is_thin_provisioned(blob) == true);

	/* 1) Blob with no parent */
	if (decouple_parent) {
		/* Decouple parent of blob with no parent (should fail) */
		spdk_bs_blob_decouple_parent(bs, channel, blobid, blob_op_complete, NULL);
		CU_ASSERT(g_bserrno != 0);
	} else {
		/* Inflate of thin blob with no parent should made it thick */
		spdk_bs_inflate_blob(bs, channel, blobid, blob_op_complete, NULL);
		CU_ASSERT(g_bserrno == 0);
		CU_ASSERT(spdk_blob_is_thin_provisioned(blob) == false);
	}

	spdk_bs_create_snapshot(bs, blobid, NULL, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	snapshotid = g_blobid;

	CU_ASSERT(spdk_blob_is_thin_provisioned(blob) == true);
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 10)

	spdk_bs_open_blob(bs, snapshotid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	snapshot = g_blob;
	CU_ASSERT(snapshot->data_ro == true)
	CU_ASSERT(snapshot->md_ro == true)
	CU_ASSERT(spdk_blob_get_num_clusters(snapshot) == 10);

	spdk_blob_close(snapshot, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	free_clusters = spdk_bs_free_cluster_count(bs);

	/* 2) Blob with parent */
	if (!decouple_parent) {
		/* Do full blob inflation */
		spdk_bs_inflate_blob(bs, channel, blobid, blob_op_complete, NULL);
		CU_ASSERT(g_bserrno == 0);
		/* all 10 clusters should be allocated */
		CU_ASSERT(spdk_bs_free_cluster_count(bs) == free_clusters - 10);
	} else {
		/* Decouple parent of blob */
		spdk_bs_blob_decouple_parent(bs, channel, blobid, blob_op_complete, NULL);
		CU_ASSERT(g_bserrno == 0);
		/* when only parent is removed, none of the clusters should be allocated */
		CU_ASSERT(spdk_bs_free_cluster_count(bs) == free_clusters);
	}

	/* Now, it should be possible to delete snapshot */
	spdk_bs_delete_blob(bs, snapshotid, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 10)
	CU_ASSERT(spdk_blob_is_thin_provisioned(blob) == decouple_parent);

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;

	spdk_bs_free_io_channel(channel);
}

static void
blob_inflate(void)
{
	_blob_inflate(false);
	_blob_inflate(true);
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
	blob->md_ro = true;
	spdk_blob_resize(blob, 5, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == -EPERM);
	blob->md_ro = false;

	/* The blob started at 0 clusters. Resize it to be 5. */
	spdk_blob_resize(blob, 5, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT((free_clusters - 5) == spdk_bs_free_cluster_count(bs));

	/* Shrink the blob to 3 clusters. This will not actually release
	 * the old clusters until the blob is synced.
	 */
	spdk_blob_resize(blob, 3, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	/* Verify there are still 5 clusters in use */
	CU_ASSERT((free_clusters - 5) == spdk_bs_free_cluster_count(bs));

	spdk_blob_sync_md(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	/* Now there are only 3 clusters in use */
	CU_ASSERT((free_clusters - 3) == spdk_bs_free_cluster_count(bs));

	/* Resize the blob to be 10 clusters. Growth takes effect immediately. */
	spdk_blob_resize(blob, 10, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT((free_clusters - 10) == spdk_bs_free_cluster_count(bs));

	/* Try to resize the blob to size larger than blobstore. */
	spdk_blob_resize(blob, bs->total_clusters + 1, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == -ENOSPC);

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
	struct spdk_bs_opts opts;
	spdk_blob_id blobid;
	int rc;

	dev = init_dev();
	spdk_bs_opts_init(&opts);
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "TESTTYPE");

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

	rc = spdk_blob_set_read_only(blob);
	CU_ASSERT(rc == 0);

	CU_ASSERT(blob->data_ro == false);
	CU_ASSERT(blob->md_ro == false);

	spdk_blob_sync_md(blob, bs_op_complete, NULL);

	CU_ASSERT(blob->data_ro == true);
	CU_ASSERT(blob->md_ro == true);
	CU_ASSERT(blob->data_ro_flags & SPDK_BLOB_READ_ONLY);

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;

	CU_ASSERT(blob->data_ro == true);
	CU_ASSERT(blob->md_ro == true);
	CU_ASSERT(blob->data_ro_flags & SPDK_BLOB_READ_ONLY);

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
	g_blob = NULL;
	g_blobid = 0;

	/* Load an existing blob store */
	dev = init_dev();
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "TESTTYPE");
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	spdk_bs_open_blob(g_bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;

	CU_ASSERT(blob->data_ro == true);
	CU_ASSERT(blob->md_ro == true);
	CU_ASSERT(blob->data_ro_flags & SPDK_BLOB_READ_ONLY);

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
	spdk_blob_io_write(blob, channel, payload, 0, 1, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == -EINVAL);

	/* Resize the blob */
	spdk_blob_resize(blob, 5, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* Confirm that write fails if blob is marked read-only. */
	blob->data_ro = true;
	spdk_blob_io_write(blob, channel, payload, 0, 1, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == -EPERM);
	blob->data_ro = false;

	/* Write to the blob */
	spdk_blob_io_write(blob, channel, payload, 0, 1, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* Write starting beyond the end */
	spdk_blob_io_write(blob, channel, payload, 5 * pages_per_cluster, 1, blob_op_complete,
			   NULL);
	CU_ASSERT(g_bserrno == -EINVAL);

	/* Write starting at a valid location but going off the end */
	spdk_blob_io_write(blob, channel, payload, 4 * pages_per_cluster, pages_per_cluster + 1,
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
	spdk_blob_io_read(blob, channel, payload, 0, 1, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == -EINVAL);

	/* Resize the blob */
	spdk_blob_resize(blob, 5, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* Confirm that read passes if blob is marked read-only. */
	blob->data_ro = true;
	spdk_blob_io_read(blob, channel, payload, 0, 1, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	blob->data_ro = false;

	/* Read from the blob */
	spdk_blob_io_read(blob, channel, payload, 0, 1, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* Read starting beyond the end */
	spdk_blob_io_read(blob, channel, payload, 5 * pages_per_cluster, 1, blob_op_complete,
			  NULL);
	CU_ASSERT(g_bserrno == -EINVAL);

	/* Read starting at a valid location but going off the end */
	spdk_blob_io_read(blob, channel, payload, 4 * pages_per_cluster, pages_per_cluster + 1,
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

	spdk_blob_resize(blob, 32, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	memset(payload_write, 0xE5, sizeof(payload_write));
	spdk_blob_io_write(blob, channel, payload_write, 4, 10, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	memset(payload_read, 0x00, sizeof(payload_read));
	spdk_blob_io_read(blob, channel, payload_read, 4, 10, blob_op_complete, NULL);
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

	spdk_blob_resize(blob, 2, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

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
	spdk_blob_io_writev(blob, channel, iov_write, 3, 250, 10, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	memset(payload_read, 0xAA, sizeof(payload_read));
	iov_read[0].iov_base = payload_read;
	iov_read[0].iov_len = 3 * 4096;
	iov_read[1].iov_base = payload_read + 3 * 4096;
	iov_read[1].iov_len = 4 * 4096;
	iov_read[2].iov_base = payload_read + 7 * 4096;
	iov_read[2].iov_len = 3 * 4096;
	spdk_blob_io_readv(blob, channel, iov_read, 3, 250, 10, blob_op_complete, NULL);
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

	spdk_blob_resize(blob, 2, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

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
	MOCK_SET(calloc, NULL);
	req_count = bs_channel_get_req_count(channel);
	spdk_blob_io_writev(blob, channel, iov_write, 3, 250, 10, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno = -ENOMEM);
	CU_ASSERT(req_count == bs_channel_get_req_count(channel));
	MOCK_CLEAR(calloc);

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

	spdk_blob_resize(blob, 2, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* Verify that writev failed if read_only flag is set. */
	blob->data_ro = true;
	iov_write.iov_base = payload_write;
	iov_write.iov_len = sizeof(payload_write);
	spdk_blob_io_writev(blob, channel, &iov_write, 1, 0, 1, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == -EPERM);

	/* Verify that reads pass if data_ro flag is set. */
	iov_read.iov_base = payload_read;
	iov_read.iov_len = sizeof(payload_read);
	spdk_blob_io_readv(blob, channel, &iov_read, 1, 0, 1, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_free_io_channel(channel);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
_blob_io_read_no_split(struct spdk_blob *blob, struct spdk_io_channel *channel,
		       uint8_t *payload, uint64_t offset, uint64_t length,
		       spdk_blob_op_complete cb_fn, void *cb_arg)
{
	uint64_t i;
	uint8_t *buf;
	uint64_t page_size = spdk_bs_get_page_size(blob->bs);

	/* To be sure that operation is NOT splitted, read one page at the time */
	buf = payload;
	for (i = 0; i < length; i++) {
		spdk_blob_io_read(blob, channel, buf, i + offset, 1, blob_op_complete, NULL);
		if (g_bserrno != 0) {
			/* Pass the error code up */
			break;
		}
		buf += page_size;
	}

	cb_fn(cb_arg, g_bserrno);
}

static void
_blob_io_write_no_split(struct spdk_blob *blob, struct spdk_io_channel *channel,
			uint8_t *payload, uint64_t offset, uint64_t length,
			spdk_blob_op_complete cb_fn, void *cb_arg)
{
	uint64_t i;
	uint8_t *buf;
	uint64_t page_size = spdk_bs_get_page_size(blob->bs);

	/* To be sure that operation is NOT splitted, write one page at the time */
	buf = payload;
	for (i = 0; i < length; i++) {
		spdk_blob_io_write(blob, channel, buf, i + offset, 1, blob_op_complete, NULL);
		if (g_bserrno != 0) {
			/* Pass the error code up */
			break;
		}
		buf += page_size;
	}

	cb_fn(cb_arg, g_bserrno);
}

static void
blob_operation_split_rw(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_blob *blob;
	struct spdk_io_channel *channel;
	struct spdk_blob_opts opts;
	spdk_blob_id blobid;
	uint64_t cluster_size;

	uint64_t payload_size;
	uint8_t *payload_read;
	uint8_t *payload_write;
	uint8_t *payload_pattern;

	uint64_t page_size;
	uint64_t pages_per_cluster;
	uint64_t pages_per_payload;

	uint64_t i;

	dev = init_dev();

	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	cluster_size = spdk_bs_get_cluster_size(bs);
	page_size = spdk_bs_get_page_size(bs);
	pages_per_cluster = cluster_size / page_size;
	pages_per_payload = pages_per_cluster * 5;
	payload_size = cluster_size * 5;

	payload_read = malloc(payload_size);
	SPDK_CU_ASSERT_FATAL(payload_read != NULL);

	payload_write = malloc(payload_size);
	SPDK_CU_ASSERT_FATAL(payload_write != NULL);

	payload_pattern = malloc(payload_size);
	SPDK_CU_ASSERT_FATAL(payload_pattern != NULL);

	/* Prepare random pattern to write */
	memset(payload_pattern, 0xFF, payload_size);
	for (i = 0; i < pages_per_payload; i++) {
		*((uint64_t *)(payload_pattern + page_size * i)) = (i + 1);
	}

	channel = spdk_bs_alloc_io_channel(bs);
	SPDK_CU_ASSERT_FATAL(channel != NULL);

	/* Create blob */
	spdk_blob_opts_init(&opts);
	opts.thin_provision = false;
	opts.num_clusters = 5;

	spdk_bs_create_blob_ext(bs, &opts, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;

	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 5);

	/* Initial read should return zeroed payload */
	memset(payload_read, 0xFF, payload_size);
	spdk_blob_io_read(blob, channel, payload_read, 0, pages_per_payload, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(spdk_mem_all_zero(payload_read, payload_size));

	/* Fill whole blob except last page */
	spdk_blob_io_write(blob, channel, payload_pattern, 0, pages_per_payload - 1,
			   blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* Write last page with a pattern */
	spdk_blob_io_write(blob, channel, payload_pattern, pages_per_payload - 1, 1,
			   blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* Read whole blob and check consistency */
	memset(payload_read, 0xFF, payload_size);
	spdk_blob_io_read(blob, channel, payload_read, 0, pages_per_payload, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_pattern, payload_read, payload_size - page_size) == 0);
	CU_ASSERT(memcmp(payload_pattern, payload_read + payload_size - page_size, page_size) == 0);

	/* Fill whole blob except first page */
	spdk_blob_io_write(blob, channel, payload_pattern, 1, pages_per_payload - 1,
			   blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* Write first page with a pattern */
	spdk_blob_io_write(blob, channel, payload_pattern, 0, 1,
			   blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* Read whole blob and check consistency */
	memset(payload_read, 0xFF, payload_size);
	spdk_blob_io_read(blob, channel, payload_read, 0, pages_per_payload, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_pattern, payload_read + page_size, payload_size - page_size) == 0);
	CU_ASSERT(memcmp(payload_pattern, payload_read, page_size) == 0);


	/* Fill whole blob with a pattern (5 clusters) */

	/* 1. Read test. */
	_blob_io_write_no_split(blob, channel, payload_pattern, 0, pages_per_payload,
				blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	memset(payload_read, 0xFF, payload_size);
	spdk_blob_io_read(blob, channel, payload_read, 0, pages_per_payload, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_pattern, payload_read, payload_size) == 0);

	/* 2. Write test. */
	spdk_blob_io_write(blob, channel, payload_pattern, 0, pages_per_payload,
			   blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	memset(payload_read, 0xFF, payload_size);
	_blob_io_read_no_split(blob, channel, payload_read, 0, pages_per_payload, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_pattern, payload_read, payload_size) == 0);

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_free_io_channel(channel);

	/* Unload the blob store */
	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
	g_blob = NULL;
	g_blobid = 0;

	free(payload_read);
	free(payload_write);
	free(payload_pattern);
}

static void
blob_operation_split_rw_iov(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_blob *blob;
	struct spdk_io_channel *channel;
	struct spdk_blob_opts opts;
	spdk_blob_id blobid;
	uint64_t cluster_size;

	uint64_t payload_size;
	uint8_t *payload_read;
	uint8_t *payload_write;
	uint8_t *payload_pattern;

	uint64_t page_size;
	uint64_t pages_per_cluster;
	uint64_t pages_per_payload;

	struct iovec iov_read[2];
	struct iovec iov_write[2];

	uint64_t i, j;

	dev = init_dev();

	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	cluster_size = spdk_bs_get_cluster_size(bs);
	page_size = spdk_bs_get_page_size(bs);
	pages_per_cluster = cluster_size / page_size;
	pages_per_payload = pages_per_cluster * 5;
	payload_size = cluster_size * 5;

	payload_read = malloc(payload_size);
	SPDK_CU_ASSERT_FATAL(payload_read != NULL);

	payload_write = malloc(payload_size);
	SPDK_CU_ASSERT_FATAL(payload_write != NULL);

	payload_pattern = malloc(payload_size);
	SPDK_CU_ASSERT_FATAL(payload_pattern != NULL);

	/* Prepare random pattern to write */
	for (i = 0; i < pages_per_payload; i++) {
		for (j = 0; j < page_size / sizeof(uint64_t); j++) {
			uint64_t *tmp;

			tmp = (uint64_t *)payload_pattern;
			tmp += ((page_size * i) / sizeof(uint64_t)) + j;
			*tmp = i + 1;
		}
	}

	channel = spdk_bs_alloc_io_channel(bs);
	SPDK_CU_ASSERT_FATAL(channel != NULL);

	/* Create blob */
	spdk_blob_opts_init(&opts);
	opts.thin_provision = false;
	opts.num_clusters = 5;

	spdk_bs_create_blob_ext(bs, &opts, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;

	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 5);

	/* Initial read should return zeroes payload */
	memset(payload_read, 0xFF, payload_size);
	iov_read[0].iov_base = payload_read;
	iov_read[0].iov_len = cluster_size * 3;
	iov_read[1].iov_base = payload_read + cluster_size * 3;
	iov_read[1].iov_len = cluster_size * 2;
	spdk_blob_io_readv(blob, channel, iov_read, 2, 0, pages_per_payload, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(spdk_mem_all_zero(payload_read, payload_size));

	/* First of iovs fills whole blob except last page and second of iovs writes last page
	 *  with a pattern. */
	iov_write[0].iov_base = payload_pattern;
	iov_write[0].iov_len = payload_size - page_size;
	iov_write[1].iov_base = payload_pattern;
	iov_write[1].iov_len = page_size;
	spdk_blob_io_writev(blob, channel, iov_write, 2, 0, pages_per_payload, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* Read whole blob and check consistency */
	memset(payload_read, 0xFF, payload_size);
	iov_read[0].iov_base = payload_read;
	iov_read[0].iov_len = cluster_size * 2;
	iov_read[1].iov_base = payload_read + cluster_size * 2;
	iov_read[1].iov_len = cluster_size * 3;
	spdk_blob_io_readv(blob, channel, iov_read, 2, 0, pages_per_payload, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_pattern, payload_read, payload_size - page_size) == 0);
	CU_ASSERT(memcmp(payload_pattern, payload_read + payload_size - page_size, page_size) == 0);

	/* First of iovs fills only first page and second of iovs writes whole blob except
	 *  first page with a pattern. */
	iov_write[0].iov_base = payload_pattern;
	iov_write[0].iov_len = page_size;
	iov_write[1].iov_base = payload_pattern;
	iov_write[1].iov_len = payload_size - page_size;
	spdk_blob_io_writev(blob, channel, iov_write, 2, 0, pages_per_payload, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* Read whole blob and check consistency */
	memset(payload_read, 0xFF, payload_size);
	iov_read[0].iov_base = payload_read;
	iov_read[0].iov_len = cluster_size * 4;
	iov_read[1].iov_base = payload_read + cluster_size * 4;
	iov_read[1].iov_len = cluster_size;
	spdk_blob_io_readv(blob, channel, iov_read, 2, 0, pages_per_payload, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_pattern, payload_read + page_size, payload_size - page_size) == 0);
	CU_ASSERT(memcmp(payload_pattern, payload_read, page_size) == 0);


	/* Fill whole blob with a pattern (5 clusters) */

	/* 1. Read test. */
	_blob_io_write_no_split(blob, channel, payload_pattern, 0, pages_per_payload,
				blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	memset(payload_read, 0xFF, payload_size);
	iov_read[0].iov_base = payload_read;
	iov_read[0].iov_len = cluster_size;
	iov_read[1].iov_base = payload_read + cluster_size;
	iov_read[1].iov_len = cluster_size * 4;
	spdk_blob_io_readv(blob, channel, iov_read, 2, 0, pages_per_payload, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_pattern, payload_read, payload_size) == 0);

	/* 2. Write test. */
	iov_write[0].iov_base = payload_read;
	iov_write[0].iov_len = cluster_size * 2;
	iov_write[1].iov_base = payload_read + cluster_size * 2;
	iov_write[1].iov_len = cluster_size * 3;
	spdk_blob_io_writev(blob, channel, iov_write, 2, 0, pages_per_payload, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	memset(payload_read, 0xFF, payload_size);
	_blob_io_read_no_split(blob, channel, payload_read, 0, pages_per_payload, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_pattern, payload_read, payload_size) == 0);

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_free_io_channel(channel);

	/* Unload the blob store */
	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
	g_blob = NULL;
	g_blobid = 0;

	free(payload_read);
	free(payload_write);
	free(payload_pattern);
}

static void
blob_unmap(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_blob *blob;
	struct spdk_io_channel *channel;
	spdk_blob_id blobid;
	struct spdk_blob_opts opts;
	uint8_t payload[4096];
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

	spdk_blob_resize(blob, 10, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

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
		spdk_blob_io_read(blob, channel, &payload, i * SPDK_BLOB_OPTS_CLUSTER_SZ / 4096, 1,
				  blob_op_complete, NULL);
		CU_ASSERT(g_bserrno == 0);
		CU_ASSERT(payload[0] == 0xFF);
	}

	/* Mark some clusters as unallocated */
	blob->active.clusters[1] = 0;
	blob->active.clusters[2] = 0;
	blob->active.clusters[3] = 0;
	blob->active.clusters[6] = 0;
	blob->active.clusters[8] = 0;

	/* Unmap clusters by resizing to 0 */
	spdk_blob_resize(blob, 0, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_blob_sync_md(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* Confirm that only 'allocated' clusters were unmapped */
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
	blob->md_ro = true;
	rc = spdk_blob_set_xattr(blob, "name", "log.txt", strlen("log.txt") + 1);
	CU_ASSERT(rc == -EPERM);

	blob->md_ro = false;
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
	blob->md_ro = true;
	rc = spdk_blob_get_xattr_value(blob, "length", &value, &value_len);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(*(uint64_t *)value == length);
	CU_ASSERT(value_len == 8);
	blob->md_ro = false;

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
	blob->md_ro = true;
	rc = spdk_blob_remove_xattr(blob, "name");
	CU_ASSERT(rc == -EPERM);

	blob->md_ro = false;
	rc = spdk_blob_remove_xattr(blob, "name");
	CU_ASSERT(rc == 0);

	rc = spdk_blob_remove_xattr(blob, "foobar");
	CU_ASSERT(rc == -ENOENT);

	/* Set internal xattr */
	length = 7898;
	rc = _spdk_blob_set_xattr(blob, "internal", &length, sizeof(length), true);
	CU_ASSERT(rc == 0);
	rc = _spdk_blob_get_xattr_value(blob, "internal", &value, &value_len, true);
	CU_ASSERT(rc == 0);
	CU_ASSERT(*(uint64_t *)value == length);
	/* try to get public xattr with same name */
	rc = spdk_blob_get_xattr_value(blob, "internal", &value, &value_len);
	CU_ASSERT(rc != 0);
	rc = _spdk_blob_get_xattr_value(blob, "internal", &value, &value_len, false);
	CU_ASSERT(rc != 0);
	/* Check if SPDK_BLOB_INTERNAL_XATTR is set */
	CU_ASSERT((blob->invalid_flags & SPDK_BLOB_INTERNAL_XATTR) ==
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

	rc = _spdk_blob_get_xattr_value(blob, "internal", &value, &value_len, true);
	CU_ASSERT(rc == 0);
	CU_ASSERT(*(uint64_t *)value == length);

	/* try to get internal xattr trough public call */
	rc = spdk_blob_get_xattr_value(blob, "internal", &value, &value_len);
	CU_ASSERT(rc != 0);

	rc = _spdk_blob_remove_xattr(blob, "internal", true);
	CU_ASSERT(rc == 0);

	CU_ASSERT((blob->invalid_flags & SPDK_BLOB_INTERNAL_XATTR) == 0);

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

	dev = init_dev();
	spdk_bs_opts_init(&opts);
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "TESTTYPE");

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
	spdk_blob_resize(blob, 10, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

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

	/* Load should fail for device with an unsupported blocklen */
	dev = init_dev();
	dev->blocklen = SPDK_BS_PAGE_SIZE * 2;
	spdk_bs_load(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == -EINVAL);

	/* Load should when max_md_ops is set to zero */
	dev = init_dev();
	spdk_bs_opts_init(&opts);
	opts.max_md_ops = 0;
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == -EINVAL);

	/* Load should when max_channel_ops is set to zero */
	dev = init_dev();
	spdk_bs_opts_init(&opts);
	opts.max_channel_ops = 0;
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == -EINVAL);

	/* Load an existing blob store */
	dev = init_dev();
	spdk_bs_opts_init(&opts);
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "TESTTYPE");
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	super_block = (struct spdk_bs_super_block *)g_dev_buffer;
	CU_ASSERT(super_block->clean == 1);
	CU_ASSERT(super_block->size == dev->blockcnt * dev->blocklen);

	spdk_bs_open_blob(g_bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;

	/* Verify that blobstore is marked dirty after first metadata sync */
	spdk_blob_sync_md(blob, blob_op_complete, NULL);
	CU_ASSERT(super_block->clean == 1);

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

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;

	/* Load should fail: bdev size < saved size */
	dev = init_dev();
	dev->blockcnt /= 2;

	spdk_bs_opts_init(&opts);
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "TESTTYPE");
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);

	CU_ASSERT(g_bserrno == -EILSEQ);

	/* Load should succeed: bdev size > saved size */
	dev = init_dev();
	dev->blockcnt *= 4;

	spdk_bs_opts_init(&opts);
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "TESTTYPE");
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);

	CU_ASSERT(g_bserrno == 0);
	spdk_bs_unload(g_bs, bs_op_complete, NULL);


	/* Test compatibility mode */

	dev = init_dev();
	super_block->size = 0;
	super_block->crc = _spdk_blob_md_page_calc_crc(super_block);

	spdk_bs_opts_init(&opts);
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "TESTTYPE");
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	/* Create a blob */
	spdk_bs_create_blob(g_bs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);

	/* Blobstore should update number of blocks in super_block */
	CU_ASSERT(super_block->size == dev->blockcnt * dev->blocklen);
	CU_ASSERT(super_block->clean == 0);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(super_block->clean == 1);
	g_bs = NULL;

}

static void
bs_load_custom_cluster_size(void)
{
	struct spdk_bs_dev *dev;
	struct spdk_bs_super_block *super_block;
	struct spdk_bs_opts opts;
	uint32_t custom_cluster_size = 4194304; /* 4MiB */
	uint32_t cluster_sz;
	uint64_t total_clusters;

	dev = init_dev();
	spdk_bs_opts_init(&opts);
	opts.cluster_sz = custom_cluster_size;
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "TESTTYPE");

	/* Initialize a new blob store */
	spdk_bs_init(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	cluster_sz = g_bs->cluster_sz;
	total_clusters = g_bs->total_clusters;

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
	spdk_bs_opts_init(&opts);
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "TESTTYPE");
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	/* Compare cluster size and number to one after initialization */
	CU_ASSERT(cluster_sz == g_bs->cluster_sz);
	CU_ASSERT(total_clusters == g_bs->total_clusters);

	super_block = (struct spdk_bs_super_block *)g_dev_buffer;
	CU_ASSERT(super_block->clean == 1);
	CU_ASSERT(super_block->size == dev->blockcnt * dev->blocklen);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(super_block->clean == 1);
	g_bs = NULL;
}

static void
bs_type(void)
{
	struct spdk_bs_dev *dev;
	struct spdk_bs_opts opts;

	dev = init_dev();
	spdk_bs_opts_init(&opts);
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "TESTTYPE");

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
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "NONEXISTING");
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno != 0);

	/* Load with empty blobstore type */
	dev = init_dev();
	memset(opts.bstype.bstype, 0, sizeof(opts.bstype.bstype));
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;

	/* Initialize a new blob store with empty bstype */
	dev = init_dev();
	memset(opts.bstype.bstype, 0, sizeof(opts.bstype.bstype));
	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;

	/* Load non existing blobstore type */
	dev = init_dev();
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "NONEXISTING");
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno != 0);

	/* Load with empty blobstore type */
	dev = init_dev();
	memset(opts.bstype.bstype, 0, sizeof(opts.bstype.bstype));
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
bs_super_block(void)
{
	struct spdk_bs_dev *dev;
	struct spdk_bs_super_block *super_block;
	struct spdk_bs_opts opts;
	struct spdk_bs_super_block_ver1 super_block_v1;

	dev = init_dev();
	spdk_bs_opts_init(&opts);
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "TESTTYPE");

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
	memset(opts.bstype.bstype, 0, sizeof(opts.bstype.bstype));
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno != 0);

	/* Create a new blob store with super block version 1 */
	dev = init_dev();
	super_block_v1.version = 1;
	memcpy(super_block_v1.signature, "SPDKBLOB", sizeof(super_block_v1.signature));
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

	memset(opts.bstype.bstype, 0, sizeof(opts.bstype.bstype));
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
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
	CU_ASSERT(g_bserrno == -EINVAL);
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
	int i;

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

		spdk_blob_resize(g_blob, 10, blob_op_complete, NULL);
		CU_ASSERT(g_bserrno == 0);

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

	/* Initialize a new blob store */
	dev = init_dev();
	spdk_bs_opts_init(&opts);
	spdk_bs_init(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	/* Destroy the blob store */
	g_bserrno = -1;
	spdk_bs_destroy(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* Loading an non-existent blob store should fail. */
	g_bs = NULL;
	dev = init_dev();

	g_bserrno = 0;
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno != 0);
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
		spdk_blob_resize(blob[i % 2], (i / 2) + 1, blob_op_complete, NULL);
		CU_ASSERT(g_bserrno == 0);
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

	/* Load an existing blob store */
	g_bserrno = 0;
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == -EILSEQ);
}

/* For blob dirty shutdown test case we do the following sub-test cases:
 * 1 Initialize new blob store and create 1 super blob with some xattrs, then we
 *   dirty shutdown and reload the blob store and verify the xattrs.
 * 2 Resize the blob from 10 clusters to 20 clusters and then dirty shutdown,
 *   reload the blob store and verify the clusters number.
 * 3 Create the second blob and then dirty shutdown, reload the blob store
 *   and verify the second blob.
 * 4 Delete the second blob and then dirty shutdown, reload the blob store
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
	spdk_blob_resize(blob, 10, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* Set the blob as the super blob */
	spdk_bs_set_super(g_bs, blobid1, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

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

	/* Get the super blob */
	spdk_bs_get_super(g_bs, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(blobid1 == g_blobid);

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
	spdk_blob_resize(blob, 20, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

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
	spdk_blob_resize(blob, 10, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

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
	spdk_blob_resize(blob_data_ro, 10, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* Set the xattr to check if flags are serialized
	 * when blob has non zero number of xattrs */
	rc = spdk_blob_set_xattr(blob_md_ro, "name", "log.txt", strlen("log.txt") + 1);
	CU_ASSERT(rc == 0);

	blob_invalid->invalid_flags = (1ULL << 63);
	blob_invalid->state = SPDK_BLOB_STATE_DIRTY;
	blob_data_ro->data_ro_flags = (1ULL << 62);
	blob_data_ro->state = SPDK_BLOB_STATE_DIRTY;
	blob_md_ro->md_ro_flags = (1ULL << 61);
	blob_md_ro->state = SPDK_BLOB_STATE_DIRTY;

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
	CU_ASSERT(blob_data_ro->data_ro == true);
	CU_ASSERT(blob_data_ro->md_ro == true);
	CU_ASSERT(spdk_blob_get_num_clusters(blob_data_ro) == 10);

	g_blob = NULL;
	g_bserrno = -1;
	spdk_bs_open_blob(g_bs, blobid_md_ro, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob_md_ro = g_blob;
	CU_ASSERT(blob_md_ro->data_ro == false);
	CU_ASSERT(blob_md_ro->md_ro == true);

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
	CU_ASSERT(super->clean == 1);

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

	spdk_bs_create_blob_ext(bs, &opts, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;

	CU_ASSERT(blob->active.num_clusters == 0);
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 0);

	/* The blob started at 0 clusters. Resize it to be 5, but still unallocated. */
	spdk_blob_resize(blob, 5, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(blob->active.num_clusters == 5);
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 5);

	/* Grow it to 1TB - still unallocated */
	spdk_blob_resize(blob, 262144, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(blob->active.num_clusters == 262144);
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 262144);

	spdk_blob_sync_md(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	/* Sync must not change anything */
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(blob->active.num_clusters == 262144);
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 262144);
	/* Since clusters are not allocated,
	 * number of metadata pages is expected to be minimal.
	 */
	CU_ASSERT(blob->active.num_pages == 1);

	/* Shrink the blob to 3 clusters - still unallocated */
	spdk_blob_resize(blob, 3, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(blob->active.num_clusters == 3);
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 3);

	spdk_blob_sync_md(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	/* Sync must not change anything */
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(blob->active.num_clusters == 3);
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

	/* Check that clusters allocation and size is still the same */
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(blob->active.num_clusters == 3);

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

	CU_ASSERT(blob->active.num_clusters == 4);
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 4);
	CU_ASSERT(blob->active.clusters[1] == 0);

	_spdk_bs_claim_cluster(bs, 0xF);
	_spdk_blob_insert_cluster_on_md_thread(blob, 1, 0xF, blob_op_complete, NULL);

	CU_ASSERT(blob->active.clusters[1] != 0);

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

	CU_ASSERT(blob->active.clusters[1] != 0);

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
	struct spdk_io_channel *channel;
	struct spdk_blob_opts opts;
	spdk_blob_id blobid;
	uint64_t free_clusters;
	uint64_t page_size;
	uint8_t payload_read[10 * 4096];
	uint8_t payload_write[10 * 4096];
	uint64_t write_bytes;
	uint64_t read_bytes;

	dev = init_dev();

	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;
	free_clusters = spdk_bs_free_cluster_count(bs);
	page_size = spdk_bs_get_page_size(bs);

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

	CU_ASSERT(blob->active.num_clusters == 0);

	/* The blob started at 0 clusters. Resize it to be 5, but still unallocated. */
	spdk_blob_resize(blob, 5, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(blob->active.num_clusters == 5);

	spdk_blob_sync_md(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	/* Sync must not change anything */
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(blob->active.num_clusters == 5);

	/* Payload should be all zeros from unallocated clusters */
	memset(payload_read, 0xFF, sizeof(payload_read));
	spdk_blob_io_read(blob, channel, payload_read, 4, 10, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(zero, payload_read, 10 * 4096) == 0);

	write_bytes = g_dev_write_bytes;
	read_bytes = g_dev_read_bytes;

	memset(payload_write, 0xE5, sizeof(payload_write));
	spdk_blob_io_write(blob, channel, payload_write, 4, 10, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(free_clusters != spdk_bs_free_cluster_count(bs));
	/* For thin-provisioned blob we need to write 10 pages plus one page metadata and
	 * read 0 bytes */
	CU_ASSERT(g_dev_write_bytes - write_bytes == page_size * 11);
	CU_ASSERT(g_dev_read_bytes - read_bytes == 0);

	spdk_blob_io_read(blob, channel, payload_read, 4, 10, blob_op_complete, NULL);
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
	struct spdk_io_channel *channel;
	struct spdk_blob_opts opts;
	spdk_blob_id blobid;
	uint64_t free_clusters;
	uint8_t payload_read[10 * 4096];
	uint8_t payload_write[10 * 4096];
	struct iovec iov_read[3];
	struct iovec iov_write[3];

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

	CU_ASSERT(blob->active.num_clusters == 0);

	/* The blob started at 0 clusters. Resize it to be 5, but still unallocated. */
	spdk_blob_resize(blob, 5, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(blob->active.num_clusters == 5);

	spdk_blob_sync_md(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	/* Sync must not change anything */
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(blob->active.num_clusters == 5);

	/* Payload should be all zeros from unallocated clusters */
	memset(payload_read, 0xAA, sizeof(payload_read));
	iov_read[0].iov_base = payload_read;
	iov_read[0].iov_len = 3 * 4096;
	iov_read[1].iov_base = payload_read + 3 * 4096;
	iov_read[1].iov_len = 4 * 4096;
	iov_read[2].iov_base = payload_read + 7 * 4096;
	iov_read[2].iov_len = 3 * 4096;
	spdk_blob_io_readv(blob, channel, iov_read, 3, 250, 10, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(zero, payload_read, 10 * 4096) == 0);

	memset(payload_write, 0xE5, sizeof(payload_write));
	iov_write[0].iov_base = payload_write;
	iov_write[0].iov_len = 1 * 4096;
	iov_write[1].iov_base = payload_write + 1 * 4096;
	iov_write[1].iov_len = 5 * 4096;
	iov_write[2].iov_base = payload_write + 6 * 4096;
	iov_write[2].iov_len = 4 * 4096;

	spdk_blob_io_writev(blob, channel, iov_write, 3, 250, 10, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	memset(payload_read, 0xAA, sizeof(payload_read));
	iov_read[0].iov_base = payload_read;
	iov_read[0].iov_len = 3 * 4096;
	iov_read[1].iov_base = payload_read + 3 * 4096;
	iov_read[1].iov_len = 4 * 4096;
	iov_read[2].iov_base = payload_read + 7 * 4096;
	iov_read[2].iov_len = 3 * 4096;
	spdk_blob_io_readv(blob, channel, iov_read, 3, 250, 10, blob_op_complete, NULL);
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

struct iter_ctx {
	int		current_iter;
	spdk_blob_id	blobid[4];
};

static void
test_iter(void *arg, struct spdk_blob *blob, int bserrno)
{
	struct iter_ctx *iter_ctx = arg;
	spdk_blob_id blobid;

	CU_ASSERT(bserrno == 0);
	blobid = spdk_blob_get_id(blob);
	CU_ASSERT(blobid == iter_ctx->blobid[iter_ctx->current_iter++]);
}

static void
bs_load_iter(void)
{
	struct spdk_bs_dev *dev;
	struct iter_ctx iter_ctx = { 0 };
	struct spdk_blob *blob;
	int i, rc;
	struct spdk_bs_opts opts;

	dev = init_dev();
	spdk_bs_opts_init(&opts);
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "TESTTYPE");

	/* Initialize a new blob store */
	spdk_bs_init(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	for (i = 0; i < 4; i++) {
		g_bserrno = -1;
		g_blobid = SPDK_BLOBID_INVALID;
		spdk_bs_create_blob(g_bs, blob_op_with_id_complete, NULL);
		CU_ASSERT(g_bserrno == 0);
		CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
		iter_ctx.blobid[i] = g_blobid;

		g_bserrno = -1;
		g_blob = NULL;
		spdk_bs_open_blob(g_bs, g_blobid, blob_op_with_handle_complete, NULL);
		CU_ASSERT(g_bserrno == 0);
		CU_ASSERT(g_blob != NULL);
		blob = g_blob;

		/* Just save the blobid as an xattr for testing purposes. */
		rc = spdk_blob_set_xattr(blob, "blobid", &g_blobid, sizeof(g_blobid));
		CU_ASSERT(rc == 0);

		/* Resize the blob */
		spdk_blob_resize(blob, i, blob_op_complete, NULL);
		CU_ASSERT(g_bserrno == 0);

		spdk_blob_close(blob, blob_op_complete, NULL);
		CU_ASSERT(g_bserrno == 0);
	}

	g_bserrno = -1;
	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	dev = init_dev();
	spdk_bs_opts_init(&opts);
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "TESTTYPE");
	opts.iter_cb_fn = test_iter;
	opts.iter_cb_arg = &iter_ctx;

	/* Test blob iteration during load after a clean shutdown. */
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	/* Dirty shutdown */
	_spdk_bs_free(g_bs);

	dev = init_dev();
	spdk_bs_opts_init(&opts);
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "TESTTYPE");
	opts.iter_cb_fn = test_iter;
	iter_ctx.current_iter = 0;
	opts.iter_cb_arg = &iter_ctx;

	/* Test blob iteration during load after a dirty shutdown. */
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
blob_snapshot_rw(void)
{
	static const uint8_t zero[10 * 4096] = { 0 };
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_blob *blob, *snapshot;
	struct spdk_io_channel *channel;
	struct spdk_blob_opts opts;
	spdk_blob_id blobid, snapshotid;
	uint64_t free_clusters;
	uint64_t cluster_size;
	uint64_t page_size;
	uint8_t payload_read[10 * 4096];
	uint8_t payload_write[10 * 4096];
	uint64_t write_bytes;
	uint64_t read_bytes;

	dev = init_dev();

	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;
	free_clusters = spdk_bs_free_cluster_count(bs);
	cluster_size = spdk_bs_get_cluster_size(bs);
	page_size = spdk_bs_get_page_size(bs);

	channel = spdk_bs_alloc_io_channel(bs);
	CU_ASSERT(channel != NULL);

	spdk_blob_opts_init(&opts);
	opts.thin_provision = true;
	opts.num_clusters = 5;

	spdk_bs_create_blob_ext(bs, &opts, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;

	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 5);

	memset(payload_read, 0xFF, sizeof(payload_read));
	spdk_blob_io_read(blob, channel, payload_read, 4, 10, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(zero, payload_read, 10 * 4096) == 0);

	memset(payload_write, 0xE5, sizeof(payload_write));
	spdk_blob_io_write(blob, channel, payload_write, 4, 10, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(free_clusters != spdk_bs_free_cluster_count(bs));

	/* Create snapshot from blob */
	spdk_bs_create_snapshot(bs, blobid, NULL, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	snapshotid = g_blobid;

	spdk_bs_open_blob(bs, snapshotid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	snapshot = g_blob;
	CU_ASSERT(snapshot->data_ro == true)
	CU_ASSERT(snapshot->md_ro == true)

	CU_ASSERT(spdk_blob_get_num_clusters(snapshot) == 5)

	write_bytes = g_dev_write_bytes;
	read_bytes = g_dev_read_bytes;

	memset(payload_write, 0xAA, sizeof(payload_write));
	spdk_blob_io_write(blob, channel, payload_write, 4, 10, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(free_clusters != spdk_bs_free_cluster_count(bs));

	/* For a clone we need to allocate and copy one cluster, update one page of metadata
	 * and then write 10 pages of payload.
	 */
	CU_ASSERT(g_dev_write_bytes - write_bytes == page_size * 11 + cluster_size);
	CU_ASSERT(g_dev_read_bytes - read_bytes == cluster_size);

	spdk_blob_io_read(blob, channel, payload_read, 4, 10, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_write, payload_read, 10 * 4096) == 0);

	/* Data on snapshot should not change after write to clone */
	memset(payload_write, 0xE5, sizeof(payload_write));
	spdk_blob_io_read(snapshot, channel, payload_read, 4, 10, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_write, payload_read, 10 * 4096) == 0);

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_blob_close(snapshot, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_delete_blob(bs, blobid, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_delete_blob(bs, snapshotid, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_free_io_channel(channel);

	/* Unload the blob store */
	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
	g_blob = NULL;
	g_blobid = 0;
}

static void
blob_snapshot_rw_iov(void)
{
	static const uint8_t zero[10 * 4096] = { 0 };
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_blob *blob, *snapshot;
	struct spdk_io_channel *channel;
	struct spdk_blob_opts opts;
	spdk_blob_id blobid, snapshotid;
	uint64_t free_clusters;
	uint8_t payload_read[10 * 4096];
	uint8_t payload_write[10 * 4096];
	struct iovec iov_read[3];
	struct iovec iov_write[3];

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
	opts.num_clusters = 5;

	spdk_bs_create_blob_ext(bs, &opts, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;

	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 5);

	/* Create snapshot from blob */
	spdk_bs_create_snapshot(bs, blobid, NULL, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	snapshotid = g_blobid;

	spdk_bs_open_blob(bs, snapshotid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	snapshot = g_blob;
	CU_ASSERT(snapshot->data_ro == true)
	CU_ASSERT(snapshot->md_ro == true)
	CU_ASSERT(spdk_blob_get_num_clusters(snapshot) == 5);

	/* Payload should be all zeros from unallocated clusters */
	memset(payload_read, 0xAA, sizeof(payload_read));
	iov_read[0].iov_base = payload_read;
	iov_read[0].iov_len = 3 * 4096;
	iov_read[1].iov_base = payload_read + 3 * 4096;
	iov_read[1].iov_len = 4 * 4096;
	iov_read[2].iov_base = payload_read + 7 * 4096;
	iov_read[2].iov_len = 3 * 4096;
	spdk_blob_io_readv(blob, channel, iov_read, 3, 250, 10, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(zero, payload_read, 10 * 4096) == 0);

	memset(payload_write, 0xE5, sizeof(payload_write));
	iov_write[0].iov_base = payload_write;
	iov_write[0].iov_len = 1 * 4096;
	iov_write[1].iov_base = payload_write + 1 * 4096;
	iov_write[1].iov_len = 5 * 4096;
	iov_write[2].iov_base = payload_write + 6 * 4096;
	iov_write[2].iov_len = 4 * 4096;

	spdk_blob_io_writev(blob, channel, iov_write, 3, 250, 10, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	memset(payload_read, 0xAA, sizeof(payload_read));
	iov_read[0].iov_base = payload_read;
	iov_read[0].iov_len = 3 * 4096;
	iov_read[1].iov_base = payload_read + 3 * 4096;
	iov_read[1].iov_len = 4 * 4096;
	iov_read[2].iov_base = payload_read + 7 * 4096;
	iov_read[2].iov_len = 3 * 4096;
	spdk_blob_io_readv(blob, channel, iov_read, 3, 250, 10, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_write, payload_read, 10 * 4096) == 0);

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_blob_close(snapshot, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_free_io_channel(channel);

	/* Unload the blob store */
	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
	g_blob = NULL;
	g_blobid = 0;
}

/**
 * Inflate / decouple parent rw unit tests.
 *
 * --------------
 * original blob:         0         1         2         3         4
 *                   ,---------+---------+---------+---------+---------.
 *         snapshot  |xxxxxxxxx|xxxxxxxxx|xxxxxxxxx|xxxxxxxxx|    -    |
 *                   +---------+---------+---------+---------+---------+
 *         snapshot2 |    -    |yyyyyyyyy|    -    |yyyyyyyyy|    -    |
 *                   +---------+---------+---------+---------+---------+
 *         blob      |    -    |zzzzzzzzz|    -    |    -    |    -    |
 *                   '---------+---------+---------+---------+---------'
 *                   .         .         .         .         .         .
 * --------          .         .         .         .         .         .
 * inflate:          .         .         .         .         .         .
 *                   ,---------+---------+---------+---------+---------.
 *         blob      |xxxxxxxxx|zzzzzzzzz|xxxxxxxxx|yyyyyyyyy|000000000|
 *                   '---------+---------+---------+---------+---------'
 *
 *         NOTE: needs to allocate 4 clusters, thin provisioning removed, dependency
 *               on snapshot2 and snapshot removed .         .         .
 *                   .         .         .         .         .         .
 * ----------------  .         .         .         .         .         .
 * decouple parent:  .         .         .         .         .         .
 *                   ,---------+---------+---------+---------+---------.
 *         snapshot  |xxxxxxxxx|xxxxxxxxx|xxxxxxxxx|xxxxxxxxx|    -    |
 *                   +---------+---------+---------+---------+---------+
 *         blob      |    -    |zzzzzzzzz|    -    |yyyyyyyyy|    -    |
 *                   '---------+---------+---------+---------+---------'
 *
 *         NOTE: needs to allocate 1 cluster, 3 clusters unallocated, dependency
 *               on snapshot2 removed and on snapshot still exists. Snapshot2
 *               should remain a clone of snapshot.
 */
static void
_blob_inflate_rw(bool decouple_parent)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_blob *blob, *snapshot, *snapshot2;
	struct spdk_io_channel *channel;
	struct spdk_blob_opts opts;
	spdk_blob_id blobid, snapshotid, snapshot2id;
	uint64_t free_clusters;
	uint64_t cluster_size;

	uint64_t payload_size;
	uint8_t *payload_read;
	uint8_t *payload_write;
	uint8_t *payload_clone;

	uint64_t pages_per_cluster;
	uint64_t pages_per_payload;

	int i;
	spdk_blob_id ids[2];
	size_t count;

	dev = init_dev();

	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	free_clusters = spdk_bs_free_cluster_count(bs);
	cluster_size = spdk_bs_get_cluster_size(bs);
	pages_per_cluster = cluster_size / spdk_bs_get_page_size(bs);
	pages_per_payload = pages_per_cluster * 5;

	payload_size = cluster_size * 5;

	payload_read = malloc(payload_size);
	SPDK_CU_ASSERT_FATAL(payload_read != NULL);

	payload_write = malloc(payload_size);
	SPDK_CU_ASSERT_FATAL(payload_write != NULL);

	payload_clone = malloc(payload_size);
	SPDK_CU_ASSERT_FATAL(payload_clone != NULL);

	channel = spdk_bs_alloc_io_channel(bs);
	SPDK_CU_ASSERT_FATAL(channel != NULL);

	/* Create blob */
	spdk_blob_opts_init(&opts);
	opts.thin_provision = true;
	opts.num_clusters = 5;

	spdk_bs_create_blob_ext(bs, &opts, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;

	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 5);

	/* 1) Initial read should return zeroed payload */
	memset(payload_read, 0xFF, payload_size);
	spdk_blob_io_read(blob, channel, payload_read, 0, pages_per_payload,
			  blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(spdk_mem_all_zero(payload_read, payload_size));

	/* Fill whole blob with a pattern, except last cluster (to be sure it
	 * isn't allocated) */
	memset(payload_write, 0xE5, payload_size - cluster_size);
	spdk_blob_io_write(blob, channel, payload_write, 0, pages_per_payload -
			   pages_per_cluster, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(free_clusters != spdk_bs_free_cluster_count(bs));

	/* 2) Create snapshot from blob (first level) */
	spdk_bs_create_snapshot(bs, blobid, NULL, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	snapshotid = g_blobid;

	spdk_bs_open_blob(bs, snapshotid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	snapshot = g_blob;
	CU_ASSERT(snapshot->data_ro == true)
	CU_ASSERT(snapshot->md_ro == true)

	CU_ASSERT(spdk_blob_get_num_clusters(snapshot) == 5)

	/* Write every second cluster with a pattern.
	 *
	 * Last cluster shouldn't be written, to be sure that snapshot nor clone
	 * doesn't allocate it.
	 *
	 * payload_clone stores expected result on "blob" read at the time and
	 * is used only to check data consistency on clone before and after
	 * inflation. Initially we fill it with a backing snapshots pattern
	 * used before.
	 */
	memset(payload_clone, 0xE5, payload_size - cluster_size);
	memset(payload_clone + payload_size - cluster_size, 0x00, cluster_size);
	memset(payload_write, 0xAA, payload_size);
	for (i = 1; i < 5; i += 2) {
		spdk_blob_io_write(blob, channel, payload_write, i * pages_per_cluster,
				   pages_per_cluster, blob_op_complete, NULL);
		CU_ASSERT(g_bserrno == 0);

		/* Update expected result */
		memcpy(payload_clone + (cluster_size * i), payload_write,
		       cluster_size);
	}
	CU_ASSERT(free_clusters != spdk_bs_free_cluster_count(bs));

	/* Check data consistency on clone */
	memset(payload_read, 0xFF, payload_size);
	spdk_blob_io_read(blob, channel, payload_read, 0, pages_per_payload,
			  blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_clone, payload_read, payload_size) == 0);

	/* 3) Create second levels snapshot from blob */
	spdk_bs_create_snapshot(bs, blobid, NULL, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	snapshot2id = g_blobid;

	spdk_bs_open_blob(bs, snapshot2id, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	snapshot2 = g_blob;
	CU_ASSERT(snapshot2->data_ro == true)
	CU_ASSERT(snapshot2->md_ro == true)

	CU_ASSERT(spdk_blob_get_num_clusters(snapshot2) == 5)

	CU_ASSERT(snapshot2->parent_id == snapshotid);

	/* Write one cluster on the top level blob. This cluster (1) covers
	 * already allocated cluster in the snapshot2, so shouldn't be inflated
	 * at all */
	spdk_blob_io_write(blob, channel, payload_write, pages_per_cluster,
			   pages_per_cluster, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* Update expected result */
	memcpy(payload_clone + cluster_size, payload_write, cluster_size);

	/* Check data consistency on clone */
	memset(payload_read, 0xFF, payload_size);
	spdk_blob_io_read(blob, channel, payload_read, 0, pages_per_payload,
			  blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_clone, payload_read, payload_size) == 0);


	/* Close all blobs */
	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_blob_close(snapshot2, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_blob_close(snapshot, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* Check snapshot-clone relations */
	count = 2;
	CU_ASSERT(spdk_blob_get_clones(bs, snapshotid, ids, &count) == 0);
	CU_ASSERT(count == 1);
	CU_ASSERT(ids[0] == snapshot2id);

	count = 2;
	CU_ASSERT(spdk_blob_get_clones(bs, snapshot2id, ids, &count) == 0);
	CU_ASSERT(count == 1);
	CU_ASSERT(ids[0] == blobid);

	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, blobid) == snapshot2id);

	free_clusters = spdk_bs_free_cluster_count(bs);
	if (!decouple_parent) {
		/* Do full blob inflation */
		spdk_bs_inflate_blob(bs, channel, blobid, blob_op_complete, NULL);
		CU_ASSERT(g_bserrno == 0);

		/* All clusters should be inflated (except one already allocated
		 * in a top level blob) */
		CU_ASSERT(spdk_bs_free_cluster_count(bs) == free_clusters - 4);

		/* Check if relation tree updated correctly */
		count = 2;
		CU_ASSERT(spdk_blob_get_clones(bs, snapshotid, ids, &count) == 0);

		/* snapshotid have one clone */
		CU_ASSERT(count == 1);
		CU_ASSERT(ids[0] == snapshot2id);

		/* snapshot2id have no clones */
		count = 2;
		CU_ASSERT(spdk_blob_get_clones(bs, snapshot2id, ids, &count) == 0);
		CU_ASSERT(count == 0);

		CU_ASSERT(spdk_blob_get_parent_snapshot(bs, blobid) == SPDK_BLOBID_INVALID);
	} else {
		/* Decouple parent of blob */
		spdk_bs_blob_decouple_parent(bs, channel, blobid, blob_op_complete, NULL);
		CU_ASSERT(g_bserrno == 0);

		/* Only one cluster from a parent should be inflated (second one
		 * is covered by a cluster written on a top level blob, and
		 * already allocated) */
		CU_ASSERT(spdk_bs_free_cluster_count(bs) == free_clusters - 1);

		/* Check if relation tree updated correctly */
		count = 2;
		CU_ASSERT(spdk_blob_get_clones(bs, snapshotid, ids, &count) == 0);

		/* snapshotid have two clones now */
		CU_ASSERT(count == 2);
		CU_ASSERT(ids[0] == blobid || ids[1] == blobid);
		CU_ASSERT(ids[0] == snapshot2id || ids[1] == snapshot2id);

		/* snapshot2id have no clones */
		count = 2;
		CU_ASSERT(spdk_blob_get_clones(bs, snapshot2id, ids, &count) == 0);
		CU_ASSERT(count == 0);

		CU_ASSERT(spdk_blob_get_parent_snapshot(bs, blobid) == snapshotid);
	}

	/* Try to delete snapshot2 (should pass) */
	spdk_bs_delete_blob(bs, snapshot2id, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* Try to delete base snapshot (for decouple_parent should fail while
	 * dependency still exists) */
	spdk_bs_delete_blob(bs, snapshotid, blob_op_complete, NULL);
	CU_ASSERT(decouple_parent || g_bserrno == 0);
	CU_ASSERT(!decouple_parent || g_bserrno != 0);

	/* Reopen blob after snapshot deletion */
	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;

	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 5);

	/* Check data consistency on inflated blob */
	memset(payload_read, 0xFF, payload_size);
	spdk_blob_io_read(blob, channel, payload_read, 0, pages_per_payload,
			  blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_clone, payload_read, payload_size) == 0);

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_free_io_channel(channel);

	/* Unload the blob store */
	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
	g_blob = NULL;
	g_blobid = 0;

	free(payload_read);
	free(payload_write);
	free(payload_clone);
}

static void
blob_inflate_rw(void)
{
	_blob_inflate_rw(false);
	_blob_inflate_rw(true);
}

/**
 * Snapshot-clones relation test
 *
 *         snapshot
 *            |
 *      +-----+-----+
 *      |           |
 *   blob(ro)   snapshot2
 *      |           |
 *   clone2      clone
 */
static void
blob_relations(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_bs_opts bs_opts;
	struct spdk_blob_opts opts;
	struct spdk_blob *blob, *snapshot, *snapshot2, *clone, *clone2;
	spdk_blob_id blobid, cloneid, snapshotid, cloneid2, snapshotid2;
	int rc;
	size_t count;
	spdk_blob_id ids[10] = {};

	dev = init_dev();
	spdk_bs_opts_init(&bs_opts);
	snprintf(bs_opts.bstype.bstype, sizeof(bs_opts.bstype.bstype), "TESTTYPE");

	spdk_bs_init(dev, &bs_opts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	/* 1. Create blob with 10 clusters */

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

	CU_ASSERT(!spdk_blob_is_read_only(blob));
	CU_ASSERT(!spdk_blob_is_snapshot(blob));
	CU_ASSERT(!spdk_blob_is_clone(blob));
	CU_ASSERT(!spdk_blob_is_thin_provisioned(blob));

	/* blob should not have underlying snapshot nor clones */
	CU_ASSERT(blob->parent_id == SPDK_BLOBID_INVALID);
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, blobid) == SPDK_BLOBID_INVALID);
	count = SPDK_COUNTOF(ids);
	rc = spdk_blob_get_clones(bs, blobid, ids, &count);
	CU_ASSERT(rc == 0);
	CU_ASSERT(count == 0);


	/* 2. Create snapshot */

	spdk_bs_create_snapshot(bs, blobid, NULL, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	snapshotid = g_blobid;

	spdk_bs_open_blob(bs, snapshotid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	snapshot = g_blob;

	CU_ASSERT(spdk_blob_is_read_only(snapshot));
	CU_ASSERT(spdk_blob_is_snapshot(snapshot));
	CU_ASSERT(!spdk_blob_is_clone(snapshot));
	CU_ASSERT(snapshot->parent_id == SPDK_BLOBID_INVALID);
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, snapshotid) == SPDK_BLOBID_INVALID);

	/* Check if original blob is converted to the clone of snapshot */
	CU_ASSERT(!spdk_blob_is_read_only(blob));
	CU_ASSERT(!spdk_blob_is_snapshot(blob));
	CU_ASSERT(spdk_blob_is_clone(blob));
	CU_ASSERT(spdk_blob_is_thin_provisioned(blob));
	CU_ASSERT(blob->parent_id == snapshotid);
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, blobid) == snapshotid);

	count = SPDK_COUNTOF(ids);
	rc = spdk_blob_get_clones(bs, snapshotid, ids, &count);
	CU_ASSERT(rc == 0);
	CU_ASSERT(count == 1);
	CU_ASSERT(ids[0] == blobid);


	/* 3. Create clone from snapshot */

	spdk_bs_create_clone(bs, snapshotid, NULL, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	cloneid = g_blobid;

	spdk_bs_open_blob(bs, cloneid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	clone = g_blob;

	CU_ASSERT(!spdk_blob_is_read_only(clone));
	CU_ASSERT(!spdk_blob_is_snapshot(clone));
	CU_ASSERT(spdk_blob_is_clone(clone));
	CU_ASSERT(spdk_blob_is_thin_provisioned(clone));
	CU_ASSERT(clone->parent_id == snapshotid);
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, cloneid) == snapshotid);

	count = SPDK_COUNTOF(ids);
	rc = spdk_blob_get_clones(bs, cloneid, ids, &count);
	CU_ASSERT(rc == 0);
	CU_ASSERT(count == 0);

	/* Check if clone is on the snapshot's list */
	count = SPDK_COUNTOF(ids);
	rc = spdk_blob_get_clones(bs, snapshotid, ids, &count);
	CU_ASSERT(rc == 0);
	CU_ASSERT(ids[0] == blobid || ids[1] == blobid);
	CU_ASSERT(ids[0] == cloneid || ids[1] == cloneid);


	/* 4. Create snapshot of the clone */

	spdk_bs_create_snapshot(bs, cloneid, NULL, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	snapshotid2 = g_blobid;

	spdk_bs_open_blob(bs, snapshotid2, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	snapshot2 = g_blob;

	CU_ASSERT(spdk_blob_is_read_only(snapshot2));
	CU_ASSERT(spdk_blob_is_snapshot(snapshot2));
	CU_ASSERT(spdk_blob_is_clone(snapshot2));
	CU_ASSERT(snapshot2->parent_id == snapshotid);
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, snapshotid2) == snapshotid);

	/* Check if clone is converted to the clone of snapshot2 and snapshot2
	 * is a child of snapshot */
	CU_ASSERT(!spdk_blob_is_read_only(clone));
	CU_ASSERT(!spdk_blob_is_snapshot(clone));
	CU_ASSERT(spdk_blob_is_clone(clone));
	CU_ASSERT(spdk_blob_is_thin_provisioned(clone));
	CU_ASSERT(clone->parent_id == snapshotid2);
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, cloneid) == snapshotid2);

	count = SPDK_COUNTOF(ids);
	rc = spdk_blob_get_clones(bs, snapshotid2, ids, &count);
	CU_ASSERT(rc == 0);
	CU_ASSERT(count == 1);
	CU_ASSERT(ids[0] == cloneid);


	/* 5. Try to create clone from read only blob */

	/* Mark blob as read only */
	spdk_blob_set_read_only(blob);
	spdk_blob_sync_md(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* Check if previously created blob is read only clone */
	CU_ASSERT(spdk_blob_is_read_only(blob));
	CU_ASSERT(!spdk_blob_is_snapshot(blob));
	CU_ASSERT(spdk_blob_is_clone(blob));
	CU_ASSERT(spdk_blob_is_thin_provisioned(blob));

	/* Create clone from read only blob */
	spdk_bs_create_clone(bs, blobid, NULL, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	cloneid2 = g_blobid;

	spdk_bs_open_blob(bs, cloneid2, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	clone2 = g_blob;

	CU_ASSERT(!spdk_blob_is_read_only(clone2));
	CU_ASSERT(!spdk_blob_is_snapshot(clone2));
	CU_ASSERT(spdk_blob_is_clone(clone2));
	CU_ASSERT(spdk_blob_is_thin_provisioned(clone2));

	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, cloneid2) == blobid);

	count = SPDK_COUNTOF(ids);
	rc = spdk_blob_get_clones(bs, blobid, ids, &count);
	CU_ASSERT(rc == 0);

	CU_ASSERT(count == 1);
	CU_ASSERT(ids[0] == cloneid2);

	/* Close blobs */

	spdk_blob_close(clone2, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_blob_close(clone, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_blob_close(snapshot, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_blob_close(snapshot2, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* Try to delete snapshot with created clones */
	spdk_bs_delete_blob(bs, snapshotid, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno != 0);

	spdk_bs_delete_blob(bs, snapshotid2, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno != 0);

	spdk_bs_unload(bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;

	/* Load an existing blob store */
	dev = init_dev();
	snprintf(bs_opts.bstype.bstype, sizeof(bs_opts.bstype.bstype), "TESTTYPE");

	spdk_bs_load(dev, NULL, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;


	/* NULL ids array should return number of clones in count */
	count = SPDK_COUNTOF(ids);
	rc = spdk_blob_get_clones(bs, snapshotid, NULL, &count);
	CU_ASSERT(rc == -ENOMEM);
	CU_ASSERT(count == 2);

	/* incorrect array size */
	count = 1;
	rc = spdk_blob_get_clones(bs, snapshotid, ids, &count);
	CU_ASSERT(rc == -ENOMEM);
	CU_ASSERT(count == 2);


	/* Verify structure of loaded blob store */

	/* snapshot */
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, snapshotid) == SPDK_BLOBID_INVALID);

	count = SPDK_COUNTOF(ids);
	rc = spdk_blob_get_clones(bs, snapshotid, ids, &count);
	CU_ASSERT(rc == 0);
	CU_ASSERT(count == 2);
	CU_ASSERT(ids[0] == blobid || ids[1] == blobid);
	CU_ASSERT(ids[0] == snapshotid2 || ids[1] == snapshotid2);

	/* blob */
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, blobid) == snapshotid);
	count = SPDK_COUNTOF(ids);
	rc = spdk_blob_get_clones(bs, blobid, ids, &count);
	CU_ASSERT(rc == 0);
	CU_ASSERT(count == 1);
	CU_ASSERT(ids[0] == cloneid2);

	/* clone */
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, cloneid) == snapshotid2);
	count = SPDK_COUNTOF(ids);
	rc = spdk_blob_get_clones(bs, cloneid, ids, &count);
	CU_ASSERT(rc == 0);
	CU_ASSERT(count == 0);

	/* snapshot2 */
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, snapshotid2) == snapshotid);
	count = SPDK_COUNTOF(ids);
	rc = spdk_blob_get_clones(bs, snapshotid2, ids, &count);
	CU_ASSERT(rc == 0);
	CU_ASSERT(count == 1);
	CU_ASSERT(ids[0] == cloneid);

	/* clone2 */
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, cloneid2) == blobid);
	count = SPDK_COUNTOF(ids);
	rc = spdk_blob_get_clones(bs, cloneid2, ids, &count);
	CU_ASSERT(rc == 0);
	CU_ASSERT(count == 0);

	/* Try to delete all blobs in the worse possible order */

	spdk_bs_delete_blob(bs, snapshotid, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno != 0);

	spdk_bs_delete_blob(bs, snapshotid2, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno != 0);

	spdk_bs_delete_blob(bs, cloneid, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_delete_blob(bs, snapshotid2, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_delete_blob(bs, snapshotid, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno != 0);

	spdk_bs_delete_blob(bs, blobid, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno != 0);

	spdk_bs_delete_blob(bs, cloneid2, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_delete_blob(bs, blobid, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_delete_blob(bs, snapshotid, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_unload(bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	g_bs = NULL;
}

static void
test_io_write(struct spdk_bs_dev *dev, struct spdk_blob *blob, struct spdk_io_channel *channel)
{
	uint8_t payload_ff[64 * 512];
	uint8_t payload_aa[64 * 512];
	uint8_t payload_00[64 * 512];
	uint8_t *cluster0, *cluster1;

	memset(payload_ff, 0xFF, sizeof(payload_ff));
	memset(payload_aa, 0xAA, sizeof(payload_aa));
	memset(payload_00, 0x00, sizeof(payload_00));

	/* Try to perform I/O with io unit = 512 */
	spdk_blob_io_write(blob, channel, payload_ff, 0, 1, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* If thin provisioned is set cluster should be allocated now */
	SPDK_CU_ASSERT_FATAL(blob->active.clusters[0] != 0);
	cluster0 = &g_dev_buffer[blob->active.clusters[0] * dev->blocklen];

	/* Each character 0-F symbolizes single io_unit containing 512 bytes block filled with that character.
	* Each page is separated by |. Whole block [...] symbolizes one cluster (containing 4 pages). */
	/* cluster0: [ F000 0000 | 0000 0000 | 0000 0000 | 0000 0000 ] */
	CU_ASSERT(memcmp(cluster0 + 0 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 1 * 512, payload_00, 31 * 512) == 0);

	/* Verify write with offset on first page */
	spdk_blob_io_write(blob, channel, payload_ff, 2, 1, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* cluster0: [ F0F0 0000 | 0000 0000 | 0000 0000 | 0000 0000 ] */
	CU_ASSERT(memcmp(cluster0 + 0 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 1 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 2 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 3 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 4 * 512, payload_00, 28 * 512) == 0);

	/* Verify write with offset on first page */
	spdk_blob_io_write(blob, channel, payload_ff, 4, 4, blob_op_complete, NULL);

	/* cluster0: [ F0F0 FFFF | 0000 0000 | 0000 0000 | 0000 0000 ] */
	CU_ASSERT(memcmp(cluster0 + 0 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 1 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 2 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 3 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 4 * 512, payload_ff, 4 * 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 8 * 512, payload_00, 24 * 512) == 0);

	/* Verify write with offset on second page */
	spdk_blob_io_write(blob, channel, payload_ff, 8, 4, blob_op_complete, NULL);

	/* cluster0: [ F0F0 FFFF | FFFF 0000 | 0000 0000 | 0000 0000 ] */
	CU_ASSERT(memcmp(cluster0 + 0 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 1 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 2 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 3 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 4 * 512, payload_ff, 8 * 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 12 * 512, payload_00, 20 * 512) == 0);

	/* Verify write across multiple pages */
	spdk_blob_io_write(blob, channel, payload_aa, 4, 8, blob_op_complete, NULL);

	/* cluster0: [ F0F0 AAAA | AAAA 0000 | 0000 0000 | 0000 0000 ] */
	CU_ASSERT(memcmp(cluster0 + 0 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 1 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 2 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 3 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 4 * 512, payload_aa, 8 * 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 12 * 512, payload_00, 20 * 512) == 0);

	/* Verify write across multiple clusters */
	spdk_blob_io_write(blob, channel, payload_ff, 28, 8, blob_op_complete, NULL);

	SPDK_CU_ASSERT_FATAL(blob->active.clusters[1] != 0);
	cluster1 = &g_dev_buffer[blob->active.clusters[1] * dev->blocklen];

	/* cluster0: [ F0F0 AAAA | AAAA 0000 | 0000 0000 | 0000 FFFF ]
	 * cluster1: [ FFFF 0000 | 0000 0000 | 0000 0000 | 0000 0000 ] */
	CU_ASSERT(memcmp(cluster0 + 0 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 1 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 2 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 3 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 4 * 512, payload_aa, 8 * 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 28 * 512, payload_ff, 4 * 512) == 0);

	CU_ASSERT(memcmp(cluster1 + 0 * 512, payload_ff, 4 * 512) == 0);
	CU_ASSERT(memcmp(cluster1 + 4 * 512, payload_00, 28 * 512) == 0);

	/* Verify write to second cluster */
	spdk_blob_io_write(blob, channel, payload_ff, 32 + 12, 2, blob_op_complete, NULL);

	SPDK_CU_ASSERT_FATAL(blob->active.clusters[1] != 0);
	cluster1 = &g_dev_buffer[blob->active.clusters[1] * dev->blocklen];

	/* cluster0: [ F0F0 AAAA | AAAA 0000 | 0000 0000 | 0000 FFFF ]
	 * cluster1: [ FFFF 0000 | 0000 FF00 | 0000 0000 | 0000 0000 ] */
	CU_ASSERT(memcmp(cluster0 + 0 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 1 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 2 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 3 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 4 * 512, payload_aa, 8 * 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 28 * 512, payload_ff, 4 * 512) == 0);

	CU_ASSERT(memcmp(cluster1 + 0 * 512, payload_ff, 4 * 512) == 0);
	CU_ASSERT(memcmp(cluster1 + 4 * 512, payload_00, 8 * 512) == 0);
	CU_ASSERT(memcmp(cluster1 + 12 * 512, payload_ff, 2 * 512) == 0);
	CU_ASSERT(memcmp(cluster1 + 14 * 512, payload_00, 18 * 512) == 0);
}

static void
test_io_read(struct spdk_bs_dev *dev, struct spdk_blob *blob, struct spdk_io_channel *channel)
{
	uint8_t payload_read[64 * 512];
	uint8_t payload_ff[64 * 512];
	uint8_t payload_aa[64 * 512];
	uint8_t payload_00[64 * 512];

	memset(payload_ff, 0xFF, sizeof(payload_ff));
	memset(payload_aa, 0xAA, sizeof(payload_aa));
	memset(payload_00, 0x00, sizeof(payload_00));

	/* Read only first io unit */
	/* cluster0: [ (F)0F0 AAAA | AAAA 0000 | 0000 0000 | 0000 FFFF ]
	 * cluster1: [ FFFF 0000 | 0000 FF00 | 0000 0000 | 0000 0000 ]
	 * payload_read: F000 0000 | 0000 0000 ... */
	memset(payload_read, 0x00, sizeof(payload_read));
	spdk_blob_io_read(blob, channel, payload_read, 0, 1, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_read + 0 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(payload_read + 1 * 512, payload_00, 31 * 512) == 0);

	/* Read four io_units starting from offset = 2
	 * cluster0: [ F0(F0 AA)AA | AAAA 0000 | 0000 0000 | 0000 FFFF ]
	 * cluster1: [ FFFF 0000 | 0000 FF00 | 0000 0000 | 0000 0000 ]
	 * payload_read: F0AA 0000 | 0000 0000 ... */

	memset(payload_read, 0x00, sizeof(payload_read));
	spdk_blob_io_read(blob, channel, payload_read, 2, 4, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	CU_ASSERT(memcmp(payload_read + 0 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(payload_read + 1 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(payload_read + 2 * 512, payload_aa, 512) == 0);
	CU_ASSERT(memcmp(payload_read + 3 * 512, payload_aa, 512) == 0);
	CU_ASSERT(memcmp(payload_read + 4 * 512, payload_00, 28 * 512) == 0);

	/* Read eight io_units across multiple pages
	 * cluster0: [ F0F0 (AAAA | AAAA) 0000 | 0000 0000 | 0000 FFFF ]
	 * cluster1: [ FFFF 0000 | 0000 FF00 | 0000 0000 | 0000 0000 ]
	 * payload_read: AAAA AAAA | 0000 0000 ... */
	memset(payload_read, 0x00, sizeof(payload_read));
	spdk_blob_io_read(blob, channel, payload_read, 4, 8, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	CU_ASSERT(memcmp(payload_read + 0 * 512, payload_aa, 8 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + 8 * 512, payload_00, 24 * 512) == 0);

	/* Read eight io_units across multiple clusters
	 * cluster0: [ F0F0 AAAA | AAAA 0000 | 0000 0000 | 0000 (FFFF ]
	 * cluster1: [ FFFF) 0000 | 0000 FF00 | 0000 0000 | 0000 0000 ]
	 * payload_read: FFFF FFFF | 0000 0000 ... */
	memset(payload_read, 0x00, sizeof(payload_read));
	spdk_blob_io_read(blob, channel, payload_read, 28, 8, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	CU_ASSERT(memcmp(payload_read + 0 * 512, payload_ff, 8 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + 8 * 512, payload_00, 24 * 512) == 0);

	/* Read four io_units from second cluster
	 * cluster0: [ F0F0 AAAA | AAAA 0000 | 0000 0000 | 0000 FFFF ]
	 * cluster1: [ FFFF 0000 | 00(00 FF)00 | 0000 0000 | 0000 0000 ]
	 * payload_read: 00FF 0000 | 0000 0000 ... */
	memset(payload_read, 0x00, sizeof(payload_read));
	spdk_blob_io_read(blob, channel, payload_read, 32 + 10, 4, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	CU_ASSERT(memcmp(payload_read + 0 * 512, payload_00, 2 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + 2 * 512, payload_ff, 2 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + 4 * 512, payload_00, 28 * 512) == 0);

	/* Read second cluster
	 * cluster0: [ F0F0 AAAA | AAAA 0000 | 0000 0000 | 0000 FFFF ]
	 * cluster1: [ (FFFF 0000 | 0000 FF00 | 0000 0000 | 0000 0000) ]
	 * payload_read: FFFF 0000 | 0000 FF00 ... */
	memset(payload_read, 0x00, sizeof(payload_read));
	spdk_blob_io_read(blob, channel, payload_read, 32, 32, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_read + 0 * 512, payload_ff, 4 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + 4 * 512, payload_00, 8 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + 12 * 512, payload_ff, 2 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + 14 * 512, payload_00, 18 * 512) == 0);

	/* Read whole two clusters
	 * cluster0: [ (F0F0 AAAA | AAAA) 0000 | 0000 0000 | 0000 FFFF ]
	 * cluster1: [ FFFF 0000 | 0000 FF00 | 0000 0000 | 0000 0000) ] */
	memset(payload_read, 0x00, sizeof(payload_read));
	spdk_blob_io_read(blob, channel, payload_read, 0, 64, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	CU_ASSERT(memcmp(payload_read + 0 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(payload_read + 1 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(payload_read + 2 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(payload_read + 3 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(payload_read + 4 * 512, payload_aa, 8 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + 28 * 512, payload_ff, 4 * 512) == 0);

	CU_ASSERT(memcmp(payload_read + (32 + 0) * 512, payload_ff, 4 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + (32 + 4) * 512, payload_00, 8 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + (32 + 12) * 512, payload_ff, 2 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + (32 + 14) * 512, payload_00, 18 * 512) == 0);
}


static void
test_io_unmap(struct spdk_bs_dev *dev, struct spdk_blob *blob, struct spdk_io_channel *channel)
{
	uint8_t payload_ff[64 * 512];
	uint8_t payload_aa[64 * 512];
	uint8_t payload_00[64 * 512];
	uint8_t *cluster0, *cluster1;

	memset(payload_ff, 0xFF, sizeof(payload_ff));
	memset(payload_aa, 0xAA, sizeof(payload_aa));
	memset(payload_00, 0x00, sizeof(payload_00));

	cluster0 = &g_dev_buffer[blob->active.clusters[0] * dev->blocklen];
	cluster1 = &g_dev_buffer[blob->active.clusters[1] * dev->blocklen];

	/* Unmap */
	spdk_blob_io_unmap(blob, channel, 0, 64, blob_op_complete, NULL);

	CU_ASSERT(g_bserrno == 0);

	CU_ASSERT(memcmp(cluster0 + 0 * 512, payload_00, 32 * 512) == 0);
	CU_ASSERT(memcmp(cluster1 + 0 * 512, payload_00, 32 * 512) == 0);
}

static void
test_io_zeroes(struct spdk_bs_dev *dev, struct spdk_blob *blob, struct spdk_io_channel *channel)
{
	uint8_t payload_ff[64 * 512];
	uint8_t payload_aa[64 * 512];
	uint8_t payload_00[64 * 512];
	uint8_t *cluster0, *cluster1;

	memset(payload_ff, 0xFF, sizeof(payload_ff));
	memset(payload_aa, 0xAA, sizeof(payload_aa));
	memset(payload_00, 0x00, sizeof(payload_00));

	cluster0 = &g_dev_buffer[blob->active.clusters[0] * dev->blocklen];
	cluster1 = &g_dev_buffer[blob->active.clusters[1] * dev->blocklen];

	/* Write zeroes  */
	spdk_blob_io_write_zeroes(blob, channel, 0, 64, blob_op_complete, NULL);

	CU_ASSERT(g_bserrno == 0);

	CU_ASSERT(memcmp(cluster0 + 0 * 512, payload_00, 32 * 512) == 0);
	CU_ASSERT(memcmp(cluster1 + 0 * 512, payload_00, 32 * 512) == 0);
}


static void
test_iov_write(struct spdk_bs_dev *dev, struct spdk_blob *blob, struct spdk_io_channel *channel)
{
	uint8_t payload_ff[64 * 512];
	uint8_t payload_aa[64 * 512];
	uint8_t payload_00[64 * 512];
	uint8_t *cluster0, *cluster1;
	struct iovec iov[4];

	memset(payload_ff, 0xFF, sizeof(payload_ff));
	memset(payload_aa, 0xAA, sizeof(payload_aa));
	memset(payload_00, 0x00, sizeof(payload_00));

	/* Try to perform I/O with io unit = 512 */
	iov[0].iov_base = payload_ff;
	iov[0].iov_len = 1 * 512;
	spdk_blob_io_writev(blob, channel, iov, 1, 0, 1, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* If thin provisioned is set cluster should be allocated now */
	SPDK_CU_ASSERT_FATAL(blob->active.clusters[0] != 0);
	cluster0 = &g_dev_buffer[blob->active.clusters[0] * dev->blocklen];

	/* Each character 0-F symbolizes single io_unit containing 512 bytes block filled with that character.
	* Each page is separated by |. Whole block [...] symbolizes one cluster (containing 4 pages). */
	/* cluster0: [ F000 0000 | 0000 0000 | 0000 0000 | 0000 0000 ] */
	CU_ASSERT(memcmp(cluster0 + 0 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 1 * 512, payload_00, 31 * 512) == 0);

	/* Verify write with offset on first page */
	iov[0].iov_base = payload_ff;
	iov[0].iov_len = 1 * 512;
	spdk_blob_io_writev(blob, channel, iov, 1, 2, 1, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* cluster0: [ F0F0 0000 | 0000 0000 | 0000 0000 | 0000 0000 ] */
	CU_ASSERT(memcmp(cluster0 + 0 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 1 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 2 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 3 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 4 * 512, payload_00, 28 * 512) == 0);

	/* Verify write with offset on first page */
	iov[0].iov_base = payload_ff;
	iov[0].iov_len = 4 * 512;
	spdk_blob_io_writev(blob, channel, iov, 1, 4, 4, blob_op_complete, NULL);

	/* cluster0: [ F0F0 FFFF | 0000 0000 | 0000 0000 | 0000 0000 ] */
	CU_ASSERT(memcmp(cluster0 + 0 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 1 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 2 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 3 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 4 * 512, payload_ff, 4 * 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 8 * 512, payload_00, 24 * 512) == 0);

	/* Verify write with offset on second page */
	iov[0].iov_base = payload_ff;
	iov[0].iov_len = 4 * 512;
	spdk_blob_io_writev(blob, channel, iov, 1, 8, 4, blob_op_complete, NULL);

	/* cluster0: [ F0F0 FFFF | FFFF 0000 | 0000 0000 | 0000 0000 ] */
	CU_ASSERT(memcmp(cluster0 + 0 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 1 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 2 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 3 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 4 * 512, payload_ff, 8 * 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 12 * 512, payload_00, 20 * 512) == 0);

	/* Verify write across multiple pages */
	iov[0].iov_base = payload_aa;
	iov[0].iov_len = 8 * 512;
	spdk_blob_io_writev(blob, channel, iov, 1, 4, 8, blob_op_complete, NULL);

	/* cluster0: [ F0F0 AAAA | AAAA 0000 | 0000 0000 | 0000 0000 ] */
	CU_ASSERT(memcmp(cluster0 + 0 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 1 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 2 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 3 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 4 * 512, payload_aa, 8 * 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 12 * 512, payload_00, 20 * 512) == 0);

	/* Verify write across multiple clusters */

	iov[0].iov_base = payload_ff;
	iov[0].iov_len = 8 * 512;
	spdk_blob_io_writev(blob, channel, iov, 1, 28, 8, blob_op_complete, NULL);

	SPDK_CU_ASSERT_FATAL(blob->active.clusters[1] != 0);
	cluster1 = &g_dev_buffer[blob->active.clusters[1] * dev->blocklen];

	/* cluster0: [ F0F0 AAAA | AAAA 0000 | 0000 0000 | 0000 FFFF ]
	 * cluster1: [ FFFF 0000 | 0000 0000 | 0000 0000 | 0000 0000 ] */
	CU_ASSERT(memcmp(cluster0 + 0 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 1 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 2 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 3 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 4 * 512, payload_aa, 8 * 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 12 * 512, payload_00, 16 * 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 28 * 512, payload_ff, 4 * 512) == 0);

	CU_ASSERT(memcmp(cluster1 + 0 * 512, payload_ff, 4 * 512) == 0);
	CU_ASSERT(memcmp(cluster1 + 4 * 512, payload_00, 28 * 512) == 0);

	/* Verify write to second cluster */

	iov[0].iov_base = payload_ff;
	iov[0].iov_len = 2 * 512;
	spdk_blob_io_writev(blob, channel, iov, 1, 32 + 12, 2, blob_op_complete, NULL);

	SPDK_CU_ASSERT_FATAL(blob->active.clusters[1] != 0);
	cluster1 = &g_dev_buffer[blob->active.clusters[1] * dev->blocklen];

	/* cluster0: [ F0F0 AAAA | AAAA 0000 | 0000 0000 | 0000 FFFF ]
	 * cluster1: [ FFFF 0000 | 0000 FF00 | 0000 0000 | 0000 0000 ] */
	CU_ASSERT(memcmp(cluster0 + 0 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 1 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 2 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 3 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 4 * 512, payload_aa, 8 * 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 28 * 512, payload_ff, 4 * 512) == 0);

	CU_ASSERT(memcmp(cluster1 + 0 * 512, payload_ff, 4 * 512) == 0);
	CU_ASSERT(memcmp(cluster1 + 4 * 512, payload_00, 8 * 512) == 0);
	CU_ASSERT(memcmp(cluster1 + 12 * 512, payload_ff, 2 * 512) == 0);
	CU_ASSERT(memcmp(cluster1 + 14 * 512, payload_00, 18 * 512) == 0);
}

static void
test_iov_read(struct spdk_bs_dev *dev, struct spdk_blob *blob, struct spdk_io_channel *channel)
{
	uint8_t payload_read[64 * 512];
	uint8_t payload_ff[64 * 512];
	uint8_t payload_aa[64 * 512];
	uint8_t payload_00[64 * 512];
	struct iovec iov[4];

	memset(payload_ff, 0xFF, sizeof(payload_ff));
	memset(payload_aa, 0xAA, sizeof(payload_aa));
	memset(payload_00, 0x00, sizeof(payload_00));

	/* Read only first io unit */
	/* cluster0: [ (F)0F0 AAAA | AAAA 0000 | 0000 0000 | 0000 FFFF ]
	 * cluster1: [ FFFF 0000 | 0000 FF00 | 0000 0000 | 0000 0000 ]
	 * payload_read: F000 0000 | 0000 0000 ... */
	memset(payload_read, 0x00, sizeof(payload_read));
	iov[0].iov_base = payload_read;
	iov[0].iov_len = 1 * 512;
	spdk_blob_io_readv(blob, channel, iov, 1, 0, 1, blob_op_complete, NULL);

	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_read + 0 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(payload_read + 1 * 512, payload_00, 31 * 512) == 0);

	/* Read four io_units starting from offset = 2
	 * cluster0: [ F0(F0 AA)AA | AAAA 0000 | 0000 0000 | 0000 FFFF ]
	 * cluster1: [ FFFF 0000 | 0000 FF00 | 0000 0000 | 0000 0000 ]
	 * payload_read: F0AA 0000 | 0000 0000 ... */

	memset(payload_read, 0x00, sizeof(payload_read));
	iov[0].iov_base = payload_read;
	iov[0].iov_len = 4 * 512;
	spdk_blob_io_readv(blob, channel, iov, 1, 2, 4, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	CU_ASSERT(memcmp(payload_read + 0 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(payload_read + 1 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(payload_read + 2 * 512, payload_aa, 512) == 0);
	CU_ASSERT(memcmp(payload_read + 3 * 512, payload_aa, 512) == 0);
	CU_ASSERT(memcmp(payload_read + 4 * 512, payload_00, 28 * 512) == 0);

	/* Read eight io_units across multiple pages
	 * cluster0: [ F0F0 (AAAA | AAAA) 0000 | 0000 0000 | 0000 FFFF ]
	 * cluster1: [ FFFF 0000 | 0000 FF00 | 0000 0000 | 0000 0000 ]
	 * payload_read: AAAA AAAA | 0000 0000 ... */
	memset(payload_read, 0x00, sizeof(payload_read));
	iov[0].iov_base = payload_read;
	iov[0].iov_len = 4 * 512;
	iov[1].iov_base = payload_read + 4 * 512;
	iov[1].iov_len = 4 * 512;
	spdk_blob_io_readv(blob, channel, iov, 2, 4, 8, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	CU_ASSERT(memcmp(payload_read + 0 * 512, payload_aa, 8 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + 8 * 512, payload_00, 24 * 512) == 0);

	/* Read eight io_units across multiple clusters
	 * cluster0: [ F0F0 AAAA | AAAA 0000 | 0000 0000 | 0000 (FFFF ]
	 * cluster1: [ FFFF) 0000 | 0000 FF00 | 0000 0000 | 0000 0000 ]
	 * payload_read: FFFF FFFF | 0000 0000 ... */
	memset(payload_read, 0x00, sizeof(payload_read));
	iov[0].iov_base = payload_read;
	iov[0].iov_len = 2 * 512;
	iov[1].iov_base = payload_read + 2 * 512;
	iov[1].iov_len = 2 * 512;
	iov[2].iov_base = payload_read + 4 * 512;
	iov[2].iov_len = 2 * 512;
	iov[3].iov_base = payload_read + 6 * 512;
	iov[3].iov_len = 2 * 512;
	spdk_blob_io_readv(blob, channel, iov, 4, 28, 8, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	CU_ASSERT(memcmp(payload_read + 0 * 512, payload_ff, 8 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + 8 * 512, payload_00, 24 * 512) == 0);

	/* Read four io_units from second cluster
	 * cluster0: [ F0F0 AAAA | AAAA 0000 | 0000 0000 | 0000 FFFF ]
	 * cluster1: [ FFFF 0000 | 00(00 FF)00 | 0000 0000 | 0000 0000 ]
	 * payload_read: 00FF 0000 | 0000 0000 ... */
	memset(payload_read, 0x00, sizeof(payload_read));
	iov[0].iov_base = payload_read;
	iov[0].iov_len = 1 * 512;
	iov[1].iov_base = payload_read + 1 * 512;
	iov[1].iov_len = 3 * 512;
	spdk_blob_io_readv(blob, channel, iov, 2, 32 + 10, 4, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	CU_ASSERT(memcmp(payload_read + 0 * 512, payload_00, 2 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + 2 * 512, payload_ff, 2 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + 4 * 512, payload_00, 28 * 512) == 0);

	/* Read second cluster
	 * cluster0: [ F0F0 AAAA | AAAA 0000 | 0000 0000 | 0000 FFFF ]
	 * cluster1: [ (FFFF 0000 | 0000 FF00 | 0000 0000 | 0000 0000) ]
	 * payload_read: FFFF 0000 | 0000 FF00 ... */
	memset(payload_read, 0x00, sizeof(payload_read));
	iov[0].iov_base = payload_read;
	iov[0].iov_len = 1 * 512;
	iov[1].iov_base = payload_read + 1 * 512;
	iov[1].iov_len = 2 * 512;
	iov[2].iov_base = payload_read + 3 * 512;
	iov[2].iov_len = 4 * 512;
	iov[3].iov_base = payload_read + 7 * 512;
	iov[3].iov_len = 25 * 512;
	spdk_blob_io_readv(blob, channel, iov, 4, 32, 32, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_read + 0 * 512, payload_ff, 4 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + 4 * 512, payload_00, 8 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + 12 * 512, payload_ff, 2 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + 14 * 512, payload_00, 18 * 512) == 0);

	/* Read whole two clusters
	 * cluster0: [ (F0F0 AAAA | AAAA) 0000 | 0000 0000 | 0000 FFFF ]
	 * cluster1: [ FFFF 0000 | 0000 FF00 | 0000 0000 | 0000 0000) ] */
	memset(payload_read, 0x00, sizeof(payload_read));
	iov[0].iov_base = payload_read;
	iov[0].iov_len = 1 * 512;
	iov[1].iov_base = payload_read + 1 * 512;
	iov[1].iov_len = 8 * 512;
	iov[2].iov_base = payload_read + 9 * 512;
	iov[2].iov_len = 16 * 512;
	iov[3].iov_base = payload_read + 25 * 512;
	iov[3].iov_len = 39 * 512;
	spdk_blob_io_readv(blob, channel, iov, 4, 0, 64, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	CU_ASSERT(memcmp(payload_read + 0 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(payload_read + 1 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(payload_read + 2 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(payload_read + 3 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(payload_read + 4 * 512, payload_aa, 8 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + 28 * 512, payload_ff, 4 * 512) == 0);

	CU_ASSERT(memcmp(payload_read + (32 + 0) * 512, payload_ff, 4 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + (32 + 4) * 512, payload_00, 8 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + (32 + 12) * 512, payload_ff, 2 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + (32 + 14) * 512, payload_00, 18 * 512) == 0);
}

static void
blob_io_unit(void)
{
	struct spdk_bs_opts bsopts;
	struct spdk_blob_opts opts;
	struct spdk_bs_dev *dev;
	struct spdk_blob *blob, *snapshot, *clone;
	spdk_blob_id blobid;
	struct spdk_io_channel *channel;

	/* Create dev with 512 bytes io unit size */

	spdk_bs_opts_init(&bsopts);
	bsopts.cluster_sz = SPDK_BS_PAGE_SIZE * 4; // 8 * 4 = 32 io_unit
	snprintf(bsopts.bstype.bstype, sizeof(bsopts.bstype.bstype), "TESTTYPE");

	/* Try to initialize a new blob store with unsupported io_unit */
	dev = init_dev();
	dev->blocklen = 512;
	dev->blockcnt =  DEV_BUFFER_SIZE / dev->blocklen;

	/* Initialize a new blob store */
	spdk_bs_init(dev, &bsopts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	CU_ASSERT(spdk_bs_get_io_unit_size(g_bs) == 512);
	channel = spdk_bs_alloc_io_channel(g_bs);

	/* Create thick provisioned blob */
	spdk_blob_opts_init(&opts);
	opts.thin_provision = false;
	opts.num_clusters = 32;

	spdk_bs_create_blob_ext(g_bs, &opts, blob_op_with_id_complete, NULL);

	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_open_blob(g_bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;

	test_io_write(dev, blob, channel);
	test_io_read(dev, blob, channel);
	test_io_zeroes(dev, blob, channel);

	test_iov_write(dev, blob, channel);
	test_iov_read(dev, blob, channel);

	test_io_unmap(dev, blob, channel);

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	blob = NULL;
	g_blob = NULL;

	/* Create thin provisioned blob */

	spdk_blob_opts_init(&opts);
	opts.thin_provision = true;
	opts.num_clusters = 32;

	spdk_bs_create_blob_ext(g_bs, &opts, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_open_blob(g_bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;

	test_io_write(dev, blob, channel);
	test_io_read(dev, blob, channel);

	test_io_zeroes(dev, blob, channel);

	test_iov_write(dev, blob, channel);
	test_iov_read(dev, blob, channel);

	/* Create snapshot */

	spdk_bs_create_snapshot(g_bs, blobid, NULL, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_open_blob(g_bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	snapshot = g_blob;

	spdk_bs_create_clone(g_bs, blobid, NULL, blob_op_with_id_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_open_blob(g_bs, blobid, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	clone = g_blob;

	test_io_read(dev, blob, channel);
	test_io_read(dev, snapshot, channel);
	test_io_read(dev, clone, channel);

	test_iov_read(dev, blob, channel);
	test_iov_read(dev, snapshot, channel);
	test_iov_read(dev, clone, channel);

	/* Inflate clone */

	spdk_bs_inflate_blob(g_bs, channel, blobid, blob_op_complete, NULL);

	CU_ASSERT(g_bserrno == 0);

	test_io_read(dev, clone, channel);

	test_io_unmap(dev, clone, channel);

	test_iov_write(dev, clone, channel);
	test_iov_read(dev, clone, channel);

	spdk_blob_close(blob, blob_op_complete, NULL);
	spdk_blob_close(snapshot, blob_op_complete, NULL);
	spdk_blob_close(clone, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	blob = NULL;
	g_blob = NULL;

	/* Unload the blob store */
	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
	g_blob = NULL;
	g_blobid = 0;
}

static void
blob_io_unit_compatiblity(void)
{
	struct spdk_bs_opts bsopts;
	struct spdk_bs_dev *dev;
	struct spdk_bs_super_block *super;

	/* Create dev with 512 bytes io unit size */

	spdk_bs_opts_init(&bsopts);
	bsopts.cluster_sz = SPDK_BS_PAGE_SIZE * 4; // 8 * 4 = 32 io_unit
	snprintf(bsopts.bstype.bstype, sizeof(bsopts.bstype.bstype), "TESTTYPE");

	/* Try to initialize a new blob store with unsupported io_unit */
	dev = init_dev();
	dev->blocklen = 512;
	dev->blockcnt =  DEV_BUFFER_SIZE / dev->blocklen;

	/* Initialize a new blob store */
	spdk_bs_init(dev, &bsopts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	CU_ASSERT(spdk_bs_get_io_unit_size(g_bs) == 512);

	/* Unload the blob store */
	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* Modify super block to behave like older version.
	 * Check if loaded io unit size equals SPDK_BS_PAGE_SIZE */
	super = (struct spdk_bs_super_block *)&g_dev_buffer[0];
	super->io_unit_size = 0;
	super->crc = _spdk_blob_md_page_calc_crc(super);

	dev = init_dev();
	dev->blocklen = 512;
	dev->blockcnt =  DEV_BUFFER_SIZE / dev->blocklen;

	spdk_bs_load(dev, &bsopts, bs_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	CU_ASSERT(spdk_bs_get_io_unit_size(g_bs) == SPDK_BS_PAGE_SIZE);

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
		CU_add_test(suite, "blob_create_internal", blob_create_internal) == NULL ||
		CU_add_test(suite, "blob_thin_provision", blob_thin_provision) == NULL ||
		CU_add_test(suite, "blob_snapshot", blob_snapshot) == NULL ||
		CU_add_test(suite, "blob_clone", blob_clone) == NULL ||
		CU_add_test(suite, "blob_inflate", blob_inflate) == NULL ||
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
		CU_add_test(suite, "bs_load_custom_cluster_size", bs_load_custom_cluster_size) == NULL ||
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
		CU_add_test(suite, "blob_thin_prov_rw_iov", blob_thin_prov_rw_iov) == NULL ||
		CU_add_test(suite, "bs_load_iter", bs_load_iter) == NULL ||
		CU_add_test(suite, "blob_snapshot_rw", blob_snapshot_rw) == NULL ||
		CU_add_test(suite, "blob_snapshot_rw_iov", blob_snapshot_rw_iov) == NULL ||
		CU_add_test(suite, "blob_relations", blob_relations) == NULL ||
		CU_add_test(suite, "blob_inflate_rw", blob_inflate_rw) == NULL ||
		CU_add_test(suite, "blob_snapshot_freeze_io", blob_snapshot_freeze_io) == NULL ||
		CU_add_test(suite, "blob_operation_split_rw", blob_operation_split_rw) == NULL ||
		CU_add_test(suite, "blob_operation_split_rw_iov", blob_operation_split_rw_iov) == NULL ||
		CU_add_test(suite, "blob_io_unit", blob_io_unit) == NULL ||
		CU_add_test(suite, "blob_io_unit_compatiblity", blob_io_unit_compatiblity) == NULL
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
