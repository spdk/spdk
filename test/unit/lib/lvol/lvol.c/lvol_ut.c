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

int g_lvserrno;
int g_resize_rc;
struct spdk_lvol_store *g_lvol_store;
struct spdk_lvol *g_lvol;
struct spdk_bs_opts g_bs_opts;

struct spdk_blob_store {
	int stub;
};

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
spdk_bs_destroy(struct spdk_blob_store *bs, spdk_bs_op_complete cb_fn, void *cb_arg)
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
	spdk_blob_id id = 0;
	return id;
}

void
spdk_bs_opts_init(struct spdk_bs_opts *opts)
{
	opts->cluster_sz = SPDK_BLOB_OPTS_CLUSTER_SZ;
	opts->num_md_pages = SPDK_BLOB_OPTS_NUM_MD_PAGES;
	opts->max_md_ops = SPDK_BLOB_OPTS_MAX_MD_OPS;
	opts->max_channel_ops = SPDK_BLOB_OPTS_MAX_CHANNEL_OPS;
}

uint64_t
spdk_bs_get_cluster_size(struct spdk_blob_store *bs)
{
	return BS_CLUSTER_SIZE;
}

void spdk_bs_md_close_blob(struct spdk_blob **b,
			   spdk_blob_op_complete cb_fn, void *cb_arg)
{
	cb_fn(cb_arg, 0);
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
	cb_fn(cb_arg, NULL, 0);
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
lvs_init_destroy_success(void)
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
	rc = spdk_lvs_destroy(g_lvol_store, lvol_store_op_complete, NULL);
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
		CU_add_test(suite, "lvs_init_destroy_success", lvs_init_destroy_success) == NULL ||
		CU_add_test(suite, "lvs_init_opts_success", lvs_init_opts_success) == NULL ||
		CU_add_test(suite, "lvs_unload_lvs_is_null_fail", lvs_unload_lvs_is_null_fail) == NULL ||
		CU_add_test(suite, "lvol_create_destroy_success", lvol_create_destroy_success) == NULL ||
		CU_add_test(suite, "lvol_create_fail", lvol_create_fail) == NULL ||
		CU_add_test(suite, "lvol_destroy_fail", lvol_destroy_fail) == NULL ||
		CU_add_test(suite, "lvol_close_fail", lvol_close_fail) == NULL ||
		CU_add_test(suite, "lvol_close_success", lvol_close_success) == NULL ||
		CU_add_test(suite, "lvol_resize", lvol_resize) == NULL
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
