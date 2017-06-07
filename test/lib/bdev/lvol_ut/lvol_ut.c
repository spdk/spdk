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

#include "vbdev_lvol.c"

int g_bserrno;

static struct spdk_bdev g_bdev = {};
static struct spdk_bs_dev *g_bs_dev = NULL;
static struct spdk_lvol_store *g_lvol_store = NULL;
bool lvol_store_initialize_fail = false;
bool lvol_store_initialize_cb_fail = false;

bool
spdk_bdev_claim(struct spdk_bdev *bdev, spdk_bdev_remove_cb_t remove_cb, void *remove_ctx)
{
	if (bdev->status == SPDK_BDEV_STATUS_CLAIMED)
		return false;

	bdev->status = SPDK_BDEV_STATUS_CLAIMED;
	return true;
}

void
spdk_bdev_unclaim(struct spdk_bdev *bdev)
{
	bdev->status = SPDK_BDEV_STATUS_UNCLAIMED;
	return;
}

const char *
spdk_bdev_get_name(const struct spdk_bdev *bdev)
{
	return "test";
}

static void
bdev_blob_destroy(struct spdk_bs_dev *bs_dev)
{
	CU_ASSERT(g_bs_dev != NULL);
	CU_ASSERT(bs_dev != NULL);
	CU_ASSERT(g_bs_dev == bs_dev);
	free(bs_dev);
	g_bs_dev = NULL;
}

struct spdk_bs_dev *
spdk_bdev_create_bs_dev(struct spdk_bdev *bdev)
{
	struct spdk_bs_dev *bs_dev;

	bs_dev = calloc(1, sizeof(*bs_dev));
	bs_dev->destroy = bdev_blob_destroy;

	CU_ASSERT(g_bs_dev == NULL);
	g_bs_dev = bs_dev;
	return bs_dev;
}

int
lvol_store_initialize(struct spdk_bs_dev *bs_dev, spdk_lvol_store_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_store *lvol_store;
	int error = 0;

	if (lvol_store_initialize_fail)
		return -1;

	if (lvol_store_initialize_cb_fail) {
		lvol_store = NULL;
		error = -1;
	} else {
		lvol_store = calloc(1, sizeof(*lvol_store));
		error = 0;
	}
	cb_fn(cb_arg, lvol_store, error);

	return 0;
}

int
lvol_store_free(struct spdk_lvol_store *lvol_store, spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	bdev_blob_destroy(lvol_store->bs_dev);
	free(lvol_store);
	cb_fn(cb_arg, 0);
	return 0;
}

void
spdk_vbdev_module_init_next(int rc)
{
	return;
}

void
spdk_vbdev_module_list_add(struct spdk_bdev_module_if *vbdev_module)
{
	return;
}

static void
lvol_store_op_with_handle_complete(void *cb_arg, struct spdk_lvol_store *lvol_store, int bserrno)
{
	g_bserrno = bserrno;
	g_lvol_store = lvol_store;
	return;
}

static void
lvol_store_op_complete(void *cb_arg, int bserrno)
{
	g_bserrno = bserrno;
	return;
}


static void
lvol_init(void)
{
	int rc = 0;
	struct spdk_lvol_store *lvol_store_temp;
	struct spdk_bs_dev *bs_dev_temp;

	g_bdev.status = SPDK_BDEV_STATUS_UNCLAIMED;

	// 1) INIT FAIL
	lvol_store_initialize_fail = true;

	rc = vbdev_construct_lvol_store(&g_bdev, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc != 0);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_lvol_store == NULL);
	CU_ASSERT(g_bs_dev == NULL);

	lvol_store_initialize_fail = false;

	// 1) INIT cb FAIL
	lvol_store_initialize_cb_fail = true;

	rc = vbdev_construct_lvol_store(&g_bdev, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_bserrno != 0);
	CU_ASSERT(g_lvol_store == NULL);
	CU_ASSERT(g_bs_dev == NULL);

	lvol_store_initialize_cb_fail = false;

	// 2) Success

	rc = vbdev_construct_lvol_store(&g_bdev, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_lvol_store != NULL);
	CU_ASSERT(g_bs_dev != NULL);

	lvol_store_temp = g_lvol_store;
	g_lvol_store = NULL;
	bs_dev_temp = g_bs_dev;
	g_bs_dev = NULL;

	// 3) Already claimed

	rc = vbdev_construct_lvol_store(&g_bdev, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc != 0);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_lvol_store == NULL);
	CU_ASSERT(g_bs_dev == NULL);

	// 4) Destroy

	g_bs_dev = bs_dev_temp;

	vbdev_destruct_lvol_store(lvol_store_temp, lvol_store_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_lvol_store == NULL);
	CU_ASSERT(g_bs_dev == NULL);
}

static void
lvol_fini(void)
{

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
		CU_add_test(suite, "lvol_init", lvol_init) == NULL ||
		CU_add_test(suite, "lvol_fini", lvol_fini) == NULL
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
