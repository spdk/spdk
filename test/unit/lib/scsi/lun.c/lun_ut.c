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

#include "scsi/task.c"
#include "scsi/lun.c"

#include "spdk_internal/mock.h"
/* These unit tests aren't multithreads, but we need to allocate threads since
 * the lun.c code will register pollers.
 */
#include "common/lib/ut_multithread.c"

/* Unit test bdev mockup */
struct spdk_bdev {
	int x;
};

SPDK_LOG_REGISTER_COMPONENT(scsi)

static bool g_lun_execute_fail = false;
static int g_lun_execute_status = SPDK_SCSI_TASK_PENDING;
static uint32_t g_task_count = 0;

DEFINE_STUB(bdev_scsi_get_dif_ctx, bool,
	    (struct spdk_bdev *bdev, struct spdk_scsi_task *task,
	     struct spdk_dif_ctx *dif_ctx), false);

static void
spdk_lun_ut_cpl_task(struct spdk_scsi_task *task)
{
	SPDK_CU_ASSERT_FATAL(g_task_count > 0);
	g_task_count--;
}

static void
spdk_lun_ut_free_task(struct spdk_scsi_task *task)
{
}

static void
ut_init_task(struct spdk_scsi_task *task)
{
	memset(task, 0, sizeof(*task));
	spdk_scsi_task_construct(task, spdk_lun_ut_cpl_task,
				 spdk_lun_ut_free_task);
	g_task_count++;
}

void
spdk_bdev_free_io(struct spdk_bdev_io *bdev_io)
{
	CU_ASSERT(0);
}

DEFINE_STUB(spdk_bdev_open_ext, int,
	    (const char *bdev_name, bool write, spdk_bdev_event_cb_t event_cb,
	     void *event_ctx, struct spdk_bdev_desc **desc),
	    0);

DEFINE_STUB_V(spdk_bdev_close, (struct spdk_bdev_desc *desc));

DEFINE_STUB(spdk_bdev_get_name, const char *,
	    (const struct spdk_bdev *bdev), "test");

DEFINE_STUB(spdk_bdev_desc_get_bdev, struct spdk_bdev *,
	    (struct spdk_bdev_desc *bdev_desc), NULL);

DEFINE_STUB_V(spdk_scsi_dev_queue_mgmt_task,
	      (struct spdk_scsi_dev *dev, struct spdk_scsi_task *task));

DEFINE_STUB_V(spdk_scsi_dev_delete_lun,
	      (struct spdk_scsi_dev *dev, struct spdk_scsi_lun *lun));

DEFINE_STUB(scsi_pr_check, int, (struct spdk_scsi_task *task), 0);
DEFINE_STUB(scsi2_reserve_check, int, (struct spdk_scsi_task *task), 0);

void
bdev_scsi_reset(struct spdk_scsi_task *task)
{
	task->status = SPDK_SCSI_STATUS_GOOD;
	task->response = SPDK_SCSI_TASK_MGMT_RESP_SUCCESS;

	scsi_lun_complete_reset_task(task->lun, task);
}

int
bdev_scsi_execute(struct spdk_scsi_task *task)
{
	if (g_lun_execute_fail) {
		return -EINVAL;
	} else {
		task->status = SPDK_SCSI_STATUS_GOOD;

		if (g_lun_execute_status == SPDK_SCSI_TASK_PENDING) {
			return g_lun_execute_status;
		} else if (g_lun_execute_status == SPDK_SCSI_TASK_COMPLETE) {
			return g_lun_execute_status;
		} else {
			return 0;
		}
	}
}

DEFINE_STUB(spdk_bdev_get_io_channel, struct spdk_io_channel *,
	    (struct spdk_bdev_desc *desc), NULL);

static struct spdk_scsi_lun *lun_construct(void)
{
	struct spdk_scsi_lun		*lun;

	lun = scsi_lun_construct("ut_bdev", NULL, NULL, NULL, NULL);

	SPDK_CU_ASSERT_FATAL(lun != NULL);
	return lun;
}

static void
lun_destruct(struct spdk_scsi_lun *lun)
{
	/* LUN will defer its removal if there are any unfinished tasks */
	SPDK_CU_ASSERT_FATAL(TAILQ_EMPTY(&lun->tasks));

	scsi_lun_destruct(lun);
}

static void
lun_task_mgmt_execute_abort_task_not_supported(void)
{
	struct spdk_scsi_lun *lun;
	struct spdk_scsi_task task = { 0 };
	struct spdk_scsi_task mgmt_task = { 0 };
	struct spdk_scsi_port initiator_port = { 0 };
	struct spdk_scsi_dev dev = { 0 };
	uint8_t cdb[6] = { 0 };

	lun = lun_construct();
	lun->dev = &dev;

	ut_init_task(&mgmt_task);
	mgmt_task.lun = lun;
	mgmt_task.initiator_port = &initiator_port;
	mgmt_task.function = SPDK_SCSI_TASK_FUNC_ABORT_TASK;

	/* Params to add regular task to the lun->tasks */
	ut_init_task(&task);
	task.lun = lun;
	task.cdb = cdb;

	scsi_lun_execute_task(lun, &task);

	/* task should now be on the tasks list */
	CU_ASSERT(!TAILQ_EMPTY(&lun->tasks));

	scsi_lun_execute_mgmt_task(lun, &mgmt_task);

	/* task abort is not supported */
	CU_ASSERT(mgmt_task.response == SPDK_SCSI_TASK_MGMT_RESP_REJECT_FUNC_NOT_SUPPORTED);

	/* task is still on the tasks list */
	CU_ASSERT_EQUAL(g_task_count, 1);

	scsi_lun_complete_task(lun, &task);
	CU_ASSERT_EQUAL(g_task_count, 0);

	lun_destruct(lun);
}

static void
lun_task_mgmt_execute_abort_task_all_not_supported(void)
{
	struct spdk_scsi_lun *lun;
	struct spdk_scsi_task task = { 0 };
	struct spdk_scsi_task mgmt_task = { 0 };
	struct spdk_scsi_port initiator_port = { 0 };
	struct spdk_scsi_dev dev = { 0 };
	uint8_t cdb[6] = { 0 };

	lun = lun_construct();
	lun->dev = &dev;

	ut_init_task(&mgmt_task);
	mgmt_task.lun = lun;
	mgmt_task.initiator_port = &initiator_port;
	mgmt_task.function = SPDK_SCSI_TASK_FUNC_ABORT_TASK_SET;

	/* Params to add regular task to the lun->tasks */
	ut_init_task(&task);
	task.initiator_port = &initiator_port;
	task.lun = lun;
	task.cdb = cdb;

	scsi_lun_execute_task(lun, &task);

	/* task should now be on the tasks list */
	CU_ASSERT(!TAILQ_EMPTY(&lun->tasks));

	scsi_lun_execute_mgmt_task(lun, &mgmt_task);

	/* task abort is not supported */
	CU_ASSERT(mgmt_task.response == SPDK_SCSI_TASK_MGMT_RESP_REJECT_FUNC_NOT_SUPPORTED);

	/* task is still on the tasks list */
	CU_ASSERT_EQUAL(g_task_count, 1);

	scsi_lun_complete_task(lun, &task);

	CU_ASSERT_EQUAL(g_task_count, 0);

	lun_destruct(lun);
}

static void
lun_task_mgmt_execute_lun_reset(void)
{
	struct spdk_scsi_lun *lun;
	struct spdk_scsi_task mgmt_task = { 0 };
	struct spdk_scsi_dev dev = { 0 };

	lun = lun_construct();
	lun->dev = &dev;

	ut_init_task(&mgmt_task);
	mgmt_task.lun = lun;
	mgmt_task.function = SPDK_SCSI_TASK_FUNC_LUN_RESET;

	scsi_lun_execute_mgmt_task(lun, &mgmt_task);

	/* Returns success */
	CU_ASSERT_EQUAL(mgmt_task.status, SPDK_SCSI_STATUS_GOOD);
	CU_ASSERT_EQUAL(mgmt_task.response, SPDK_SCSI_TASK_MGMT_RESP_SUCCESS);

	lun_destruct(lun);

	CU_ASSERT_EQUAL(g_task_count, 0);
}

static void
lun_task_mgmt_execute_invalid_case(void)
{
	struct spdk_scsi_lun *lun;
	struct spdk_scsi_task mgmt_task = { 0 };
	struct spdk_scsi_dev dev = { 0 };

	lun = lun_construct();
	lun->dev = &dev;

	ut_init_task(&mgmt_task);
	mgmt_task.function = 5;

	/* Pass an invalid value to the switch statement */
	scsi_lun_execute_mgmt_task(lun, &mgmt_task);

	/* function code is invalid */
	CU_ASSERT_EQUAL(mgmt_task.response, SPDK_SCSI_TASK_MGMT_RESP_REJECT_FUNC_NOT_SUPPORTED);

	lun_destruct(lun);

	CU_ASSERT_EQUAL(g_task_count, 0);
}

static void
lun_append_task_null_lun_task_cdb_spc_inquiry(void)
{
	struct spdk_scsi_task task = { 0 };
	uint8_t cdb[6] = { 0 };

	ut_init_task(&task);
	task.cdb = cdb;
	task.cdb[0] = SPDK_SPC_INQUIRY;
	/* alloc_len >= 4096 */
	task.cdb[3] = 0xFF;
	task.cdb[4] = 0xFF;
	task.lun = NULL;

	spdk_scsi_task_process_null_lun(&task);

	CU_ASSERT_EQUAL(task.status, SPDK_SCSI_STATUS_GOOD);

	spdk_scsi_task_put(&task);

	/* spdk_scsi_task_process_null_lun() does not call cpl_fn */
	CU_ASSERT_EQUAL(g_task_count, 1);
	g_task_count = 0;
}

static void
lun_append_task_null_lun_alloc_len_lt_4096(void)
{
	struct spdk_scsi_task task = { 0 };
	uint8_t cdb[6] = { 0 };

	ut_init_task(&task);
	task.cdb = cdb;
	task.cdb[0] = SPDK_SPC_INQUIRY;
	/* alloc_len < 4096 */
	task.cdb[3] = 0;
	task.cdb[4] = 0;
	/* alloc_len is set to a minimal value of 4096
	 * Hence, buf of size 4096 is allocated */
	spdk_scsi_task_process_null_lun(&task);

	CU_ASSERT_EQUAL(task.status, SPDK_SCSI_STATUS_GOOD);

	spdk_scsi_task_put(&task);

	/* spdk_scsi_task_process_null_lun() does not call cpl_fn */
	CU_ASSERT_EQUAL(g_task_count, 1);
	g_task_count = 0;
}

static void
lun_append_task_null_lun_not_supported(void)
{
	struct spdk_scsi_task task = { 0 };
	uint8_t cdb[6] = { 0 };

	ut_init_task(&task);
	task.cdb = cdb;
	task.lun = NULL;

	spdk_scsi_task_process_null_lun(&task);

	CU_ASSERT_EQUAL(task.status, SPDK_SCSI_STATUS_CHECK_CONDITION);
	/* LUN not supported; task's data transferred should be 0 */
	CU_ASSERT_EQUAL(task.data_transferred, 0);

	/* spdk_scsi_task_process_null_lun() does not call cpl_fn */
	CU_ASSERT_EQUAL(g_task_count, 1);
	g_task_count = 0;
}

static void
lun_execute_scsi_task_pending(void)
{
	struct spdk_scsi_lun *lun;
	struct spdk_scsi_task task = { 0 };
	struct spdk_scsi_dev dev = { 0 };

	lun = lun_construct();

	ut_init_task(&task);
	task.lun = lun;
	lun->dev = &dev;

	g_lun_execute_fail = false;
	g_lun_execute_status = SPDK_SCSI_TASK_PENDING;

	/* the tasks list should still be empty since it has not been
	   executed yet
	 */
	CU_ASSERT(TAILQ_EMPTY(&lun->tasks));

	scsi_lun_execute_task(lun, &task);

	/* Assert the task has been successfully added to the tasks queue */
	CU_ASSERT(!TAILQ_EMPTY(&lun->tasks));

	/* task is still on the tasks list */
	CU_ASSERT_EQUAL(g_task_count, 1);

	/* Need to complete task so LUN might be removed right now */
	scsi_lun_complete_task(lun, &task);

	CU_ASSERT_EQUAL(g_task_count, 0);

	lun_destruct(lun);
}

static void
lun_execute_scsi_task_complete(void)
{
	struct spdk_scsi_lun *lun;
	struct spdk_scsi_task task = { 0 };
	struct spdk_scsi_dev dev = { 0 };

	lun = lun_construct();

	ut_init_task(&task);
	task.lun = lun;
	lun->dev = &dev;

	g_lun_execute_fail = false;
	g_lun_execute_status = SPDK_SCSI_TASK_COMPLETE;

	/* the tasks list should still be empty since it has not been
	   executed yet
	 */
	CU_ASSERT(TAILQ_EMPTY(&lun->tasks));

	scsi_lun_execute_task(lun, &task);

	/* Assert the task has not been added to the tasks queue */
	CU_ASSERT(TAILQ_EMPTY(&lun->tasks));

	lun_destruct(lun);

	CU_ASSERT_EQUAL(g_task_count, 0);
}

static void
lun_execute_scsi_task_resize(void)
{
	struct spdk_scsi_lun *lun;
	struct spdk_scsi_task task = { 0 };
	struct spdk_scsi_dev dev = { 0 };
	uint8_t cdb = SPDK_SBC_READ_16;

	lun = lun_construct();

	ut_init_task(&task);
	task.lun = lun;
	task.cdb = &cdb;
	lun->dev = &dev;
	lun->resizing = true;

	/* the tasks list should still be empty since it has not been
	   executed yet
	 */
	CU_ASSERT(TAILQ_EMPTY(&lun->tasks));

	scsi_lun_execute_task(lun, &task);
	CU_ASSERT_EQUAL(task.status, SPDK_SCSI_STATUS_CHECK_CONDITION);
	/* SENSE KEY */
	CU_ASSERT_EQUAL(task.sense_data[2], SPDK_SCSI_SENSE_UNIT_ATTENTION);
	/* SCSI_SENSE_ASCQ_CAPACITY_DATA_HAS_CHANGED: 0x2a09 */
	CU_ASSERT_EQUAL(task.sense_data[12], SPDK_SCSI_ASC_CAPACITY_DATA_HAS_CHANGED);
	CU_ASSERT_EQUAL(task.sense_data[13], SPDK_SCSI_ASCQ_CAPACITY_DATA_HAS_CHANGED);
	CU_ASSERT(lun->resizing == false);

	/* Assert the task has not been added to the tasks queue */
	CU_ASSERT(TAILQ_EMPTY(&lun->tasks));

	lun_destruct(lun);

	CU_ASSERT_EQUAL(g_task_count, 0);
}

static void
lun_destruct_success(void)
{
	struct spdk_scsi_lun *lun;

	lun = lun_construct();

	scsi_lun_destruct(lun);

	CU_ASSERT_EQUAL(g_task_count, 0);
}

static void
lun_construct_null_ctx(void)
{
	struct spdk_scsi_lun		*lun;

	lun = scsi_lun_construct(NULL, NULL, NULL, NULL, NULL);

	/* lun should be NULL since we passed NULL for the ctx pointer. */
	CU_ASSERT(lun == NULL);
	CU_ASSERT_EQUAL(g_task_count, 0);
}

static void
lun_construct_success(void)
{
	struct spdk_scsi_lun *lun = lun_construct();

	lun_destruct(lun);

	CU_ASSERT_EQUAL(g_task_count, 0);
}

static void
lun_reset_task_wait_scsi_task_complete(void)
{
	struct spdk_scsi_lun *lun;
	struct spdk_scsi_task task = { 0 };
	struct spdk_scsi_task mgmt_task = { 0 };
	struct spdk_scsi_dev dev = { 0 };

	lun = lun_construct();
	lun->dev = &dev;

	ut_init_task(&task);
	task.lun = lun;

	g_lun_execute_fail = false;
	g_lun_execute_status = SPDK_SCSI_TASK_PENDING;

	ut_init_task(&mgmt_task);
	mgmt_task.lun = lun;
	mgmt_task.function = SPDK_SCSI_TASK_FUNC_LUN_RESET;

	/* Execute the task but it is still in the task list. */
	scsi_lun_execute_task(lun, &task);

	CU_ASSERT(TAILQ_EMPTY(&lun->pending_tasks));
	CU_ASSERT(!TAILQ_EMPTY(&lun->tasks));

	/* Execute the reset task */
	scsi_lun_execute_mgmt_task(lun, &mgmt_task);

	/* The reset task should be on the submitted mgmt task list and
	 * a poller is created because the task prior to the reset task is pending.
	 */
	CU_ASSERT(!TAILQ_EMPTY(&lun->mgmt_tasks));
	CU_ASSERT(lun->reset_poller != NULL);

	/* Execute the poller to check if the task prior to the reset task complete. */
	scsi_lun_reset_check_outstanding_tasks(&mgmt_task);

	CU_ASSERT(!TAILQ_EMPTY(&lun->mgmt_tasks));
	CU_ASSERT(lun->reset_poller != NULL);

	/* Complete the task. */
	scsi_lun_complete_task(lun, &task);

	CU_ASSERT(TAILQ_EMPTY(&lun->tasks));

	/* Execute the poller to check if the task prior to the reset task complete. */
	scsi_lun_reset_check_outstanding_tasks(&mgmt_task);

	CU_ASSERT(TAILQ_EMPTY(&lun->mgmt_tasks));
	CU_ASSERT(lun->reset_poller == NULL);
	CU_ASSERT_EQUAL(mgmt_task.status, SPDK_SCSI_STATUS_GOOD);
	CU_ASSERT_EQUAL(mgmt_task.response, SPDK_SCSI_TASK_MGMT_RESP_SUCCESS);

	lun_destruct(lun);

	CU_ASSERT_EQUAL(g_task_count, 0);
}

static void
lun_reset_task_suspend_scsi_task(void)
{
	struct spdk_scsi_lun *lun;
	struct spdk_scsi_task task = { 0 };
	struct spdk_scsi_task mgmt_task = { 0 };
	struct spdk_scsi_dev dev = { 0 };

	lun = lun_construct();
	lun->dev = &dev;

	ut_init_task(&task);
	task.lun = lun;

	g_lun_execute_fail = false;
	g_lun_execute_status = SPDK_SCSI_TASK_COMPLETE;

	ut_init_task(&mgmt_task);
	mgmt_task.lun = lun;
	mgmt_task.function = SPDK_SCSI_TASK_FUNC_LUN_RESET;

	/* Append a reset task to the pending mgmt task list. */
	scsi_lun_append_mgmt_task(lun, &mgmt_task);

	CU_ASSERT(!TAILQ_EMPTY(&lun->pending_mgmt_tasks));

	/* Execute the task but it is on the pending task list. */
	scsi_lun_execute_task(lun, &task);

	CU_ASSERT(!TAILQ_EMPTY(&lun->pending_tasks));

	/* Execute the reset task. The task will be executed then. */
	_scsi_lun_execute_mgmt_task(lun);

	CU_ASSERT(TAILQ_EMPTY(&lun->mgmt_tasks));
	CU_ASSERT(lun->reset_poller == NULL);
	CU_ASSERT_EQUAL(mgmt_task.status, SPDK_SCSI_STATUS_GOOD);
	CU_ASSERT_EQUAL(mgmt_task.response, SPDK_SCSI_TASK_MGMT_RESP_SUCCESS);

	CU_ASSERT(TAILQ_EMPTY(&lun->pending_tasks));
	CU_ASSERT(TAILQ_EMPTY(&lun->tasks));

	lun_destruct(lun);

	CU_ASSERT_EQUAL(g_task_count, 0);
}

static void
lun_check_pending_tasks_only_for_specific_initiator(void)
{
	struct spdk_scsi_lun *lun;
	struct spdk_scsi_task task1 = {};
	struct spdk_scsi_task task2 = {};
	struct spdk_scsi_port initiator_port1 = {};
	struct spdk_scsi_port initiator_port2 = {};
	struct spdk_scsi_port initiator_port3 = {};

	lun = scsi_lun_construct("ut_bdev", NULL, NULL, NULL, NULL);

	task1.initiator_port = &initiator_port1;
	task2.initiator_port = &initiator_port2;

	TAILQ_INSERT_TAIL(&lun->tasks, &task1, scsi_link);
	TAILQ_INSERT_TAIL(&lun->tasks, &task2, scsi_link);
	CU_ASSERT(scsi_lun_has_outstanding_tasks(lun) == true);
	CU_ASSERT(_scsi_lun_has_pending_tasks(lun) == false);
	CU_ASSERT(scsi_lun_has_pending_tasks(lun, NULL) == true);
	CU_ASSERT(scsi_lun_has_pending_tasks(lun, &initiator_port1) == true);
	CU_ASSERT(scsi_lun_has_pending_tasks(lun, &initiator_port2) == true);
	CU_ASSERT(scsi_lun_has_pending_tasks(lun, &initiator_port3) == false);
	TAILQ_REMOVE(&lun->tasks, &task1, scsi_link);
	TAILQ_REMOVE(&lun->tasks, &task2, scsi_link);
	CU_ASSERT(_scsi_lun_has_pending_tasks(lun) == false);
	CU_ASSERT(scsi_lun_has_pending_tasks(lun, NULL) == false);

	TAILQ_INSERT_TAIL(&lun->pending_tasks, &task1, scsi_link);
	TAILQ_INSERT_TAIL(&lun->pending_tasks, &task2, scsi_link);
	CU_ASSERT(scsi_lun_has_outstanding_tasks(lun) == false);
	CU_ASSERT(_scsi_lun_has_pending_tasks(lun) == true);
	CU_ASSERT(scsi_lun_has_pending_tasks(lun, NULL) == true);
	CU_ASSERT(scsi_lun_has_pending_tasks(lun, &initiator_port1) == true);
	CU_ASSERT(scsi_lun_has_pending_tasks(lun, &initiator_port2) == true);
	CU_ASSERT(scsi_lun_has_pending_tasks(lun, &initiator_port3) == false);
	TAILQ_REMOVE(&lun->pending_tasks, &task1, scsi_link);
	TAILQ_REMOVE(&lun->pending_tasks, &task2, scsi_link);
	CU_ASSERT(_scsi_lun_has_pending_tasks(lun) == false);
	CU_ASSERT(scsi_lun_has_pending_tasks(lun, NULL) == false);

	TAILQ_INSERT_TAIL(&lun->mgmt_tasks, &task1, scsi_link);
	TAILQ_INSERT_TAIL(&lun->mgmt_tasks, &task2, scsi_link);
	CU_ASSERT(scsi_lun_has_outstanding_mgmt_tasks(lun) == true);
	CU_ASSERT(_scsi_lun_has_pending_mgmt_tasks(lun) == false);
	CU_ASSERT(scsi_lun_has_pending_mgmt_tasks(lun, NULL) == true);
	CU_ASSERT(scsi_lun_has_pending_mgmt_tasks(lun, &initiator_port1) == true);
	CU_ASSERT(scsi_lun_has_pending_mgmt_tasks(lun, &initiator_port2) == true);
	CU_ASSERT(scsi_lun_has_pending_mgmt_tasks(lun, &initiator_port3) == false);
	TAILQ_REMOVE(&lun->mgmt_tasks, &task1, scsi_link);
	TAILQ_REMOVE(&lun->mgmt_tasks, &task2, scsi_link);
	CU_ASSERT(_scsi_lun_has_pending_mgmt_tasks(lun) == false);
	CU_ASSERT(scsi_lun_has_pending_mgmt_tasks(lun, NULL) == false);

	TAILQ_INSERT_TAIL(&lun->pending_mgmt_tasks, &task1, scsi_link);
	TAILQ_INSERT_TAIL(&lun->pending_mgmt_tasks, &task2, scsi_link);
	CU_ASSERT(_scsi_lun_has_pending_mgmt_tasks(lun) == true);
	CU_ASSERT(scsi_lun_has_pending_mgmt_tasks(lun, NULL) == true);
	CU_ASSERT(scsi_lun_has_pending_mgmt_tasks(lun, &initiator_port1) == true);
	CU_ASSERT(scsi_lun_has_pending_mgmt_tasks(lun, &initiator_port2) == true);
	CU_ASSERT(scsi_lun_has_pending_mgmt_tasks(lun, &initiator_port3) == false);
	TAILQ_REMOVE(&lun->pending_mgmt_tasks, &task1, scsi_link);
	TAILQ_REMOVE(&lun->pending_mgmt_tasks, &task2, scsi_link);
	CU_ASSERT(_scsi_lun_has_pending_mgmt_tasks(lun) == false);
	CU_ASSERT(scsi_lun_has_pending_mgmt_tasks(lun, NULL) == false);

	scsi_lun_remove(lun);
}

static void
abort_pending_mgmt_tasks_when_lun_is_removed(void)
{
	struct spdk_scsi_lun *lun;
	struct spdk_scsi_task task1, task2, task3;

	lun = scsi_lun_construct("ut_bdev", NULL, NULL, NULL, NULL);

	/* Normal case */
	ut_init_task(&task1);
	ut_init_task(&task2);
	ut_init_task(&task3);
	task1.lun = lun;
	task2.lun = lun;
	task3.lun = lun;
	task1.function = SPDK_SCSI_TASK_FUNC_LUN_RESET;
	task2.function = SPDK_SCSI_TASK_FUNC_LUN_RESET;
	task3.function = SPDK_SCSI_TASK_FUNC_LUN_RESET;

	CU_ASSERT(g_task_count == 3);

	scsi_lun_append_mgmt_task(lun, &task1);
	scsi_lun_append_mgmt_task(lun, &task2);
	scsi_lun_append_mgmt_task(lun, &task3);

	CU_ASSERT(!TAILQ_EMPTY(&lun->pending_mgmt_tasks));

	_scsi_lun_execute_mgmt_task(lun);

	CU_ASSERT(TAILQ_EMPTY(&lun->pending_mgmt_tasks));
	CU_ASSERT(TAILQ_EMPTY(&lun->mgmt_tasks));
	CU_ASSERT(g_task_count == 0);
	CU_ASSERT(task1.response == SPDK_SCSI_TASK_MGMT_RESP_SUCCESS);
	CU_ASSERT(task2.response == SPDK_SCSI_TASK_MGMT_RESP_SUCCESS);
	CU_ASSERT(task3.response == SPDK_SCSI_TASK_MGMT_RESP_SUCCESS);

	/* LUN hotplug case */
	ut_init_task(&task1);
	ut_init_task(&task2);
	ut_init_task(&task3);
	task1.function = SPDK_SCSI_TASK_FUNC_LUN_RESET;
	task2.function = SPDK_SCSI_TASK_FUNC_LUN_RESET;
	task3.function = SPDK_SCSI_TASK_FUNC_LUN_RESET;

	CU_ASSERT(g_task_count == 3);

	scsi_lun_append_mgmt_task(lun, &task1);
	scsi_lun_append_mgmt_task(lun, &task2);
	scsi_lun_append_mgmt_task(lun, &task3);

	CU_ASSERT(!TAILQ_EMPTY(&lun->pending_mgmt_tasks));

	lun->removed = true;

	_scsi_lun_execute_mgmt_task(lun);

	CU_ASSERT(TAILQ_EMPTY(&lun->pending_mgmt_tasks));
	CU_ASSERT(TAILQ_EMPTY(&lun->mgmt_tasks));
	CU_ASSERT(g_task_count == 0);
	CU_ASSERT(task1.response == SPDK_SCSI_TASK_MGMT_RESP_INVALID_LUN);
	CU_ASSERT(task2.response == SPDK_SCSI_TASK_MGMT_RESP_INVALID_LUN);
	CU_ASSERT(task3.response == SPDK_SCSI_TASK_MGMT_RESP_INVALID_LUN);

	scsi_lun_remove(lun);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("lun_suite", NULL, NULL);

	CU_ADD_TEST(suite, lun_task_mgmt_execute_abort_task_not_supported);
	CU_ADD_TEST(suite, lun_task_mgmt_execute_abort_task_all_not_supported);
	CU_ADD_TEST(suite, lun_task_mgmt_execute_lun_reset);
	CU_ADD_TEST(suite, lun_task_mgmt_execute_invalid_case);
	CU_ADD_TEST(suite, lun_append_task_null_lun_task_cdb_spc_inquiry);
	CU_ADD_TEST(suite, lun_append_task_null_lun_alloc_len_lt_4096);
	CU_ADD_TEST(suite, lun_append_task_null_lun_not_supported);
	CU_ADD_TEST(suite, lun_execute_scsi_task_pending);
	CU_ADD_TEST(suite, lun_execute_scsi_task_complete);
	CU_ADD_TEST(suite, lun_execute_scsi_task_resize);
	CU_ADD_TEST(suite, lun_destruct_success);
	CU_ADD_TEST(suite, lun_construct_null_ctx);
	CU_ADD_TEST(suite, lun_construct_success);
	CU_ADD_TEST(suite, lun_reset_task_wait_scsi_task_complete);
	CU_ADD_TEST(suite, lun_reset_task_suspend_scsi_task);
	CU_ADD_TEST(suite, lun_check_pending_tasks_only_for_specific_initiator);
	CU_ADD_TEST(suite, abort_pending_mgmt_tasks_when_lun_is_removed);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	allocate_threads(1);
	set_thread(0);
	CU_basic_run_tests();
	free_threads();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
