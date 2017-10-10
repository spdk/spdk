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

#include "spdk_cunit.h"
#include "spdk/blob.h"
#include "spdk/io_channel.h"

#include "lib/test_env.c"

#include "lvol.c"

#define DEV_BUFFER_SIZE (64 * 1024 * 1024)
#define DEV_BUFFER_BLOCKLEN (4096)
#define DEV_BUFFER_BLOCKCNT (DEV_BUFFER_SIZE / DEV_BUFFER_BLOCKLEN)
#define BS_CLUSTER_SIZE (1024 * 1024)
#define BS_FREE_CLUSTERS (DEV_BUFFER_SIZE / BS_CLUSTER_SIZE)
#define BS_PAGE_SIZE (4096)

#define SPDK_BLOB_OPTS_CLUSTER_SZ (1024 * 1024)
#define SPDK_BLOB_OPTS_NUM_MD_PAGES UINT32_MAX
#define SPDK_BLOB_OPTS_MAX_MD_OPS 32
#define SPDK_BLOB_OPTS_MAX_CHANNEL_OPS 512

const char *uuid = "828d9766-ae50-11e7-bd8d-001e67edf35d";

struct spdk_blob {
	spdk_blob_id	id;
};

int g_lvolerrno;
int g_lvserrno;
int g_bs_load_status;
int g_get_super_status;
int g_open_blob_status;
int g_get_uuid_status;
int g_close_super_status;
int g_resize_rc;
int g_lvols_count;
int g_fail_on_n_lvol_load = 0xFF;
struct spdk_lvol_store *g_lvol_store;
struct spdk_lvol *g_lvol;
struct spdk_bs_opts g_bs_opts;

struct spdk_blob_store {
	int stub;
};
struct spdk_blob_store *g_blob_store;
struct spdk_blob *g_blob;

static void
bs_iter(void)
{
	if (g_lvols_count > 0) {
		g_blob->id = g_lvols_count + 1; /* skip super blob */
		g_lvols_count--;
		g_fail_on_n_lvol_load--;
	} else {
		g_lvolerrno = -ENOENT;
	}

	if (g_fail_on_n_lvol_load == 0) {
		g_lvolerrno = -1;
	}
}

void
spdk_bs_md_iter_next(struct spdk_blob_store *bs, struct spdk_blob **b,
		     spdk_blob_op_with_handle_complete cb_fn, void *cb_arg)
{
	bs_iter();

	cb_fn(cb_arg, g_blob, g_lvolerrno);
}

void
spdk_bs_md_iter_first(struct spdk_blob_store *bs,
		      spdk_blob_op_with_handle_complete cb_fn, void *cb_arg)
{
	bs_iter();

	cb_fn(cb_arg, g_blob, g_lvolerrno);
}

uint64_t spdk_blob_get_num_clusters(struct spdk_blob *blob)
{
	return 0;
}

void
spdk_bs_get_super(struct spdk_blob_store *bs,
		  spdk_blob_op_with_id_complete cb_fn, void *cb_arg)
{
	cb_fn(cb_arg, 0, g_get_super_status);
}

void
spdk_bs_set_super(struct spdk_blob_store *bs, spdk_blob_id blobid,
		  spdk_bs_op_complete cb_fn, void *cb_arg)
{
	cb_fn(cb_arg, 0);
}

void
spdk_bs_load(struct spdk_bs_dev *dev, struct spdk_bs_opts *opts,
	     spdk_bs_op_with_handle_complete cb_fn, void *cb_arg)
{
	if (g_bs_load_status == 0) {
		g_blob_store = calloc(1, sizeof(*g_blob_store));
	}

	cb_fn(cb_arg, g_blob_store, g_bs_load_status);
}

struct spdk_io_channel *spdk_bs_alloc_io_channel(struct spdk_blob_store *bs)
{
	return NULL;
}

int
spdk_blob_md_set_xattr(struct spdk_blob *blob, const char *name, const void *value,
		       uint16_t value_len)
{
	return 0;
}

int
spdk_bs_md_get_xattr_value(struct spdk_blob *blob, const char *name,
			   const void **value, size_t *value_len)
{
	*value = uuid;
	*value_len = UUID_STRING_LEN;

	return g_get_uuid_status;
}

uint64_t
spdk_bs_get_page_size(struct spdk_blob_store *bs)
{
	return BS_PAGE_SIZE;
}

static void
init_dev(struct spdk_bs_dev *dev)
{
	dev->blockcnt = DEV_BUFFER_BLOCKCNT;
	dev->blocklen = DEV_BUFFER_BLOCKLEN;
	return;
}

void
spdk_bs_init(struct spdk_bs_dev *dev, struct spdk_bs_opts *o,
	     spdk_bs_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_blob_store *bs;

	bs = calloc(1, sizeof(*bs));

	memcpy(&g_bs_opts, o, sizeof(struct spdk_bs_opts));

	cb_fn(cb_arg, bs, 0);
}

void
spdk_bs_unload(struct spdk_blob_store *bs, spdk_bs_op_complete cb_fn, void *cb_arg)
{
	free(bs);

	cb_fn(cb_arg, 0);
}

void
spdk_bs_md_delete_blob(struct spdk_blob_store *bs, spdk_blob_id blobid,
		       spdk_blob_op_complete cb_fn, void *cb_arg)
{
	cb_fn(cb_arg, 0);
}

spdk_blob_id
spdk_blob_get_id(struct spdk_blob *blob)
{
	return blob->id;
}

void
spdk_bs_opts_init(struct spdk_bs_opts *opts)
{
	opts->cluster_sz = SPDK_BLOB_OPTS_CLUSTER_SZ;
	opts->num_md_pages = SPDK_BLOB_OPTS_NUM_MD_PAGES;
	opts->max_md_ops = SPDK_BLOB_OPTS_MAX_MD_OPS;
	opts->max_channel_ops = SPDK_BLOB_OPTS_MAX_CHANNEL_OPS;
	memset(&opts->bstype, 0, sizeof(opts->bstype));
}

uint64_t
spdk_bs_get_cluster_size(struct spdk_blob_store *bs)
{
	return BS_CLUSTER_SIZE;
}

void spdk_bs_md_close_blob(struct spdk_blob **b,
			   spdk_blob_op_complete cb_fn, void *cb_arg)
{
	free(g_blob);

	cb_fn(cb_arg, g_close_super_status);
}

int
spdk_bs_md_resize_blob(struct spdk_blob *blob, uint64_t sz)
{
	return g_resize_rc;
}

void
spdk_bs_md_sync_blob(struct spdk_blob *blob,
		     spdk_blob_op_complete cb_fn, void *cb_arg)
{
	cb_fn(cb_arg, 0);
}

void
spdk_bs_md_open_blob(struct spdk_blob_store *bs, spdk_blob_id blobid,
		     spdk_blob_op_with_handle_complete cb_fn, void *cb_arg)
{
	if (g_open_blob_status == 0) {
		g_blob = calloc(1, sizeof(*g_blob));
	}

	cb_fn(cb_arg, g_blob, g_open_blob_status);
}

uint64_t
spdk_bs_free_cluster_count(struct spdk_blob_store *bs)
{
	return BS_FREE_CLUSTERS;
}

void
spdk_bs_md_create_blob(struct spdk_blob_store *bs,
		       spdk_blob_op_with_id_complete cb_fn, void *cb_arg)
{
	cb_fn(cb_arg, 0, 0);
}

static void
_lvol_send_msg(spdk_thread_fn fn, void *ctx, void *thread_ctx)
{
	fn(ctx);
}

static void
lvol_store_op_with_handle_complete(void *cb_arg, struct spdk_lvol_store *lvol_store, int lvserrno)
{
	g_lvol_store = lvol_store;
	g_lvserrno = lvserrno;
}
static void
lvol_op_with_handle_complete(void *cb_arg, struct spdk_lvol *lvol, int lvserrno)
{
	g_lvol = lvol;
	g_lvserrno = lvserrno;
}

static void
lvol_store_op_complete(void *cb_arg, int lvserrno)
{
	g_lvserrno = lvserrno;
}

static void
lvs_init_unload_success(void)
{
	struct spdk_bs_dev bs_dev;
	int rc = 0;

	init_dev(&bs_dev);

	spdk_allocate_thread(_lvol_send_msg, NULL, NULL);

	g_lvserrno = -1;

	rc = spdk_lvs_init(&bs_dev, NULL, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	spdk_free_thread();
}

static void
lvs_init_opts_success(void)
{
	struct spdk_bs_dev bs_dev;
	struct spdk_lvs_opts opts;
	int rc = 0;

	init_dev(&bs_dev);

	spdk_allocate_thread(_lvol_send_msg, NULL, NULL);

	g_lvserrno = -1;

	opts.cluster_sz = 8192;
	rc = spdk_lvs_init(&bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_bs_opts.cluster_sz == opts.cluster_sz);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	spdk_free_thread();
}

static void
lvs_unload_lvs_is_null_fail(void)
{
	int rc = 0;

	spdk_allocate_thread(_lvol_send_msg, NULL, NULL);

	g_lvserrno = -1;
	rc = spdk_lvs_unload(NULL, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == -ENODEV);
	CU_ASSERT(g_lvserrno == -1);

	spdk_free_thread();
}

static void
lvol_create_destroy_success(void)
{
	struct spdk_bs_dev bs_dev;
	int rc = 0;

	init_dev(&bs_dev);

	spdk_allocate_thread(_lvol_send_msg, NULL, NULL);

	g_lvserrno = -1;
	rc = spdk_lvs_init(&bs_dev, NULL, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	spdk_lvol_create(g_lvol_store, 10, lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	spdk_lvol_destroy(g_lvol);

	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	spdk_free_thread();
}

static void
lvol_create_fail(void)
{
	struct spdk_bs_dev bs_dev;
	int rc = 0;

	init_dev(&bs_dev);

	spdk_allocate_thread(_lvol_send_msg, NULL, NULL);

	g_lvol_store = NULL;
	g_lvserrno = 0;
	rc = spdk_lvs_init(NULL, NULL, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc != 0);
	CU_ASSERT(g_lvol_store == NULL);

	rc = spdk_lvs_init(&bs_dev, NULL, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	g_lvol = NULL;
	rc = spdk_lvol_create(NULL, 10, lvol_op_with_handle_complete, NULL);
	CU_ASSERT(rc != 0);
	CU_ASSERT(g_lvol == NULL);

	rc = spdk_lvol_create(g_lvol_store, DEV_BUFFER_SIZE + 1, lvol_op_with_handle_complete, NULL);
	CU_ASSERT(rc != 0);
	CU_ASSERT(g_lvol == NULL);

	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	spdk_free_thread();
}

static void
lvol_destroy_fail(void)
{
	struct spdk_bs_dev bs_dev;
	int rc = 0;

	init_dev(&bs_dev);

	spdk_allocate_thread(_lvol_send_msg, NULL, NULL);

	rc = spdk_lvs_init(&bs_dev, NULL, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	spdk_lvol_create(g_lvol_store, 10, lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	spdk_lvol_destroy(g_lvol);
	// nothing to check here... it should just still work

	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	spdk_free_thread();
}

static void
lvol_close_fail(void)
{
	struct spdk_bs_dev bs_dev;
	int rc = 0;

	init_dev(&bs_dev);

	spdk_allocate_thread(_lvol_send_msg, NULL, NULL);

	rc = spdk_lvs_init(&bs_dev, NULL, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	spdk_lvol_create(g_lvol_store, 10, lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	spdk_lvol_close(g_lvol);
	// nothing to check here... it should just still work

	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	spdk_free_thread();
}

static void
lvol_close_success(void)
{
	struct spdk_bs_dev bs_dev;
	int rc = 0;

	init_dev(&bs_dev);

	spdk_allocate_thread(_lvol_send_msg, NULL, NULL);

	g_lvserrno = -1;
	rc = spdk_lvs_init(&bs_dev, NULL, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	spdk_lvol_create(g_lvol_store, 10, lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	spdk_lvol_close(g_lvol);

	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	spdk_free_thread();
}

static void
lvol_resize(void)
{
	struct spdk_bs_dev bs_dev;
	int rc = 0;

	init_dev(&bs_dev);

	spdk_allocate_thread(_lvol_send_msg, NULL, NULL);

	g_resize_rc = 0;
	g_lvserrno = -1;
	rc = spdk_lvs_init(&bs_dev, NULL, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	spdk_lvol_create(g_lvol_store, 10, lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	/* Resize to same size */
	rc = spdk_lvol_resize(g_lvol, 10, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);

	/* Resize to smaller size */
	rc = spdk_lvol_resize(g_lvol, 5, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);

	/* Resize to bigger size */
	rc = spdk_lvol_resize(g_lvol, 15, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);

	/* Resize to size = 0 */
	rc = spdk_lvol_resize(g_lvol, 0, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);

	/* Resize to bigger size than available */
	rc = spdk_lvol_resize(g_lvol, 0xFFFFFFFF, lvol_store_op_complete, NULL);
	CU_ASSERT(rc != 0);

	/* Fail resize */
	g_resize_rc = -1;
	g_lvserrno = 0;
	rc = spdk_lvol_resize(g_lvol, 10, lvol_store_op_complete, NULL);
	CU_ASSERT(rc != 0);
	CU_ASSERT(g_lvserrno != 0);

	spdk_lvol_destroy(g_lvol);

	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	spdk_free_thread();
}

static void
lvs_load(void)
{
	int rc = -1;
	struct spdk_bs_dev bs_dev;
	struct spdk_lvs_with_handle_req *req;

	req = calloc(1, sizeof(*req));
	SPDK_CU_ASSERT_FATAL(req != NULL);

	init_dev(&bs_dev);

	spdk_allocate_thread(_lvol_send_msg, NULL, NULL);

	/* Fail on bs load */
	g_bs_load_status = -1;
	spdk_lvs_load(&bs_dev, lvol_store_op_with_handle_complete, req);
	CU_ASSERT(g_lvserrno != 0);
	CU_ASSERT(g_lvol_store == NULL);

	/* Fail on getting super blob */
	g_bs_load_status = 0;
	g_get_super_status = -1;
	spdk_lvs_load(&bs_dev, lvol_store_op_with_handle_complete, req);
	CU_ASSERT(g_lvserrno == -ENODEV);
	CU_ASSERT(g_lvol_store == NULL);

	/* Fail on opening super blob */
	g_lvserrno = 0;
	g_get_super_status = 0;
	g_open_blob_status = -1;
	spdk_lvs_load(&bs_dev, lvol_store_op_with_handle_complete, req);
	CU_ASSERT(g_lvserrno == -ENODEV);
	CU_ASSERT(g_lvol_store == NULL);

	/* Fail on getting uuid */
	g_lvserrno = 0;
	g_get_super_status = 0;
	g_open_blob_status = 0;
	g_get_uuid_status = -1;
	spdk_lvs_load(&bs_dev, lvol_store_op_with_handle_complete, req);
	CU_ASSERT(g_lvserrno == -ENODEV);
	CU_ASSERT(g_lvol_store == NULL);

	/* Fail on closing super blob */
	g_lvserrno = 0;
	g_get_super_status = 0;
	g_open_blob_status = 0;
	g_get_uuid_status = 0;
	g_close_super_status = -1;
	spdk_lvs_load(&bs_dev, lvol_store_op_with_handle_complete, req);
	CU_ASSERT(g_lvserrno == -ENODEV);
	CU_ASSERT(g_lvol_store == NULL);

	/* Load successfully */
	g_lvserrno = 0;
	g_get_super_status = 0;
	g_open_blob_status = 0;
	g_get_uuid_status = 0;
	g_close_super_status = 0;
	spdk_lvs_load(&bs_dev, lvol_store_op_with_handle_complete, req);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_lvol_store != NULL);

	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);

	free(req);

	spdk_free_thread();
}

static void
lvols_load(void)
{
	int rc = -1;
	struct spdk_bs_dev bs_dev;
	struct spdk_lvs_with_handle_req *req;

	req = calloc(1, sizeof(*req));
	SPDK_CU_ASSERT_FATAL(req != NULL);

	init_dev(&bs_dev);

	spdk_allocate_thread(_lvol_send_msg, NULL, NULL);

	/* Load lvs */
	g_lvserrno = 0;
	g_get_super_status = 0;
	g_open_blob_status = 0;
	g_get_uuid_status = 0;
	g_close_super_status = 0;
	spdk_lvs_load(&bs_dev, lvol_store_op_with_handle_complete, req);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_lvol_store != NULL);

	g_blob = calloc(1, sizeof(*g_blob));

	/* Fail on loading second lvol */
	g_lvols_count = 3;
	g_fail_on_n_lvol_load = 2;
	spdk_load_lvols(g_lvol_store, lvol_store_op_with_handle_complete, req);
	CU_ASSERT(TAILQ_EMPTY(&g_lvol_store->lvols));
	CU_ASSERT(g_lvserrno != 0);

	/* Fail on opening lvol */
	g_lvols_count = 3;
	g_fail_on_n_lvol_load = 0xFF;
	g_open_blob_status = -1;
	spdk_load_lvols(g_lvol_store, lvol_store_op_with_handle_complete, req);
	CU_ASSERT(TAILQ_EMPTY(&g_lvol_store->lvols));
	CU_ASSERT(g_lvserrno != 0);

	/* Load successfully with 0 lvols */
	g_lvserrno = 0;
	g_lvolerrno = 0;
	g_lvols_count = 0;
	g_fail_on_n_lvol_load = 0xFF;
	g_open_blob_status = 0;
	spdk_load_lvols(g_lvol_store, lvol_store_op_with_handle_complete, req);
	CU_ASSERT(TAILQ_EMPTY(&g_lvol_store->lvols));
	CU_ASSERT(g_lvserrno == 0);

	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);

	free(g_blob);

	/* Load lvs */
	g_lvserrno = 0;
	g_get_super_status = 0;
	g_open_blob_status = 0;
	g_get_uuid_status = 0;
	g_close_super_status = 0;
	spdk_lvs_load(&bs_dev, lvol_store_op_with_handle_complete, req);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_lvol_store != NULL);

	g_blob = calloc(1, sizeof(*g_blob));

	/* Load successfully with 3 lvols */
	g_lvserrno = 0;
	g_lvolerrno = 0;
	g_lvols_count = 3;
	g_fail_on_n_lvol_load = 0xFF;
	g_open_blob_status = 0;
	spdk_load_lvols(g_lvol_store, lvol_store_op_with_handle_complete, req);
	CU_ASSERT(!TAILQ_EMPTY(&g_lvol_store->lvols));
	CU_ASSERT(g_lvserrno == 0);

	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);

	free(req);
	free(g_blob);

	spdk_free_thread();
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("lvol", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "lvs_init_unload_success", lvs_init_unload_success) == NULL ||
		CU_add_test(suite, "lvs_init_opts_success", lvs_init_opts_success) == NULL ||
		CU_add_test(suite, "lvs_unload_lvs_is_null_fail", lvs_unload_lvs_is_null_fail) == NULL ||
		CU_add_test(suite, "lvol_create_destroy_success", lvol_create_destroy_success) == NULL ||
		CU_add_test(suite, "lvol_create_fail", lvol_create_fail) == NULL ||
		CU_add_test(suite, "lvol_destroy_fail", lvol_destroy_fail) == NULL ||
		CU_add_test(suite, "lvol_close_fail", lvol_close_fail) == NULL ||
		CU_add_test(suite, "lvol_close_success", lvol_close_success) == NULL ||
		CU_add_test(suite, "lvol_resize", lvol_resize) == NULL ||
		CU_add_test(suite, "lvol_load", lvs_load) == NULL ||
		CU_add_test(suite, "lvol_load", lvols_load) == NULL
	) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
