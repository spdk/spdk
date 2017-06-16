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
#include "spdk/io_channel.h"

#include "lib/test_env.c"
#include "lib/blob/bs_dev_common.c"
#include "lvol.c"

int g_bserrno;
struct spdk_lvol_store *g_lvol_store;

static void
_lvol_send_msg(spdk_thread_fn fn, void *ctx, void *thread_ctx)
{
	fn(ctx);
}

static void
lvol_store_op_with_handle_complete(void *cb_arg, struct spdk_lvol_store *lvol_store, int bserrno)
{
	g_lvol_store = lvol_store;
	g_bserrno = bserrno;
}

static void
lvol_store_op_complete(void *cb_arg, int bserrno)
{
	g_bserrno = bserrno;
}

static void
lvol_init(void)
{
	struct spdk_bs_dev bs_dev;
	int rc = 0;

	init_dev(&bs_dev);

	spdk_allocate_thread(_lvol_send_msg, NULL);

	g_bserrno = -1;
	rc = spdk_lvol_store_init(&bs_dev, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	g_bserrno = -1;
	rc = spdk_lvol_store_unload(g_lvol_store, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_bserrno == 0);
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
		CU_add_test(suite, "lvol_store_init_fini", lvol_init) == NULL
	) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	g_dev_buffer = calloc(1, DEV_BUFFER_SIZE);
	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	free(g_dev_buffer);
	return num_failures;
}
