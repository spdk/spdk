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

#include "vbdev_lvol.c"

struct spdk_lvol_store *g_lvs = NULL;

void
spdk_lvol_destroy(struct spdk_lvol *lvol)
{
}

struct lvol_store_bdev_pair *
vbdev_get_lvs_pair_by_lvs(struct spdk_lvol_store *lvs_orig)
{
	return NULL;
}

int
spdk_json_write_name(struct spdk_json_write_ctx *w, const char *name)
{
	return 0;
}

int
spdk_json_write_string(struct spdk_json_write_ctx *w, const char *val)
{
	return 0;
}

const char *
spdk_bdev_get_name(const struct spdk_bdev *bdev)
{
	return "test";
}

void
spdk_bdev_register(struct spdk_bdev *bdev)
{
}

struct spdk_lvol_store *
vbdev_get_lvol_store_by_guid(uuid_t uuid)
{
	if (uuid == 0)
		return NULL;
	else if (uuid == g_lvs->uuid)
		return g_lvs;
	else
		return NULL;
}

void
spdk_lvol_create(struct spdk_lvol_store *ls, size_t sz, spdk_lvol_op_with_handle_complete cb_fn,
		 void *cb_arg)
{
}

static void
vbdev_lvol_create_complete(void *cb_arg, struct spdk_lvol *lvol, int lvolerrno)
{
	if (lvolerrno < 0) {
		CU_ASSERT(lvol == NULL);
		return;
	}
	CU_ASSERT(lvol != NULL);
}

static void
lvol_init(void)
{
	int sz = 10;
	g_lvs = calloc(1, sizeof(*g_lvs));
	uuid_generate_time(g_lvs->uuid);

	vbdev_lvol_create(g_lvs->uuid, sz, vbdev_lvol_create_complete, NULL);
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
		CU_add_test(suite, "lvol_init", lvol_init) == NULL
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
