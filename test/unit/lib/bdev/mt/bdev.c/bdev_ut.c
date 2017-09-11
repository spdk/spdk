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

#include "lib/test_env.c"
#include "lib/ut_multithread.c"

/* HACK: disable VTune integration so the unit test doesn't need VTune headers and libs to build */
#undef SPDK_CONFIG_VTUNE

#include "bdev.c"

#define BDEV_UT_NUM_THREADS 3

DEFINE_STUB_V(spdk_scsi_nvme_translate, (const struct spdk_bdev_io *bdev_io,
		int *sc, int *sk, int *asc, int *ascq));

struct ut_bdev {
	struct spdk_bdev	bdev;
	int			io_target;
};

struct ut_bdev g_bdev;
struct spdk_bdev_desc *g_desc;

static int
stub_create_ch(void *io_device, void *ctx_buf)
{
	return 0;
}

static void
stub_destroy_ch(void *io_device, void *ctx_buf)
{
}

static struct spdk_io_channel *
stub_get_io_channel(void *ctx)
{
	return spdk_get_io_channel(&g_bdev.io_target);
}

static int
stub_destruct(void *ctx)
{
	return 0;
}

static struct spdk_bdev_fn_table fn_table = {
	.get_io_channel =	stub_get_io_channel,
	.destruct =		stub_destruct,
};

static int
module_init(void)
{
	return 0;
}

static void
module_fini(void)
{
}

SPDK_BDEV_MODULE_REGISTER(bdev_ut, module_init, module_fini, NULL, NULL, NULL)

static void
register_bdev(void)
{
	g_bdev.bdev.name = "bdev_ut";
	g_bdev.bdev.fn_table = &fn_table;
	g_bdev.bdev.module = SPDK_GET_BDEV_MODULE(bdev_ut);

	spdk_io_device_register(&g_bdev.io_target, stub_create_ch, stub_destroy_ch, 0);
	spdk_bdev_register(&g_bdev.bdev);
}

static void
unregister_bdev(void)
{
	/* Handle any deferred messages. */
	poll_threads();
	spdk_bdev_unregister(&g_bdev.bdev);
	spdk_io_device_unregister(&g_bdev.io_target, NULL);
	memset(&g_bdev, 0, sizeof(g_bdev));
}

static void
bdev_init_cb(void *done, int rc)
{
	CU_ASSERT(rc == 0);
	*(bool *)done = true;
}

static void
setup_test(void)
{
	bool done = false;

	allocate_threads(BDEV_UT_NUM_THREADS);
	spdk_bdev_initialize(bdev_init_cb, &done, NULL, NULL);
	register_bdev();
	spdk_bdev_open(&g_bdev.bdev, true, NULL, NULL, &g_desc);
}

static void
teardown_test(void)
{
	spdk_bdev_close(g_desc);
	g_desc = NULL;
	unregister_bdev();
	spdk_bdev_finish();
	free_threads();
}

static void
test1(void)
{
	setup_test();

	set_thread(0);

	g_ut_threads[0].ch = spdk_bdev_get_io_channel(g_desc);
	spdk_put_io_channel(g_ut_threads[0].ch);

	teardown_test();
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("bdev", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "test1", test1) == NULL
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
