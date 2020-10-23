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

#include "common/lib/ut_multithread.c"
#include "unit/lib/json_mock.c"

#include "spdk/config.h"
/* HACK: disable VTune integration so the unit test doesn't need VTune headers and libs to build */
#undef SPDK_CONFIG_VTUNE

#include "bdev/bdev.c"

#define BDEV_UT_NUM_THREADS 3

struct spdk_trace_histories *g_trace_histories;
DEFINE_STUB_V(spdk_trace_add_register_fn, (struct spdk_trace_register_fn *reg_fn));
DEFINE_STUB_V(spdk_trace_register_owner, (uint8_t type, char id_prefix));
DEFINE_STUB_V(spdk_trace_register_object, (uint8_t type, char id_prefix));
DEFINE_STUB_V(spdk_trace_register_description, (const char *name,
		uint16_t tpoint_id, uint8_t owner_type,
		uint8_t object_type, uint8_t new_object,
		uint8_t arg1_type, const char *arg1_name));
DEFINE_STUB_V(_spdk_trace_record, (uint64_t tsc, uint16_t tpoint_id, uint16_t poller_id,
				   uint32_t size, uint64_t object_id, uint64_t arg1));
DEFINE_STUB(spdk_notify_send, uint64_t, (const char *type, const char *ctx), 0);
DEFINE_STUB(spdk_notify_type_register, struct spdk_notify_type *, (const char *type), NULL);
DEFINE_STUB_V(spdk_scsi_nvme_translate, (const struct spdk_bdev_io *bdev_io, int *sc, int *sk,
		int *asc, int *ascq));

struct ut_bdev {
	struct spdk_bdev	bdev;
	void			*io_target;
};

struct ut_bdev_channel {
	TAILQ_HEAD(, spdk_bdev_io)	outstanding_io;
	uint32_t			outstanding_cnt;
	uint32_t			avail_cnt;
};

int g_io_device;
struct ut_bdev g_bdev;
struct spdk_bdev_desc *g_desc;
bool g_teardown_done = false;
bool g_get_io_channel = true;
bool g_create_ch = true;
bool g_init_complete_called = false;
bool g_fini_start_called = true;
int g_status = 0;
int g_count = 0;
struct spdk_histogram_data *g_histogram = NULL;

static int
stub_create_ch(void *io_device, void *ctx_buf)
{
	struct ut_bdev_channel *ch = ctx_buf;

	if (g_create_ch == false) {
		return -1;
	}

	TAILQ_INIT(&ch->outstanding_io);
	ch->outstanding_cnt = 0;
	/*
	 * When avail gets to 0, the submit_request function will return ENOMEM.
	 *  Most tests to not want ENOMEM to occur, so by default set this to a
	 *  big value that won't get hit.  The ENOMEM tests can then override this
	 *  value to something much smaller to induce ENOMEM conditions.
	 */
	ch->avail_cnt = 2048;
	return 0;
}

static void
stub_destroy_ch(void *io_device, void *ctx_buf)
{
}

static struct spdk_io_channel *
stub_get_io_channel(void *ctx)
{
	struct ut_bdev *ut_bdev = ctx;

	if (g_get_io_channel == true) {
		return spdk_get_io_channel(ut_bdev->io_target);
	} else {
		return NULL;
	}
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
	struct spdk_bdev_io *io;

	if (bdev_io->type == SPDK_BDEV_IO_TYPE_RESET) {
		while (!TAILQ_EMPTY(&ch->outstanding_io)) {
			io = TAILQ_FIRST(&ch->outstanding_io);
			TAILQ_REMOVE(&ch->outstanding_io, io, module_link);
			ch->outstanding_cnt--;
			spdk_bdev_io_complete(io, SPDK_BDEV_IO_STATUS_ABORTED);
			ch->avail_cnt++;
		}
	} else if (bdev_io->type == SPDK_BDEV_IO_TYPE_ABORT) {
		TAILQ_FOREACH(io, &ch->outstanding_io, module_link) {
			if (io == bdev_io->u.abort.bio_to_abort) {
				TAILQ_REMOVE(&ch->outstanding_io, io, module_link);
				ch->outstanding_cnt--;
				spdk_bdev_io_complete(io, SPDK_BDEV_IO_STATUS_ABORTED);
				ch->avail_cnt++;

				spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
				return;
			}
		}

		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	if (ch->avail_cnt > 0) {
		TAILQ_INSERT_TAIL(&ch->outstanding_io, bdev_io, module_link);
		ch->outstanding_cnt++;
		ch->avail_cnt--;
	} else {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM);
	}
}

static uint32_t
stub_complete_io(void *io_target, uint32_t num_to_complete)
{
	struct spdk_io_channel *_ch = spdk_get_io_channel(io_target);
	struct ut_bdev_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct spdk_bdev_io *io;
	bool complete_all = (num_to_complete == 0);
	uint32_t num_completed = 0;

	while (complete_all || num_completed < num_to_complete) {
		if (TAILQ_EMPTY(&ch->outstanding_io)) {
			break;
		}
		io = TAILQ_FIRST(&ch->outstanding_io);
		TAILQ_REMOVE(&ch->outstanding_io, io, module_link);
		ch->outstanding_cnt--;
		spdk_bdev_io_complete(io, SPDK_BDEV_IO_STATUS_SUCCESS);
		ch->avail_cnt++;
		num_completed++;
	}

	spdk_put_io_channel(_ch);
	return num_completed;
}

static bool
stub_io_type_supported(void *ctx, enum spdk_bdev_io_type type)
{
	return true;
}

static struct spdk_bdev_fn_table fn_table = {
	.get_io_channel =	stub_get_io_channel,
	.destruct =		stub_destruct,
	.submit_request =	stub_submit_request,
	.io_type_supported =	stub_io_type_supported,
};

struct spdk_bdev_module bdev_ut_if;

static int
module_init(void)
{
	spdk_bdev_module_init_done(&bdev_ut_if);
	return 0;
}

static void
module_fini(void)
{
}

static void
init_complete(void)
{
	g_init_complete_called = true;
}

static void
fini_start(void)
{
	g_fini_start_called = true;
}

struct spdk_bdev_module bdev_ut_if = {
	.name = "bdev_ut",
	.module_init = module_init,
	.module_fini = module_fini,
	.async_init = true,
	.init_complete = init_complete,
	.fini_start = fini_start,
};

SPDK_BDEV_MODULE_REGISTER(bdev_ut, &bdev_ut_if)

static void
register_bdev(struct ut_bdev *ut_bdev, char *name, void *io_target)
{
	memset(ut_bdev, 0, sizeof(*ut_bdev));

	ut_bdev->io_target = io_target;
	ut_bdev->bdev.ctxt = ut_bdev;
	ut_bdev->bdev.name = name;
	ut_bdev->bdev.fn_table = &fn_table;
	ut_bdev->bdev.module = &bdev_ut_if;
	ut_bdev->bdev.blocklen = 4096;
	ut_bdev->bdev.blockcnt = 1024;

	spdk_bdev_register(&ut_bdev->bdev);
}

static void
unregister_bdev(struct ut_bdev *ut_bdev)
{
	/* Handle any deferred messages. */
	poll_threads();
	spdk_bdev_unregister(&ut_bdev->bdev, NULL, NULL);
}

static void
bdev_init_cb(void *done, int rc)
{
	CU_ASSERT(rc == 0);
	*(bool *)done = true;
}

static void
_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
	       void *event_ctx)
{
	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		if (event_ctx != NULL) {
			*(bool *)event_ctx = true;
		}
		break;
	default:
		CU_ASSERT(false);
		break;
	}
}

static void
setup_test(void)
{
	bool done = false;

	allocate_cores(BDEV_UT_NUM_THREADS);
	allocate_threads(BDEV_UT_NUM_THREADS);
	set_thread(0);
	spdk_bdev_initialize(bdev_init_cb, &done);
	spdk_io_device_register(&g_io_device, stub_create_ch, stub_destroy_ch,
				sizeof(struct ut_bdev_channel), NULL);
	register_bdev(&g_bdev, "ut_bdev", &g_io_device);
	spdk_bdev_open_ext("ut_bdev", true, _bdev_event_cb, NULL, &g_desc);
}

static void
finish_cb(void *cb_arg)
{
	g_teardown_done = true;
}

static void
teardown_test(void)
{
	set_thread(0);
	g_teardown_done = false;
	spdk_bdev_close(g_desc);
	g_desc = NULL;
	unregister_bdev(&g_bdev);
	spdk_io_device_unregister(&g_io_device, NULL);
	spdk_bdev_finish(finish_cb, NULL);
	poll_threads();
	memset(&g_bdev, 0, sizeof(g_bdev));
	CU_ASSERT(g_teardown_done == true);
	g_teardown_done = false;
	free_threads();
	free_cores();
}

static uint32_t
bdev_io_tailq_cnt(bdev_io_tailq_t *tailq)
{
	struct spdk_bdev_io *io;
	uint32_t cnt = 0;

	TAILQ_FOREACH(io, tailq, internal.link) {
		cnt++;
	}

	return cnt;
}

static void
basic(void)
{
	g_init_complete_called = false;
	setup_test();
	CU_ASSERT(g_init_complete_called == true);

	set_thread(0);

	g_get_io_channel = false;
	g_ut_threads[0].ch = spdk_bdev_get_io_channel(g_desc);
	CU_ASSERT(g_ut_threads[0].ch == NULL);

	g_get_io_channel = true;
	g_create_ch = false;
	g_ut_threads[0].ch = spdk_bdev_get_io_channel(g_desc);
	CU_ASSERT(g_ut_threads[0].ch == NULL);

	g_get_io_channel = true;
	g_create_ch = true;
	g_ut_threads[0].ch = spdk_bdev_get_io_channel(g_desc);
	CU_ASSERT(g_ut_threads[0].ch != NULL);
	spdk_put_io_channel(g_ut_threads[0].ch);

	g_fini_start_called = false;
	teardown_test();
	CU_ASSERT(g_fini_start_called == true);
}

static void
_bdev_unregistered(void *done, int rc)
{
	CU_ASSERT(rc == 0);
	*(bool *)done = true;
}

static void
unregister_and_close(void)
{
	bool done, remove_notify;
	struct spdk_bdev_desc *desc = NULL;

	setup_test();
	set_thread(0);

	/* setup_test() automatically opens the bdev,
	 * but this test needs to do that in a different
	 * way. */
	spdk_bdev_close(g_desc);
	poll_threads();

	/* Try hotremoving a bdev with descriptors which don't provide
	 * any context to the notification callback */
	spdk_bdev_open_ext("ut_bdev", true, _bdev_event_cb, NULL, &desc);
	SPDK_CU_ASSERT_FATAL(desc != NULL);

	/* There is an open descriptor on the device. Unregister it,
	 * which can't proceed until the descriptor is closed. */
	done = false;
	spdk_bdev_unregister(&g_bdev.bdev, _bdev_unregistered, &done);

	/* Poll the threads to allow all events to be processed */
	poll_threads();

	/* Make sure the bdev was not unregistered. We still have a
	 * descriptor open */
	CU_ASSERT(done == false);

	spdk_bdev_close(desc);
	poll_threads();
	desc = NULL;

	/* The unregister should have completed */
	CU_ASSERT(done == true);


	/* Register the bdev again */
	register_bdev(&g_bdev, "ut_bdev", &g_io_device);

	remove_notify = false;
	spdk_bdev_open_ext("ut_bdev", true, _bdev_event_cb, &remove_notify, &desc);
	SPDK_CU_ASSERT_FATAL(desc != NULL);
	CU_ASSERT(remove_notify == false);

	/* There is an open descriptor on the device. Unregister it,
	 * which can't proceed until the descriptor is closed. */
	done = false;
	spdk_bdev_unregister(&g_bdev.bdev, _bdev_unregistered, &done);
	/* No polling has occurred, so neither of these should execute */
	CU_ASSERT(remove_notify == false);
	CU_ASSERT(done == false);

	/* Prior to the unregister completing, close the descriptor */
	spdk_bdev_close(desc);

	/* Poll the threads to allow all events to be processed */
	poll_threads();

	/* Remove notify should not have been called because the
	 * descriptor is already closed. */
	CU_ASSERT(remove_notify == false);

	/* The unregister should have completed */
	CU_ASSERT(done == true);

	/* Restore the original g_bdev so that we can use teardown_test(). */
	register_bdev(&g_bdev, "ut_bdev", &g_io_device);
	spdk_bdev_open_ext("ut_bdev", true, _bdev_event_cb, NULL, &g_desc);
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
	stub_complete_io(g_bdev.io_target, 0);

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
	enum spdk_bdev_io_status status1 = SPDK_BDEV_IO_STATUS_PENDING,
				 status2 = SPDK_BDEV_IO_STATUS_PENDING;

	setup_test();

	set_thread(0);
	io_ch[0] = spdk_bdev_get_io_channel(g_desc);
	CU_ASSERT(io_ch[0] != NULL);
	spdk_bdev_reset(g_desc, io_ch[0], aborted_reset_done, &status1);
	poll_threads();
	CU_ASSERT(g_bdev.bdev.internal.reset_in_progress != NULL);

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
	CU_ASSERT(g_bdev.bdev.internal.reset_in_progress != NULL);

	/*
	 * Now destroy ch1.  This will abort the queued reset.  Check that
	 *  the second reset was completed with failed status.  Also check
	 *  that bdev->internal.reset_in_progress != NULL, since the
	 *  original reset has not been completed yet.  This ensures that
	 *  the bdev code is correctly noticing that the failed reset is
	 *  *not* the one that had been submitted to the bdev module.
	 */
	set_thread(1);
	spdk_put_io_channel(io_ch[1]);
	poll_threads();
	CU_ASSERT(status2 == SPDK_BDEV_IO_STATUS_FAILED);
	CU_ASSERT(g_bdev.bdev.internal.reset_in_progress != NULL);

	/*
	 * Now complete the first reset, verify that it completed with SUCCESS
	 *  status and that bdev->internal.reset_in_progress is also set back to NULL.
	 */
	set_thread(0);
	spdk_put_io_channel(io_ch[0]);
	stub_complete_io(g_bdev.io_target, 0);
	poll_threads();
	CU_ASSERT(status1 == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(g_bdev.bdev.internal.reset_in_progress == NULL);

	teardown_test();
}

static void
io_during_io_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	enum spdk_bdev_io_status *status = cb_arg;

	*status = bdev_io->internal.status;
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
	rc = spdk_bdev_read_blocks(g_desc, io_ch[0], NULL, 0, 1, io_during_io_done, &status0);
	CU_ASSERT(rc == 0);

	set_thread(1);
	io_ch[1] = spdk_bdev_get_io_channel(g_desc);
	bdev_ch[1] = spdk_io_channel_get_ctx(io_ch[1]);
	CU_ASSERT(bdev_ch[1]->flags == 0);
	status1 = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_read_blocks(g_desc, io_ch[1], NULL, 0, 1, io_during_io_done, &status1);
	CU_ASSERT(rc == 0);

	poll_threads();
	CU_ASSERT(status0 == SPDK_BDEV_IO_STATUS_PENDING);
	CU_ASSERT(status1 == SPDK_BDEV_IO_STATUS_PENDING);

	set_thread(0);
	stub_complete_io(g_bdev.io_target, 0);
	CU_ASSERT(status0 == SPDK_BDEV_IO_STATUS_SUCCESS);

	set_thread(1);
	stub_complete_io(g_bdev.io_target, 0);
	CU_ASSERT(status1 == SPDK_BDEV_IO_STATUS_SUCCESS);

	/*
	 * Now submit a reset, and leave it pending while we submit I/O on two different
	 *  channels.  These I/O should be failed by the bdev layer since the reset is in
	 *  progress.
	 */
	set_thread(0);
	status_reset = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_reset(g_desc, io_ch[0], io_during_io_done, &status_reset);
	CU_ASSERT(rc == 0);

	CU_ASSERT(bdev_ch[0]->flags == 0);
	CU_ASSERT(bdev_ch[1]->flags == 0);
	poll_threads();
	CU_ASSERT(bdev_ch[0]->flags == BDEV_CH_RESET_IN_PROGRESS);
	CU_ASSERT(bdev_ch[1]->flags == BDEV_CH_RESET_IN_PROGRESS);

	set_thread(0);
	status0 = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_read_blocks(g_desc, io_ch[0], NULL, 0, 1, io_during_io_done, &status0);
	CU_ASSERT(rc == 0);

	set_thread(1);
	status1 = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_read_blocks(g_desc, io_ch[1], NULL, 0, 1, io_during_io_done, &status1);
	CU_ASSERT(rc == 0);

	/*
	 * A reset is in progress so these read I/O should complete with aborted.  Note that we
	 *  need to poll_threads() since I/O completed inline have their completion deferred.
	 */
	poll_threads();
	CU_ASSERT(status_reset == SPDK_BDEV_IO_STATUS_PENDING);
	CU_ASSERT(status0 == SPDK_BDEV_IO_STATUS_ABORTED);
	CU_ASSERT(status1 == SPDK_BDEV_IO_STATUS_ABORTED);

	/*
	 * Complete the reset
	 */
	set_thread(0);
	stub_complete_io(g_bdev.io_target, 0);

	/*
	 * Only poll thread 0. We should not get a completion.
	 */
	poll_thread(0);
	CU_ASSERT(status_reset == SPDK_BDEV_IO_STATUS_PENDING);

	/*
	 * Poll both thread 0 and 1 so the messages can propagate and we
	 * get a completion.
	 */
	poll_threads();
	CU_ASSERT(status_reset == SPDK_BDEV_IO_STATUS_SUCCESS);

	spdk_put_io_channel(io_ch[0]);
	set_thread(1);
	spdk_put_io_channel(io_ch[1]);
	poll_threads();

	teardown_test();
}

static void
basic_qos(void)
{
	struct spdk_io_channel *io_ch[2];
	struct spdk_bdev_channel *bdev_ch[2];
	struct spdk_bdev *bdev;
	enum spdk_bdev_io_status status, abort_status;
	int rc;

	setup_test();

	/* Enable QoS */
	bdev = &g_bdev.bdev;
	bdev->internal.qos = calloc(1, sizeof(*bdev->internal.qos));
	SPDK_CU_ASSERT_FATAL(bdev->internal.qos != NULL);
	TAILQ_INIT(&bdev->internal.qos->queued);
	/*
	 * Enable read/write IOPS, read only byte per second and
	 * read/write byte per second rate limits.
	 * In this case, all rate limits will take equal effect.
	 */
	/* 2000 read/write I/O per second, or 2 per millisecond */
	bdev->internal.qos->rate_limits[SPDK_BDEV_QOS_RW_IOPS_RATE_LIMIT].limit = 2000;
	/* 8K read/write byte per millisecond with 4K block size */
	bdev->internal.qos->rate_limits[SPDK_BDEV_QOS_RW_BPS_RATE_LIMIT].limit = 8192000;
	/* 8K read only byte per millisecond with 4K block size */
	bdev->internal.qos->rate_limits[SPDK_BDEV_QOS_R_BPS_RATE_LIMIT].limit = 8192000;

	g_get_io_channel = true;

	set_thread(0);
	io_ch[0] = spdk_bdev_get_io_channel(g_desc);
	bdev_ch[0] = spdk_io_channel_get_ctx(io_ch[0]);
	CU_ASSERT(bdev_ch[0]->flags == BDEV_CH_QOS_ENABLED);

	set_thread(1);
	io_ch[1] = spdk_bdev_get_io_channel(g_desc);
	bdev_ch[1] = spdk_io_channel_get_ctx(io_ch[1]);
	CU_ASSERT(bdev_ch[1]->flags == BDEV_CH_QOS_ENABLED);

	/*
	 * Send an I/O on thread 0, which is where the QoS thread is running.
	 */
	set_thread(0);
	status = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_read_blocks(g_desc, io_ch[0], NULL, 0, 1, io_during_io_done, &status);
	CU_ASSERT(rc == 0);
	CU_ASSERT(status == SPDK_BDEV_IO_STATUS_PENDING);
	poll_threads();
	stub_complete_io(g_bdev.io_target, 0);
	poll_threads();
	CU_ASSERT(status == SPDK_BDEV_IO_STATUS_SUCCESS);

	/* Send an I/O on thread 1. The QoS thread is not running here. */
	status = SPDK_BDEV_IO_STATUS_PENDING;
	set_thread(1);
	rc = spdk_bdev_read_blocks(g_desc, io_ch[1], NULL, 0, 1, io_during_io_done, &status);
	CU_ASSERT(rc == 0);
	CU_ASSERT(status == SPDK_BDEV_IO_STATUS_PENDING);
	poll_threads();
	/* Complete I/O on thread 1. This should not complete the I/O we submitted */
	stub_complete_io(g_bdev.io_target, 0);
	poll_threads();
	CU_ASSERT(status == SPDK_BDEV_IO_STATUS_PENDING);
	/* Now complete I/O on thread 0 */
	set_thread(0);
	poll_threads();
	stub_complete_io(g_bdev.io_target, 0);
	poll_threads();
	CU_ASSERT(status == SPDK_BDEV_IO_STATUS_SUCCESS);

	/* Reset rate limit for the next test cases. */
	spdk_delay_us(SPDK_BDEV_QOS_TIMESLICE_IN_USEC);
	poll_threads();

	/*
	 * Test abort request when QoS is enabled.
	 */

	/* Send an I/O on thread 0, which is where the QoS thread is running. */
	set_thread(0);
	status = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_read_blocks(g_desc, io_ch[0], NULL, 0, 1, io_during_io_done, &status);
	CU_ASSERT(rc == 0);
	CU_ASSERT(status == SPDK_BDEV_IO_STATUS_PENDING);
	/* Send an abort to the I/O on the same thread. */
	abort_status = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_abort(g_desc, io_ch[0], &status, io_during_io_done, &abort_status);
	CU_ASSERT(rc == 0);
	CU_ASSERT(abort_status == SPDK_BDEV_IO_STATUS_PENDING);
	poll_threads();
	CU_ASSERT(abort_status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(status == SPDK_BDEV_IO_STATUS_ABORTED);

	/* Send an I/O on thread 1. The QoS thread is not running here. */
	status = SPDK_BDEV_IO_STATUS_PENDING;
	set_thread(1);
	rc = spdk_bdev_read_blocks(g_desc, io_ch[1], NULL, 0, 1, io_during_io_done, &status);
	CU_ASSERT(rc == 0);
	CU_ASSERT(status == SPDK_BDEV_IO_STATUS_PENDING);
	poll_threads();
	/* Send an abort to the I/O on the same thread. */
	abort_status = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_abort(g_desc, io_ch[1], &status, io_during_io_done, &abort_status);
	CU_ASSERT(rc == 0);
	CU_ASSERT(abort_status == SPDK_BDEV_IO_STATUS_PENDING);
	poll_threads();
	/* Complete the I/O with failure and the abort with success on thread 1. */
	CU_ASSERT(abort_status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(status == SPDK_BDEV_IO_STATUS_ABORTED);

	set_thread(0);

	/*
	 * Close the descriptor only, which should stop the qos channel as
	 * the last descriptor removed.
	 */
	spdk_bdev_close(g_desc);
	poll_threads();
	CU_ASSERT(bdev->internal.qos->ch == NULL);

	/*
	 * Open the bdev again which shall setup the qos channel as the
	 * channels are valid.
	 */
	spdk_bdev_open_ext("ut_bdev", true, _bdev_event_cb, NULL, &g_desc);
	poll_threads();
	CU_ASSERT(bdev->internal.qos->ch != NULL);

	/* Tear down the channels */
	set_thread(0);
	spdk_put_io_channel(io_ch[0]);
	set_thread(1);
	spdk_put_io_channel(io_ch[1]);
	poll_threads();
	set_thread(0);

	/* Close the descriptor, which should stop the qos channel */
	spdk_bdev_close(g_desc);
	poll_threads();
	CU_ASSERT(bdev->internal.qos->ch == NULL);

	/* Open the bdev again, no qos channel setup without valid channels. */
	spdk_bdev_open_ext("ut_bdev", true, _bdev_event_cb, NULL, &g_desc);
	poll_threads();
	CU_ASSERT(bdev->internal.qos->ch == NULL);

	/* Create the channels in reverse order. */
	set_thread(1);
	io_ch[1] = spdk_bdev_get_io_channel(g_desc);
	bdev_ch[1] = spdk_io_channel_get_ctx(io_ch[1]);
	CU_ASSERT(bdev_ch[1]->flags == BDEV_CH_QOS_ENABLED);

	set_thread(0);
	io_ch[0] = spdk_bdev_get_io_channel(g_desc);
	bdev_ch[0] = spdk_io_channel_get_ctx(io_ch[0]);
	CU_ASSERT(bdev_ch[0]->flags == BDEV_CH_QOS_ENABLED);

	/* Confirm that the qos thread is now thread 1 */
	CU_ASSERT(bdev->internal.qos->ch == bdev_ch[1]);

	/* Tear down the channels */
	set_thread(0);
	spdk_put_io_channel(io_ch[0]);
	set_thread(1);
	spdk_put_io_channel(io_ch[1]);
	poll_threads();

	set_thread(0);

	teardown_test();
}

static void
io_during_qos_queue(void)
{
	struct spdk_io_channel *io_ch[2];
	struct spdk_bdev_channel *bdev_ch[2];
	struct spdk_bdev *bdev;
	enum spdk_bdev_io_status status0, status1, status2;
	int rc;

	setup_test();
	MOCK_SET(spdk_get_ticks, 0);

	/* Enable QoS */
	bdev = &g_bdev.bdev;
	bdev->internal.qos = calloc(1, sizeof(*bdev->internal.qos));
	SPDK_CU_ASSERT_FATAL(bdev->internal.qos != NULL);
	TAILQ_INIT(&bdev->internal.qos->queued);
	/*
	 * Enable read/write IOPS, read only byte per sec, write only
	 * byte per sec and read/write byte per sec rate limits.
	 * In this case, both read only and write only byte per sec
	 * rate limit will take effect.
	 */
	/* 4000 read/write I/O per second, or 4 per millisecond */
	bdev->internal.qos->rate_limits[SPDK_BDEV_QOS_RW_IOPS_RATE_LIMIT].limit = 4000;
	/* 8K byte per millisecond with 4K block size */
	bdev->internal.qos->rate_limits[SPDK_BDEV_QOS_RW_BPS_RATE_LIMIT].limit = 8192000;
	/* 4K byte per millisecond with 4K block size */
	bdev->internal.qos->rate_limits[SPDK_BDEV_QOS_R_BPS_RATE_LIMIT].limit = 4096000;
	/* 4K byte per millisecond with 4K block size */
	bdev->internal.qos->rate_limits[SPDK_BDEV_QOS_W_BPS_RATE_LIMIT].limit = 4096000;

	g_get_io_channel = true;

	/* Create channels */
	set_thread(0);
	io_ch[0] = spdk_bdev_get_io_channel(g_desc);
	bdev_ch[0] = spdk_io_channel_get_ctx(io_ch[0]);
	CU_ASSERT(bdev_ch[0]->flags == BDEV_CH_QOS_ENABLED);

	set_thread(1);
	io_ch[1] = spdk_bdev_get_io_channel(g_desc);
	bdev_ch[1] = spdk_io_channel_get_ctx(io_ch[1]);
	CU_ASSERT(bdev_ch[1]->flags == BDEV_CH_QOS_ENABLED);

	/* Send two read I/Os */
	status1 = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_read_blocks(g_desc, io_ch[1], NULL, 0, 1, io_during_io_done, &status1);
	CU_ASSERT(rc == 0);
	CU_ASSERT(status1 == SPDK_BDEV_IO_STATUS_PENDING);
	set_thread(0);
	status0 = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_read_blocks(g_desc, io_ch[0], NULL, 0, 1, io_during_io_done, &status0);
	CU_ASSERT(rc == 0);
	CU_ASSERT(status0 == SPDK_BDEV_IO_STATUS_PENDING);
	/* Send one write I/O */
	status2 = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_write_blocks(g_desc, io_ch[0], NULL, 0, 1, io_during_io_done, &status2);
	CU_ASSERT(rc == 0);
	CU_ASSERT(status2 == SPDK_BDEV_IO_STATUS_PENDING);

	/* Complete any I/O that arrived at the disk */
	poll_threads();
	set_thread(1);
	stub_complete_io(g_bdev.io_target, 0);
	set_thread(0);
	stub_complete_io(g_bdev.io_target, 0);
	poll_threads();

	/* Only one of the two read I/Os should complete. (logical XOR) */
	if (status0 == SPDK_BDEV_IO_STATUS_SUCCESS) {
		CU_ASSERT(status1 == SPDK_BDEV_IO_STATUS_PENDING);
	} else {
		CU_ASSERT(status1 == SPDK_BDEV_IO_STATUS_SUCCESS);
	}
	/* The write I/O should complete. */
	CU_ASSERT(status2 == SPDK_BDEV_IO_STATUS_SUCCESS);

	/* Advance in time by a millisecond */
	spdk_delay_us(1000);

	/* Complete more I/O */
	poll_threads();
	set_thread(1);
	stub_complete_io(g_bdev.io_target, 0);
	set_thread(0);
	stub_complete_io(g_bdev.io_target, 0);
	poll_threads();

	/* Now the second read I/O should be done */
	CU_ASSERT(status0 == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(status1 == SPDK_BDEV_IO_STATUS_SUCCESS);

	/* Tear down the channels */
	set_thread(1);
	spdk_put_io_channel(io_ch[1]);
	set_thread(0);
	spdk_put_io_channel(io_ch[0]);
	poll_threads();

	teardown_test();
}

static void
io_during_qos_reset(void)
{
	struct spdk_io_channel *io_ch[2];
	struct spdk_bdev_channel *bdev_ch[2];
	struct spdk_bdev *bdev;
	enum spdk_bdev_io_status status0, status1, reset_status;
	int rc;

	setup_test();
	MOCK_SET(spdk_get_ticks, 0);

	/* Enable QoS */
	bdev = &g_bdev.bdev;
	bdev->internal.qos = calloc(1, sizeof(*bdev->internal.qos));
	SPDK_CU_ASSERT_FATAL(bdev->internal.qos != NULL);
	TAILQ_INIT(&bdev->internal.qos->queued);
	/*
	 * Enable read/write IOPS, write only byte per sec and
	 * read/write byte per second rate limits.
	 * In this case, read/write byte per second rate limit will
	 * take effect first.
	 */
	/* 2000 read/write I/O per second, or 2 per millisecond */
	bdev->internal.qos->rate_limits[SPDK_BDEV_QOS_RW_IOPS_RATE_LIMIT].limit = 2000;
	/* 4K byte per millisecond with 4K block size */
	bdev->internal.qos->rate_limits[SPDK_BDEV_QOS_RW_BPS_RATE_LIMIT].limit = 4096000;
	/* 8K byte per millisecond with 4K block size */
	bdev->internal.qos->rate_limits[SPDK_BDEV_QOS_W_BPS_RATE_LIMIT].limit = 8192000;

	g_get_io_channel = true;

	/* Create channels */
	set_thread(0);
	io_ch[0] = spdk_bdev_get_io_channel(g_desc);
	bdev_ch[0] = spdk_io_channel_get_ctx(io_ch[0]);
	CU_ASSERT(bdev_ch[0]->flags == BDEV_CH_QOS_ENABLED);

	set_thread(1);
	io_ch[1] = spdk_bdev_get_io_channel(g_desc);
	bdev_ch[1] = spdk_io_channel_get_ctx(io_ch[1]);
	CU_ASSERT(bdev_ch[1]->flags == BDEV_CH_QOS_ENABLED);

	/* Send two I/O. One of these gets queued by QoS. The other is sitting at the disk. */
	status1 = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_write_blocks(g_desc, io_ch[1], NULL, 0, 1, io_during_io_done, &status1);
	CU_ASSERT(rc == 0);
	set_thread(0);
	status0 = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_write_blocks(g_desc, io_ch[0], NULL, 0, 1, io_during_io_done, &status0);
	CU_ASSERT(rc == 0);

	poll_threads();
	CU_ASSERT(status1 == SPDK_BDEV_IO_STATUS_PENDING);
	CU_ASSERT(status0 == SPDK_BDEV_IO_STATUS_PENDING);

	/* Reset the bdev. */
	reset_status = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_reset(g_desc, io_ch[0], io_during_io_done, &reset_status);
	CU_ASSERT(rc == 0);

	/* Complete any I/O that arrived at the disk */
	poll_threads();
	set_thread(1);
	stub_complete_io(g_bdev.io_target, 0);
	set_thread(0);
	stub_complete_io(g_bdev.io_target, 0);
	poll_threads();

	CU_ASSERT(reset_status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(status0 == SPDK_BDEV_IO_STATUS_ABORTED);
	CU_ASSERT(status1 == SPDK_BDEV_IO_STATUS_ABORTED);

	/* Tear down the channels */
	set_thread(1);
	spdk_put_io_channel(io_ch[1]);
	set_thread(0);
	spdk_put_io_channel(io_ch[0]);
	poll_threads();

	teardown_test();
}

static void
enomem_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	enum spdk_bdev_io_status *status = cb_arg;

	*status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;
	spdk_bdev_free_io(bdev_io);
}

static void
enomem(void)
{
	struct spdk_io_channel *io_ch;
	struct spdk_bdev_channel *bdev_ch;
	struct spdk_bdev_shared_resource *shared_resource;
	struct ut_bdev_channel *ut_ch;
	const uint32_t IO_ARRAY_SIZE = 64;
	const uint32_t AVAIL = 20;
	enum spdk_bdev_io_status status[IO_ARRAY_SIZE], status_reset;
	uint32_t nomem_cnt, i;
	struct spdk_bdev_io *first_io;
	int rc;

	setup_test();

	set_thread(0);
	io_ch = spdk_bdev_get_io_channel(g_desc);
	bdev_ch = spdk_io_channel_get_ctx(io_ch);
	shared_resource = bdev_ch->shared_resource;
	ut_ch = spdk_io_channel_get_ctx(bdev_ch->channel);
	ut_ch->avail_cnt = AVAIL;

	/* First submit a number of IOs equal to what the channel can support. */
	for (i = 0; i < AVAIL; i++) {
		status[i] = SPDK_BDEV_IO_STATUS_PENDING;
		rc = spdk_bdev_read_blocks(g_desc, io_ch, NULL, 0, 1, enomem_done, &status[i]);
		CU_ASSERT(rc == 0);
	}
	CU_ASSERT(TAILQ_EMPTY(&shared_resource->nomem_io));

	/*
	 * Next, submit one additional I/O.  This one should fail with ENOMEM and then go onto
	 *  the enomem_io list.
	 */
	status[AVAIL] = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_read_blocks(g_desc, io_ch, NULL, 0, 1, enomem_done, &status[AVAIL]);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(!TAILQ_EMPTY(&shared_resource->nomem_io));
	first_io = TAILQ_FIRST(&shared_resource->nomem_io);

	/*
	 * Now submit a bunch more I/O.  These should all fail with ENOMEM and get queued behind
	 *  the first_io above.
	 */
	for (i = AVAIL + 1; i < IO_ARRAY_SIZE; i++) {
		status[i] = SPDK_BDEV_IO_STATUS_PENDING;
		rc = spdk_bdev_read_blocks(g_desc, io_ch, NULL, 0, 1, enomem_done, &status[i]);
		CU_ASSERT(rc == 0);
	}

	/* Assert that first_io is still at the head of the list. */
	CU_ASSERT(TAILQ_FIRST(&shared_resource->nomem_io) == first_io);
	CU_ASSERT(bdev_io_tailq_cnt(&shared_resource->nomem_io) == (IO_ARRAY_SIZE - AVAIL));
	nomem_cnt = bdev_io_tailq_cnt(&shared_resource->nomem_io);
	CU_ASSERT(shared_resource->nomem_threshold == (AVAIL - NOMEM_THRESHOLD_COUNT));

	/*
	 * Complete 1 I/O only.  The key check here is bdev_io_tailq_cnt - this should not have
	 *  changed since completing just 1 I/O should not trigger retrying the queued nomem_io
	 *  list.
	 */
	stub_complete_io(g_bdev.io_target, 1);
	CU_ASSERT(bdev_io_tailq_cnt(&shared_resource->nomem_io) == nomem_cnt);

	/*
	 * Complete enough I/O to hit the nomem_theshold.  This should trigger retrying nomem_io,
	 *  and we should see I/O get resubmitted to the test bdev module.
	 */
	stub_complete_io(g_bdev.io_target, NOMEM_THRESHOLD_COUNT - 1);
	CU_ASSERT(bdev_io_tailq_cnt(&shared_resource->nomem_io) < nomem_cnt);
	nomem_cnt = bdev_io_tailq_cnt(&shared_resource->nomem_io);

	/* Complete 1 I/O only.  This should not trigger retrying the queued nomem_io. */
	stub_complete_io(g_bdev.io_target, 1);
	CU_ASSERT(bdev_io_tailq_cnt(&shared_resource->nomem_io) == nomem_cnt);

	/*
	 * Send a reset and confirm that all I/O are completed, including the ones that
	 *  were queued on the nomem_io list.
	 */
	status_reset = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_reset(g_desc, io_ch, enomem_done, &status_reset);
	poll_threads();
	CU_ASSERT(rc == 0);
	/* This will complete the reset. */
	stub_complete_io(g_bdev.io_target, 0);

	CU_ASSERT(bdev_io_tailq_cnt(&shared_resource->nomem_io) == 0);
	CU_ASSERT(shared_resource->io_outstanding == 0);

	spdk_put_io_channel(io_ch);
	poll_threads();
	teardown_test();
}

static void
enomem_multi_bdev(void)
{
	struct spdk_io_channel *io_ch;
	struct spdk_bdev_channel *bdev_ch;
	struct spdk_bdev_shared_resource *shared_resource;
	struct ut_bdev_channel *ut_ch;
	const uint32_t IO_ARRAY_SIZE = 64;
	const uint32_t AVAIL = 20;
	enum spdk_bdev_io_status status[IO_ARRAY_SIZE];
	uint32_t i;
	struct ut_bdev *second_bdev;
	struct spdk_bdev_desc *second_desc = NULL;
	struct spdk_bdev_channel *second_bdev_ch;
	struct spdk_io_channel *second_ch;
	int rc;

	setup_test();

	/* Register second bdev with the same io_target  */
	second_bdev = calloc(1, sizeof(*second_bdev));
	SPDK_CU_ASSERT_FATAL(second_bdev != NULL);
	register_bdev(second_bdev, "ut_bdev2", g_bdev.io_target);
	spdk_bdev_open_ext("ut_bdev2", true, _bdev_event_cb, NULL, &second_desc);
	SPDK_CU_ASSERT_FATAL(second_desc != NULL);

	set_thread(0);
	io_ch = spdk_bdev_get_io_channel(g_desc);
	bdev_ch = spdk_io_channel_get_ctx(io_ch);
	shared_resource = bdev_ch->shared_resource;
	ut_ch = spdk_io_channel_get_ctx(bdev_ch->channel);
	ut_ch->avail_cnt = AVAIL;

	second_ch = spdk_bdev_get_io_channel(second_desc);
	second_bdev_ch = spdk_io_channel_get_ctx(second_ch);
	SPDK_CU_ASSERT_FATAL(shared_resource == second_bdev_ch->shared_resource);

	/* Saturate io_target through bdev A. */
	for (i = 0; i < AVAIL; i++) {
		status[i] = SPDK_BDEV_IO_STATUS_PENDING;
		rc = spdk_bdev_read_blocks(g_desc, io_ch, NULL, 0, 1, enomem_done, &status[i]);
		CU_ASSERT(rc == 0);
	}
	CU_ASSERT(TAILQ_EMPTY(&shared_resource->nomem_io));

	/*
	 * Now submit I/O through the second bdev. This should fail with ENOMEM
	 * and then go onto the nomem_io list.
	 */
	status[AVAIL] = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_read_blocks(second_desc, second_ch, NULL, 0, 1, enomem_done, &status[AVAIL]);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(!TAILQ_EMPTY(&shared_resource->nomem_io));

	/* Complete first bdev's I/O. This should retry sending second bdev's nomem_io */
	stub_complete_io(g_bdev.io_target, AVAIL);

	SPDK_CU_ASSERT_FATAL(TAILQ_EMPTY(&shared_resource->nomem_io));
	CU_ASSERT(shared_resource->io_outstanding == 1);

	/* Now complete our retried I/O  */
	stub_complete_io(g_bdev.io_target, 1);
	SPDK_CU_ASSERT_FATAL(shared_resource->io_outstanding == 0);

	spdk_put_io_channel(io_ch);
	spdk_put_io_channel(second_ch);
	spdk_bdev_close(second_desc);
	unregister_bdev(second_bdev);
	poll_threads();
	free(second_bdev);
	teardown_test();
}


static void
enomem_multi_io_target(void)
{
	struct spdk_io_channel *io_ch;
	struct spdk_bdev_channel *bdev_ch;
	struct ut_bdev_channel *ut_ch;
	const uint32_t IO_ARRAY_SIZE = 64;
	const uint32_t AVAIL = 20;
	enum spdk_bdev_io_status status[IO_ARRAY_SIZE];
	uint32_t i;
	int new_io_device;
	struct ut_bdev *second_bdev;
	struct spdk_bdev_desc *second_desc = NULL;
	struct spdk_bdev_channel *second_bdev_ch;
	struct spdk_io_channel *second_ch;
	int rc;

	setup_test();

	/* Create new io_target and a second bdev using it */
	spdk_io_device_register(&new_io_device, stub_create_ch, stub_destroy_ch,
				sizeof(struct ut_bdev_channel), NULL);
	second_bdev = calloc(1, sizeof(*second_bdev));
	SPDK_CU_ASSERT_FATAL(second_bdev != NULL);
	register_bdev(second_bdev, "ut_bdev2", &new_io_device);
	spdk_bdev_open_ext("ut_bdev2", true, _bdev_event_cb, NULL, &second_desc);
	SPDK_CU_ASSERT_FATAL(second_desc != NULL);

	set_thread(0);
	io_ch = spdk_bdev_get_io_channel(g_desc);
	bdev_ch = spdk_io_channel_get_ctx(io_ch);
	ut_ch = spdk_io_channel_get_ctx(bdev_ch->channel);
	ut_ch->avail_cnt = AVAIL;

	/* Different io_target should imply a different shared_resource */
	second_ch = spdk_bdev_get_io_channel(second_desc);
	second_bdev_ch = spdk_io_channel_get_ctx(second_ch);
	SPDK_CU_ASSERT_FATAL(bdev_ch->shared_resource != second_bdev_ch->shared_resource);

	/* Saturate io_target through bdev A. */
	for (i = 0; i < AVAIL; i++) {
		status[i] = SPDK_BDEV_IO_STATUS_PENDING;
		rc = spdk_bdev_read_blocks(g_desc, io_ch, NULL, 0, 1, enomem_done, &status[i]);
		CU_ASSERT(rc == 0);
	}
	CU_ASSERT(TAILQ_EMPTY(&bdev_ch->shared_resource->nomem_io));

	/* Issue one more I/O to fill ENOMEM list. */
	status[AVAIL] = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_read_blocks(g_desc, io_ch, NULL, 0, 1, enomem_done, &status[AVAIL]);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(!TAILQ_EMPTY(&bdev_ch->shared_resource->nomem_io));

	/*
	 * Now submit I/O through the second bdev. This should go through and complete
	 * successfully because we're using a different io_device underneath.
	 */
	status[AVAIL] = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_read_blocks(second_desc, second_ch, NULL, 0, 1, enomem_done, &status[AVAIL]);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(TAILQ_EMPTY(&second_bdev_ch->shared_resource->nomem_io));
	stub_complete_io(second_bdev->io_target, 1);

	/* Cleanup; Complete outstanding I/O. */
	stub_complete_io(g_bdev.io_target, AVAIL);
	SPDK_CU_ASSERT_FATAL(TAILQ_EMPTY(&bdev_ch->shared_resource->nomem_io));
	/* Complete the ENOMEM I/O */
	stub_complete_io(g_bdev.io_target, 1);
	CU_ASSERT(bdev_ch->shared_resource->io_outstanding == 0);

	SPDK_CU_ASSERT_FATAL(TAILQ_EMPTY(&bdev_ch->shared_resource->nomem_io));
	CU_ASSERT(bdev_ch->shared_resource->io_outstanding == 0);
	spdk_put_io_channel(io_ch);
	spdk_put_io_channel(second_ch);
	spdk_bdev_close(second_desc);
	unregister_bdev(second_bdev);
	spdk_io_device_unregister(&new_io_device, NULL);
	poll_threads();
	free(second_bdev);
	teardown_test();
}

static void
qos_dynamic_enable_done(void *cb_arg, int status)
{
	int *rc = cb_arg;
	*rc = status;
}

static void
qos_dynamic_enable(void)
{
	struct spdk_io_channel *io_ch[2];
	struct spdk_bdev_channel *bdev_ch[2];
	struct spdk_bdev *bdev;
	enum spdk_bdev_io_status bdev_io_status[2];
	uint64_t limits[SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES] = {};
	int status, second_status, rc, i;

	setup_test();
	MOCK_SET(spdk_get_ticks, 0);

	for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
		limits[i] = UINT64_MAX;
	}

	bdev = &g_bdev.bdev;

	g_get_io_channel = true;

	/* Create channels */
	set_thread(0);
	io_ch[0] = spdk_bdev_get_io_channel(g_desc);
	bdev_ch[0] = spdk_io_channel_get_ctx(io_ch[0]);
	CU_ASSERT(bdev_ch[0]->flags == 0);

	set_thread(1);
	io_ch[1] = spdk_bdev_get_io_channel(g_desc);
	bdev_ch[1] = spdk_io_channel_get_ctx(io_ch[1]);
	CU_ASSERT(bdev_ch[1]->flags == 0);

	set_thread(0);

	/*
	 * Enable QoS: Read/Write IOPS, Read/Write byte,
	 * Read only byte and Write only byte per second
	 * rate limits.
	 * More than 10 I/Os allowed per timeslice.
	 */
	status = -1;
	limits[SPDK_BDEV_QOS_RW_IOPS_RATE_LIMIT] = 10000;
	limits[SPDK_BDEV_QOS_RW_BPS_RATE_LIMIT] = 100;
	limits[SPDK_BDEV_QOS_R_BPS_RATE_LIMIT] = 100;
	limits[SPDK_BDEV_QOS_W_BPS_RATE_LIMIT] = 10;
	spdk_bdev_set_qos_rate_limits(bdev, limits, qos_dynamic_enable_done, &status);
	poll_threads();
	CU_ASSERT(status == 0);
	CU_ASSERT((bdev_ch[0]->flags & BDEV_CH_QOS_ENABLED) != 0);
	CU_ASSERT((bdev_ch[1]->flags & BDEV_CH_QOS_ENABLED) != 0);

	/*
	 * Submit and complete 10 I/O to fill the QoS allotment for this timeslice.
	 * Additional I/O will then be queued.
	 */
	set_thread(0);
	for (i = 0; i < 10; i++) {
		bdev_io_status[0] = SPDK_BDEV_IO_STATUS_PENDING;
		rc = spdk_bdev_read_blocks(g_desc, io_ch[0], NULL, 0, 1, io_during_io_done, &bdev_io_status[0]);
		CU_ASSERT(rc == 0);
		CU_ASSERT(bdev_io_status[0] == SPDK_BDEV_IO_STATUS_PENDING);
		poll_thread(0);
		stub_complete_io(g_bdev.io_target, 0);
		CU_ASSERT(bdev_io_status[0] == SPDK_BDEV_IO_STATUS_SUCCESS);
	}

	/*
	 * Send two more I/O.  These I/O will be queued since the current timeslice allotment has been
	 * filled already.  We want to test that when QoS is disabled that these two I/O:
	 *  1) are not aborted
	 *  2) are sent back to their original thread for resubmission
	 */
	bdev_io_status[0] = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_read_blocks(g_desc, io_ch[0], NULL, 0, 1, io_during_io_done, &bdev_io_status[0]);
	CU_ASSERT(rc == 0);
	CU_ASSERT(bdev_io_status[0] == SPDK_BDEV_IO_STATUS_PENDING);
	set_thread(1);
	bdev_io_status[1] = SPDK_BDEV_IO_STATUS_PENDING;
	rc = spdk_bdev_read_blocks(g_desc, io_ch[1], NULL, 0, 1, io_during_io_done, &bdev_io_status[1]);
	CU_ASSERT(rc == 0);
	CU_ASSERT(bdev_io_status[1] == SPDK_BDEV_IO_STATUS_PENDING);
	poll_threads();

	/*
	 * Disable QoS: Read/Write IOPS, Read/Write byte,
	 * Read only byte rate limits
	 */
	status = -1;
	limits[SPDK_BDEV_QOS_RW_IOPS_RATE_LIMIT] = 0;
	limits[SPDK_BDEV_QOS_RW_BPS_RATE_LIMIT] = 0;
	limits[SPDK_BDEV_QOS_R_BPS_RATE_LIMIT] = 0;
	spdk_bdev_set_qos_rate_limits(bdev, limits, qos_dynamic_enable_done, &status);
	poll_threads();
	CU_ASSERT(status == 0);
	CU_ASSERT((bdev_ch[0]->flags & BDEV_CH_QOS_ENABLED) != 0);
	CU_ASSERT((bdev_ch[1]->flags & BDEV_CH_QOS_ENABLED) != 0);

	/* Disable QoS: Write only Byte per second rate limit */
	status = -1;
	limits[SPDK_BDEV_QOS_W_BPS_RATE_LIMIT] = 0;
	spdk_bdev_set_qos_rate_limits(bdev, limits, qos_dynamic_enable_done, &status);
	poll_threads();
	CU_ASSERT(status == 0);
	CU_ASSERT((bdev_ch[0]->flags & BDEV_CH_QOS_ENABLED) == 0);
	CU_ASSERT((bdev_ch[1]->flags & BDEV_CH_QOS_ENABLED) == 0);

	/*
	 * All I/O should have been resubmitted back on their original thread.  Complete
	 *  all I/O on thread 0, and ensure that only the thread 0 I/O was completed.
	 */
	set_thread(0);
	stub_complete_io(g_bdev.io_target, 0);
	poll_threads();
	CU_ASSERT(bdev_io_status[0] == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(bdev_io_status[1] == SPDK_BDEV_IO_STATUS_PENDING);

	/* Now complete all I/O on thread 1 and ensure the thread 1 I/O was completed. */
	set_thread(1);
	stub_complete_io(g_bdev.io_target, 0);
	poll_threads();
	CU_ASSERT(bdev_io_status[1] == SPDK_BDEV_IO_STATUS_SUCCESS);

	/* Disable QoS again */
	status = -1;
	limits[SPDK_BDEV_QOS_RW_IOPS_RATE_LIMIT] = 0;
	spdk_bdev_set_qos_rate_limits(bdev, limits, qos_dynamic_enable_done, &status);
	poll_threads();
	CU_ASSERT(status == 0); /* This should succeed */
	CU_ASSERT((bdev_ch[0]->flags & BDEV_CH_QOS_ENABLED) == 0);
	CU_ASSERT((bdev_ch[1]->flags & BDEV_CH_QOS_ENABLED) == 0);

	/* Enable QoS on thread 0 */
	status = -1;
	limits[SPDK_BDEV_QOS_RW_IOPS_RATE_LIMIT] = 10000;
	spdk_bdev_set_qos_rate_limits(bdev, limits, qos_dynamic_enable_done, &status);
	poll_threads();
	CU_ASSERT(status == 0);
	CU_ASSERT((bdev_ch[0]->flags & BDEV_CH_QOS_ENABLED) != 0);
	CU_ASSERT((bdev_ch[1]->flags & BDEV_CH_QOS_ENABLED) != 0);

	/* Disable QoS on thread 1 */
	set_thread(1);
	status = -1;
	limits[SPDK_BDEV_QOS_RW_IOPS_RATE_LIMIT] = 0;
	spdk_bdev_set_qos_rate_limits(bdev, limits, qos_dynamic_enable_done, &status);
	/* Don't poll yet. This should leave the channels with QoS enabled */
	CU_ASSERT(status == -1);
	CU_ASSERT((bdev_ch[0]->flags & BDEV_CH_QOS_ENABLED) != 0);
	CU_ASSERT((bdev_ch[1]->flags & BDEV_CH_QOS_ENABLED) != 0);

	/* Enable QoS. This should immediately fail because the previous disable QoS hasn't completed. */
	second_status = 0;
	limits[SPDK_BDEV_QOS_RW_BPS_RATE_LIMIT] = 10;
	spdk_bdev_set_qos_rate_limits(bdev, limits, qos_dynamic_enable_done, &second_status);
	poll_threads();
	CU_ASSERT(status == 0); /* The disable should succeed */
	CU_ASSERT(second_status < 0); /* The enable should fail */
	CU_ASSERT((bdev_ch[0]->flags & BDEV_CH_QOS_ENABLED) == 0);
	CU_ASSERT((bdev_ch[1]->flags & BDEV_CH_QOS_ENABLED) == 0);

	/* Enable QoS on thread 1. This should succeed now that the disable has completed. */
	status = -1;
	limits[SPDK_BDEV_QOS_RW_IOPS_RATE_LIMIT] = 10000;
	spdk_bdev_set_qos_rate_limits(bdev, limits, qos_dynamic_enable_done, &status);
	poll_threads();
	CU_ASSERT(status == 0);
	CU_ASSERT((bdev_ch[0]->flags & BDEV_CH_QOS_ENABLED) != 0);
	CU_ASSERT((bdev_ch[1]->flags & BDEV_CH_QOS_ENABLED) != 0);

	/* Tear down the channels */
	set_thread(0);
	spdk_put_io_channel(io_ch[0]);
	set_thread(1);
	spdk_put_io_channel(io_ch[1]);
	poll_threads();

	set_thread(0);
	teardown_test();
}

static void
histogram_status_cb(void *cb_arg, int status)
{
	g_status = status;
}

static void
histogram_data_cb(void *cb_arg, int status, struct spdk_histogram_data *histogram)
{
	g_status = status;
	g_histogram = histogram;
}

static void
histogram_io_count(void *ctx, uint64_t start, uint64_t end, uint64_t count,
		   uint64_t total, uint64_t so_far)
{
	g_count += count;
}

static void
bdev_histograms_mt(void)
{
	struct spdk_io_channel *ch[2];
	struct spdk_histogram_data *histogram;
	uint8_t buf[4096];
	int status = false;
	int rc;


	setup_test();

	set_thread(0);
	ch[0] = spdk_bdev_get_io_channel(g_desc);
	CU_ASSERT(ch[0] != NULL);

	set_thread(1);
	ch[1] = spdk_bdev_get_io_channel(g_desc);
	CU_ASSERT(ch[1] != NULL);


	/* Enable histogram */
	spdk_bdev_histogram_enable(&g_bdev.bdev, histogram_status_cb, NULL, true);
	poll_threads();
	CU_ASSERT(g_status == 0);
	CU_ASSERT(g_bdev.bdev.internal.histogram_enabled == true);

	/* Allocate histogram */
	histogram = spdk_histogram_data_alloc();

	/* Check if histogram is zeroed */
	spdk_bdev_histogram_get(&g_bdev.bdev, histogram, histogram_data_cb, NULL);
	poll_threads();
	CU_ASSERT(g_status == 0);
	SPDK_CU_ASSERT_FATAL(g_histogram != NULL);

	g_count = 0;
	spdk_histogram_data_iterate(g_histogram, histogram_io_count, NULL);

	CU_ASSERT(g_count == 0);

	set_thread(0);
	rc = spdk_bdev_write_blocks(g_desc, ch[0], &buf, 0, 1, io_during_io_done, &status);
	CU_ASSERT(rc == 0);

	spdk_delay_us(10);
	stub_complete_io(g_bdev.io_target, 1);
	poll_threads();
	CU_ASSERT(status == true);


	set_thread(1);
	rc = spdk_bdev_read_blocks(g_desc, ch[1], &buf, 0, 1, io_during_io_done, &status);
	CU_ASSERT(rc == 0);

	spdk_delay_us(10);
	stub_complete_io(g_bdev.io_target, 1);
	poll_threads();
	CU_ASSERT(status == true);

	set_thread(0);

	/* Check if histogram gathered data from all I/O channels */
	spdk_bdev_histogram_get(&g_bdev.bdev, histogram, histogram_data_cb, NULL);
	poll_threads();
	CU_ASSERT(g_status == 0);
	CU_ASSERT(g_bdev.bdev.internal.histogram_enabled == true);
	SPDK_CU_ASSERT_FATAL(g_histogram != NULL);

	g_count = 0;
	spdk_histogram_data_iterate(g_histogram, histogram_io_count, NULL);
	CU_ASSERT(g_count == 2);

	/* Disable histogram */
	spdk_bdev_histogram_enable(&g_bdev.bdev, histogram_status_cb, NULL, false);
	poll_threads();
	CU_ASSERT(g_status == 0);
	CU_ASSERT(g_bdev.bdev.internal.histogram_enabled == false);

	spdk_histogram_data_free(histogram);

	/* Tear down the channels */
	set_thread(0);
	spdk_put_io_channel(ch[0]);
	set_thread(1);
	spdk_put_io_channel(ch[1]);
	poll_threads();
	set_thread(0);
	teardown_test();

}

struct timeout_io_cb_arg {
	struct iovec iov;
	uint8_t type;
};

static int
bdev_channel_count_submitted_io(struct spdk_bdev_channel *ch)
{
	struct spdk_bdev_io *bdev_io;
	int n = 0;

	if (!ch) {
		return -1;
	}

	TAILQ_FOREACH(bdev_io, &ch->io_submitted, internal.ch_link) {
		n++;
	}

	return n;
}

static void
bdev_channel_io_timeout_cb(void *cb_arg, struct spdk_bdev_io *bdev_io)
{
	struct timeout_io_cb_arg *ctx = cb_arg;

	ctx->type = bdev_io->type;
	ctx->iov.iov_base = bdev_io->iov.iov_base;
	ctx->iov.iov_len = bdev_io->iov.iov_len;
}

static bool g_io_done;

static void
io_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	g_io_done = true;
	spdk_bdev_free_io(bdev_io);
}

static void
bdev_set_io_timeout_mt(void)
{
	struct spdk_io_channel *ch[3];
	struct spdk_bdev_channel *bdev_ch[3];
	struct timeout_io_cb_arg cb_arg;

	setup_test();

	g_bdev.bdev.optimal_io_boundary = 16;
	g_bdev.bdev.split_on_optimal_io_boundary = true;

	set_thread(0);
	ch[0] = spdk_bdev_get_io_channel(g_desc);
	CU_ASSERT(ch[0] != NULL);

	set_thread(1);
	ch[1] = spdk_bdev_get_io_channel(g_desc);
	CU_ASSERT(ch[1] != NULL);

	set_thread(2);
	ch[2] = spdk_bdev_get_io_channel(g_desc);
	CU_ASSERT(ch[2] != NULL);

	/* Multi-thread mode
	 * 1, Check the poller was registered successfully
	 * 2, Check the timeout IO and ensure the IO was the submitted by user
	 * 3, Check the link int the bdev_ch works right.
	 * 4, Close desc and put io channel during the timeout poller is polling
	 */

	/* In desc thread set the timeout */
	set_thread(0);
	CU_ASSERT(spdk_bdev_set_timeout(g_desc, 5, bdev_channel_io_timeout_cb, &cb_arg) == 0);
	CU_ASSERT(g_desc->io_timeout_poller != NULL);
	CU_ASSERT(g_desc->cb_fn == bdev_channel_io_timeout_cb);
	CU_ASSERT(g_desc->cb_arg == &cb_arg);

	/* check the IO submitted list and timeout handler */
	CU_ASSERT(spdk_bdev_read_blocks(g_desc, ch[0], (void *)0x2000, 0, 1, io_done, NULL) == 0);
	bdev_ch[0] = spdk_io_channel_get_ctx(ch[0]);
	CU_ASSERT(bdev_channel_count_submitted_io(bdev_ch[0]) == 1);

	set_thread(1);
	CU_ASSERT(spdk_bdev_write_blocks(g_desc, ch[1], (void *)0x1000, 0, 1, io_done, NULL) == 0);
	bdev_ch[1] = spdk_io_channel_get_ctx(ch[1]);
	CU_ASSERT(bdev_channel_count_submitted_io(bdev_ch[1]) == 1);

	/* Now test that a single-vector command is split correctly.
	 * Offset 14, length 8, payload 0xF000
	 *  Child - Offset 14, length 2, payload 0xF000
	 *  Child - Offset 16, length 6, payload 0xF000 + 2 * 512
	 *
	 * Set up the expected values before calling spdk_bdev_read_blocks
	 */
	set_thread(2);
	CU_ASSERT(spdk_bdev_read_blocks(g_desc, ch[2], (void *)0xF000, 14, 8, io_done, NULL) == 0);
	bdev_ch[2] = spdk_io_channel_get_ctx(ch[2]);
	CU_ASSERT(bdev_channel_count_submitted_io(bdev_ch[2]) == 3);

	set_thread(0);
	memset(&cb_arg, 0, sizeof(cb_arg));
	spdk_delay_us(3 * spdk_get_ticks_hz());
	poll_threads();
	CU_ASSERT(cb_arg.type == 0);
	CU_ASSERT(cb_arg.iov.iov_base == (void *)0x0);
	CU_ASSERT(cb_arg.iov.iov_len == 0);

	/* Now the time reach the limit */
	spdk_delay_us(3 * spdk_get_ticks_hz());
	poll_thread(0);
	CU_ASSERT(cb_arg.type == SPDK_BDEV_IO_TYPE_READ);
	CU_ASSERT(cb_arg.iov.iov_base == (void *)0x2000);
	CU_ASSERT(cb_arg.iov.iov_len == 1 * g_bdev.bdev.blocklen);
	stub_complete_io(g_bdev.io_target, 1);
	CU_ASSERT(bdev_channel_count_submitted_io(bdev_ch[0]) == 0);

	memset(&cb_arg, 0, sizeof(cb_arg));
	set_thread(1);
	poll_thread(1);
	CU_ASSERT(cb_arg.type == SPDK_BDEV_IO_TYPE_WRITE);
	CU_ASSERT(cb_arg.iov.iov_base == (void *)0x1000);
	CU_ASSERT(cb_arg.iov.iov_len == 1 * g_bdev.bdev.blocklen);
	stub_complete_io(g_bdev.io_target, 1);
	CU_ASSERT(bdev_channel_count_submitted_io(bdev_ch[1]) == 0);

	memset(&cb_arg, 0, sizeof(cb_arg));
	set_thread(2);
	poll_thread(2);
	CU_ASSERT(cb_arg.type == SPDK_BDEV_IO_TYPE_READ);
	CU_ASSERT(cb_arg.iov.iov_base == (void *)0xF000);
	CU_ASSERT(cb_arg.iov.iov_len == 8 * g_bdev.bdev.blocklen);
	stub_complete_io(g_bdev.io_target, 1);
	CU_ASSERT(bdev_channel_count_submitted_io(bdev_ch[2]) == 2);
	stub_complete_io(g_bdev.io_target, 1);
	CU_ASSERT(bdev_channel_count_submitted_io(bdev_ch[2]) == 0);

	/* Run poll_timeout_done() it means complete the timeout poller */
	set_thread(0);
	poll_thread(0);
	CU_ASSERT(g_desc->refs == 0);
	CU_ASSERT(spdk_bdev_read_blocks(g_desc, ch[0], (void *)0x1000, 0, 1, io_done, NULL) == 0);
	set_thread(1);
	CU_ASSERT(spdk_bdev_write_blocks(g_desc, ch[1], (void *)0x2000, 0, 2, io_done, NULL) == 0);
	set_thread(2);
	CU_ASSERT(spdk_bdev_read_blocks(g_desc, ch[2], (void *)0x3000, 0, 3, io_done, NULL) == 0);

	/* Trigger timeout poller to run again, desc->refs is incremented.
	 * In thread 0 we destroy the io channel before timeout poller runs.
	 * Timeout callback is not called on thread 0.
	 */
	spdk_delay_us(6 * spdk_get_ticks_hz());
	memset(&cb_arg, 0, sizeof(cb_arg));
	set_thread(0);
	stub_complete_io(g_bdev.io_target, 1);
	spdk_put_io_channel(ch[0]);
	poll_thread(0);
	CU_ASSERT(g_desc->refs == 1)
	CU_ASSERT(cb_arg.type == 0);
	CU_ASSERT(cb_arg.iov.iov_base == (void *)0x0);
	CU_ASSERT(cb_arg.iov.iov_len == 0);

	/* In thread 1 timeout poller runs then we destroy the io channel
	 * Timeout callback is called on thread 1.
	 */
	memset(&cb_arg, 0, sizeof(cb_arg));
	set_thread(1);
	poll_thread(1);
	stub_complete_io(g_bdev.io_target, 1);
	spdk_put_io_channel(ch[1]);
	poll_thread(1);
	CU_ASSERT(cb_arg.type == SPDK_BDEV_IO_TYPE_WRITE);
	CU_ASSERT(cb_arg.iov.iov_base == (void *)0x2000);
	CU_ASSERT(cb_arg.iov.iov_len == 2 * g_bdev.bdev.blocklen);

	/* Close the desc.
	 * Unregister the timeout poller first.
	 * Then decrement desc->refs but it's not zero yet so desc is not freed.
	 */
	set_thread(0);
	spdk_bdev_close(g_desc);
	CU_ASSERT(g_desc->refs == 1);
	CU_ASSERT(g_desc->io_timeout_poller == NULL);

	/* Timeout poller runs on thread 2 then we destroy the io channel.
	 * Desc is closed so we would exit the timeout poller directly.
	 * timeout callback is not called on thread 2.
	 */
	memset(&cb_arg, 0, sizeof(cb_arg));
	set_thread(2);
	poll_thread(2);
	stub_complete_io(g_bdev.io_target, 1);
	spdk_put_io_channel(ch[2]);
	poll_thread(2);
	CU_ASSERT(cb_arg.type == 0);
	CU_ASSERT(cb_arg.iov.iov_base == (void *)0x0);
	CU_ASSERT(cb_arg.iov.iov_len == 0);

	set_thread(0);
	poll_thread(0);
	g_teardown_done = false;
	unregister_bdev(&g_bdev);
	spdk_io_device_unregister(&g_io_device, NULL);
	spdk_bdev_finish(finish_cb, NULL);
	poll_threads();
	memset(&g_bdev, 0, sizeof(g_bdev));
	CU_ASSERT(g_teardown_done == true);
	g_teardown_done = false;
	free_threads();
	free_cores();
}

static bool g_io_done2;
static bool g_lock_lba_range_done;
static bool g_unlock_lba_range_done;

static void
io_done2(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	g_io_done2 = true;
	spdk_bdev_free_io(bdev_io);
}

static void
lock_lba_range_done(void *ctx, int status)
{
	g_lock_lba_range_done = true;
}

static void
unlock_lba_range_done(void *ctx, int status)
{
	g_unlock_lba_range_done = true;
}

static uint32_t
stub_channel_outstanding_cnt(void *io_target)
{
	struct spdk_io_channel *_ch = spdk_get_io_channel(io_target);
	struct ut_bdev_channel *ch = spdk_io_channel_get_ctx(_ch);
	uint32_t outstanding_cnt;

	outstanding_cnt = ch->outstanding_cnt;

	spdk_put_io_channel(_ch);
	return outstanding_cnt;
}

static void
lock_lba_range_then_submit_io(void)
{
	struct spdk_bdev_desc *desc = NULL;
	void *io_target;
	struct spdk_io_channel *io_ch[3];
	struct spdk_bdev_channel *bdev_ch[3];
	struct lba_range *range;
	char buf[4096];
	int ctx0, ctx1, ctx2;
	int rc;

	setup_test();

	io_target = g_bdev.io_target;
	desc = g_desc;

	set_thread(0);
	io_ch[0] = spdk_bdev_get_io_channel(desc);
	bdev_ch[0] = spdk_io_channel_get_ctx(io_ch[0]);
	CU_ASSERT(io_ch[0] != NULL);

	set_thread(1);
	io_ch[1] = spdk_bdev_get_io_channel(desc);
	bdev_ch[1] = spdk_io_channel_get_ctx(io_ch[1]);
	CU_ASSERT(io_ch[1] != NULL);

	set_thread(0);
	g_lock_lba_range_done = false;
	rc = bdev_lock_lba_range(desc, io_ch[0], 20, 10, lock_lba_range_done, &ctx0);
	CU_ASSERT(rc == 0);
	poll_threads();

	/* The lock should immediately become valid, since there are no outstanding
	 * write I/O.
	 */
	CU_ASSERT(g_lock_lba_range_done == true);
	range = TAILQ_FIRST(&bdev_ch[0]->locked_ranges);
	SPDK_CU_ASSERT_FATAL(range != NULL);
	CU_ASSERT(range->offset == 20);
	CU_ASSERT(range->length == 10);
	CU_ASSERT(range->owner_ch == bdev_ch[0]);

	g_io_done = false;
	CU_ASSERT(TAILQ_EMPTY(&bdev_ch[0]->io_locked));
	rc = spdk_bdev_read_blocks(desc, io_ch[0], buf, 20, 1, io_done, &ctx0);
	CU_ASSERT(rc == 0);
	CU_ASSERT(stub_channel_outstanding_cnt(io_target) == 1);

	stub_complete_io(io_target, 1);
	poll_threads();
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(TAILQ_EMPTY(&bdev_ch[0]->io_locked));

	/* Try a write I/O.  This should actually be allowed to execute, since the channel
	 * holding the lock is submitting the write I/O.
	 */
	g_io_done = false;
	CU_ASSERT(TAILQ_EMPTY(&bdev_ch[0]->io_locked));
	rc = spdk_bdev_write_blocks(desc, io_ch[0], buf, 20, 1, io_done, &ctx0);
	CU_ASSERT(rc == 0);
	CU_ASSERT(stub_channel_outstanding_cnt(io_target) == 1);

	stub_complete_io(io_target, 1);
	poll_threads();
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(TAILQ_EMPTY(&bdev_ch[0]->io_locked));

	/* Try a write I/O.  This should get queued in the io_locked tailq. */
	set_thread(1);
	g_io_done = false;
	CU_ASSERT(TAILQ_EMPTY(&bdev_ch[1]->io_locked));
	rc = spdk_bdev_write_blocks(desc, io_ch[1], buf, 20, 1, io_done, &ctx1);
	CU_ASSERT(rc == 0);
	poll_threads();
	CU_ASSERT(stub_channel_outstanding_cnt(io_target) == 0);
	CU_ASSERT(!TAILQ_EMPTY(&bdev_ch[1]->io_locked));
	CU_ASSERT(g_io_done == false);

	/* Try to unlock the lba range using thread 1's io_ch.  This should fail. */
	rc = bdev_unlock_lba_range(desc, io_ch[1], 20, 10, unlock_lba_range_done, &ctx1);
	CU_ASSERT(rc == -EINVAL);

	/* Now create a new channel and submit a write I/O with it.  This should also be queued.
	 * The new channel should inherit the active locks from the bdev's internal list.
	 */
	set_thread(2);
	io_ch[2] = spdk_bdev_get_io_channel(desc);
	bdev_ch[2] = spdk_io_channel_get_ctx(io_ch[2]);
	CU_ASSERT(io_ch[2] != NULL);

	g_io_done2 = false;
	CU_ASSERT(TAILQ_EMPTY(&bdev_ch[2]->io_locked));
	rc = spdk_bdev_write_blocks(desc, io_ch[2], buf, 22, 2, io_done2, &ctx2);
	CU_ASSERT(rc == 0);
	poll_threads();
	CU_ASSERT(stub_channel_outstanding_cnt(io_target) == 0);
	CU_ASSERT(!TAILQ_EMPTY(&bdev_ch[2]->io_locked));
	CU_ASSERT(g_io_done2 == false);

	set_thread(0);
	rc = bdev_unlock_lba_range(desc, io_ch[0], 20, 10, unlock_lba_range_done, &ctx0);
	CU_ASSERT(rc == 0);
	poll_threads();
	CU_ASSERT(TAILQ_EMPTY(&bdev_ch[0]->locked_ranges));

	/* The LBA range is unlocked, so the write IOs should now have started execution. */
	CU_ASSERT(TAILQ_EMPTY(&bdev_ch[1]->io_locked));
	CU_ASSERT(TAILQ_EMPTY(&bdev_ch[2]->io_locked));

	set_thread(1);
	CU_ASSERT(stub_channel_outstanding_cnt(io_target) == 1);
	stub_complete_io(io_target, 1);
	set_thread(2);
	CU_ASSERT(stub_channel_outstanding_cnt(io_target) == 1);
	stub_complete_io(io_target, 1);

	poll_threads();
	CU_ASSERT(g_io_done == true);
	CU_ASSERT(g_io_done2 == true);

	/* Tear down the channels */
	set_thread(0);
	spdk_put_io_channel(io_ch[0]);
	set_thread(1);
	spdk_put_io_channel(io_ch[1]);
	set_thread(2);
	spdk_put_io_channel(io_ch[2]);
	poll_threads();
	set_thread(0);
	teardown_test();
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("bdev", NULL, NULL);

	CU_ADD_TEST(suite, basic);
	CU_ADD_TEST(suite, unregister_and_close);
	CU_ADD_TEST(suite, basic_qos);
	CU_ADD_TEST(suite, put_channel_during_reset);
	CU_ADD_TEST(suite, aborted_reset);
	CU_ADD_TEST(suite, io_during_reset);
	CU_ADD_TEST(suite, io_during_qos_queue);
	CU_ADD_TEST(suite, io_during_qos_reset);
	CU_ADD_TEST(suite, enomem);
	CU_ADD_TEST(suite, enomem_multi_bdev);
	CU_ADD_TEST(suite, enomem_multi_io_target);
	CU_ADD_TEST(suite, qos_dynamic_enable);
	CU_ADD_TEST(suite, bdev_histograms_mt);
	CU_ADD_TEST(suite, bdev_set_io_timeout_mt);
	CU_ADD_TEST(suite, lock_lba_range_then_submit_io);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
