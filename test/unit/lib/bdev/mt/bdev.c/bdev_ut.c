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

struct ut_bdev_channel {
	TAILQ_HEAD(, spdk_bdev_io)	outstanding_io;
};

struct ut_bdev g_bdev;
struct spdk_bdev_desc *g_desc;

static int
stub_create_ch(void *io_device, void *ctx_buf)
{
	struct ut_bdev_channel *ch = ctx_buf;

	TAILQ_INIT(&ch->outstanding_io);
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

static void
stub_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
	struct ut_bdev_channel *ch = spdk_io_channel_get_ctx(_ch);

	TAILQ_INSERT_TAIL(&ch->outstanding_io, bdev_io, module_link);
}

static void
stub_complete_io(void)
{
	struct spdk_io_channel *_ch = spdk_get_io_channel(&g_bdev.io_target);
	struct ut_bdev_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct spdk_bdev_io *io;

	while (!TAILQ_EMPTY(&ch->outstanding_io)) {
		io = TAILQ_FIRST(&ch->outstanding_io);
		TAILQ_REMOVE(&ch->outstanding_io, io, module_link);
		spdk_bdev_io_complete(io, SPDK_BDEV_IO_STATUS_SUCCESS);
	}

	spdk_put_io_channel(_ch);
}

static struct spdk_bdev_fn_table fn_table = {
	.get_io_channel =	stub_get_io_channel,
	.destruct =		stub_destruct,
	.submit_request =	stub_submit_request,
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
	g_bdev.bdev.blocklen = 4096;
	g_bdev.bdev.blockcnt = 1024;

	spdk_io_device_register(&g_bdev.io_target, stub_create_ch, stub_destroy_ch,
				sizeof(struct ut_bdev_channel));
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
basic(void)
{
	setup_test();

	set_thread(0);

	g_ut_threads[0].ch = spdk_bdev_get_io_channel(g_desc);
	spdk_put_io_channel(g_ut_threads[0].ch);

	teardown_test();
}

static void
reset_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	bool *done = cb_arg;

	CU_ASSERT(success == true);
	*done = true;
	spdk_bdev_free_io(bdev_io);
}

static void
put_channel_during_reset(void)
{
	struct spdk_io_channel *io_ch;
	bool done = false;

	setup_test();

	set_thread(0);
	io_ch = spdk_bdev_get_io_channel(g_desc);
	CU_ASSERT(io_ch != NULL);

	/*
	 * Start a reset, but then put the I/O channel before
	 *  the deferred messages for the reset get a chance to
	 *  execute.
	 */
	spdk_bdev_reset(g_desc, io_ch, reset_done, &done);
	spdk_put_io_channel(io_ch);
	poll_threads();
	stub_complete_io();

	teardown_test();
}

static void
aborted_reset_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	enum spdk_bdev_io_status *status = cb_arg;

	*status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;
	spdk_bdev_free_io(bdev_io);
}

static void
aborted_reset(void)
{
	struct spdk_io_channel *io_ch[2];
	enum spdk_bdev_io_status status1, status2;

	setup_test();

	set_thread(0);
	io_ch[0] = spdk_bdev_get_io_channel(g_desc);
	CU_ASSERT(io_ch[0] != NULL);
	spdk_bdev_reset(g_desc, io_ch[0], aborted_reset_done, &status1);
	poll_threads();
	CU_ASSERT(g_bdev.bdev.reset_in_progress != NULL);

	/*
	 * First reset has been submitted on ch0.  Now submit a second
	 *  reset on ch1 which will get queued since there is already a
	 *  reset in progress.
	 */
	set_thread(1);
	io_ch[1] = spdk_bdev_get_io_channel(g_desc);
	CU_ASSERT(io_ch[1] != NULL);
	spdk_bdev_reset(g_desc, io_ch[1], aborted_reset_done, &status2);
	poll_threads();
	CU_ASSERT(g_bdev.bdev.reset_in_progress != NULL);

	/*
	 * Now destroy ch1.  This will abort the queued reset.  Check that
	 *  the second reset was completed with failed status.  Also check
	 *  that bdev->reset_in_progress != NULL, since the original reset
	 *  has not been completed yet.  This ensures that the bdev code is
	 *  correctly noticing that the failed reset is *not* the one that
	 *  had been submitted to the bdev module.
	 */
	set_thread(1);
	spdk_put_io_channel(io_ch[1]);
	poll_threads();
	CU_ASSERT(status2 == SPDK_BDEV_IO_STATUS_FAILED);
	CU_ASSERT(g_bdev.bdev.reset_in_progress != NULL);

	/*
	 * Now complete the first reset, verify that it completed with SUCCESS
	 *  status and that bdev->reset_in_progress is also set back to NULL.
	 */
	set_thread(0);
	spdk_put_io_channel(io_ch[0]);
	stub_complete_io();
	poll_threads();
	CU_ASSERT(status1 == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(g_bdev.bdev.reset_in_progress == NULL);

	teardown_test();
}

static void
io_during_reset_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	enum spdk_bdev_io_status *status = cb_arg;

	*status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;
	spdk_bdev_free_io(bdev_io);
}

static void
io_during_reset(void)
{
	struct spdk_io_channel *io_ch[2];
	struct spdk_bdev_channel *bdev_ch[2];
	enum spdk_bdev_io_status status0, status1, status_reset;
	int rc;

	setup_test();

	/*
	 * First test normal case - submit an I/O on each of two channels (with no resets)
	 *  and verify they complete successfully.
	 */
	set_thread(0);
	io_ch[0] = spdk_bdev_get_io_channel(g_desc);
	bdev_ch[0] = spdk_io_channel_get_ctx(io_ch[0]);
	CU_ASSERT(bdev_ch[0]->flags == 0);
	status0 = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_read_blocks(g_desc, io_ch[0], NULL, 0, 1, io_during_reset_done, &status0);
	CU_ASSERT(rc == 0);

	set_thread(1);
	io_ch[1] = spdk_bdev_get_io_channel(g_desc);
	bdev_ch[1] = spdk_io_channel_get_ctx(io_ch[1]);
	CU_ASSERT(bdev_ch[1]->flags == 0);
	status1 = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_read_blocks(g_desc, io_ch[1], NULL, 0, 1, io_during_reset_done, &status1);
	CU_ASSERT(rc == 0);

	poll_threads();
	CU_ASSERT(status0 == SPDK_BDEV_IO_STATUS_PENDING);
	CU_ASSERT(status1 == SPDK_BDEV_IO_STATUS_PENDING);

	set_thread(0);
	stub_complete_io();
	CU_ASSERT(status0 == SPDK_BDEV_IO_STATUS_SUCCESS);

	set_thread(1);
	stub_complete_io();
	CU_ASSERT(status1 == SPDK_BDEV_IO_STATUS_SUCCESS);

	/*
	 * Now submit a reset, and leave it pending while we submit I?O on two different
	 *  channels.  These I/O should be failed by the bdev layer since the reset is in
	 *  progress.
	 */
	set_thread(0);
	status_reset = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_reset(g_desc, io_ch[0], io_during_reset_done, &status_reset);
	CU_ASSERT(rc == 0);

	CU_ASSERT(bdev_ch[0]->flags == 0);
	CU_ASSERT(bdev_ch[1]->flags == 0);
	poll_threads();
	CU_ASSERT(bdev_ch[0]->flags == BDEV_CH_RESET_IN_PROGRESS);
	CU_ASSERT(bdev_ch[1]->flags == BDEV_CH_RESET_IN_PROGRESS);

	set_thread(0);
	status0 = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_read_blocks(g_desc, io_ch[0], NULL, 0, 1, io_during_reset_done, &status0);
	CU_ASSERT(rc == 0);

	set_thread(1);
	status1 = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_read_blocks(g_desc, io_ch[1], NULL, 0, 1, io_during_reset_done, &status1);
	CU_ASSERT(rc == 0);

	/*
	 * A reset is in progress so these read I/O should complete with failure.  Note that we
	 *  need to poll_threads() since I/O completed inline have their completion deferred.
	 */
	poll_threads();
	CU_ASSERT(status_reset == SPDK_BDEV_IO_STATUS_PENDING);
	CU_ASSERT(status0 == SPDK_BDEV_IO_STATUS_FAILED);
	CU_ASSERT(status1 == SPDK_BDEV_IO_STATUS_FAILED);

	set_thread(0);
	stub_complete_io();
	spdk_put_io_channel(io_ch[0]);
	set_thread(1);
	spdk_put_io_channel(io_ch[1]);
	poll_threads();
	CU_ASSERT(status_reset == SPDK_BDEV_IO_STATUS_SUCCESS);

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
		CU_add_test(suite, "basic", basic) == NULL ||
		CU_add_test(suite, "put_channel_during_reset", put_channel_during_reset) == NULL ||
		CU_add_test(suite, "aborted_reset", aborted_reset) == NULL ||
		CU_add_test(suite, "io_during_reset", io_during_reset) == NULL
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
