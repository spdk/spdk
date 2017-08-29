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

#include "task.c"
#include "lun.c"
#include "lun_db.c"

#include "spdk_internal/mock.h"

/* Unit test bdev mockup */
struct spdk_bdev {
	int x;
};

SPDK_LOG_REGISTER_TRACE_FLAG("scsi", SPDK_TRACE_SCSI)

struct spdk_scsi_globals g_spdk_scsi;

static bool g_lun_execute_fail = false;
static int g_lun_execute_status = SPDK_SCSI_TASK_PENDING;
static uint32_t g_task_count = 0;

void
spdk_poller_register(struct spdk_poller **ppoller, spdk_poller_fn fn, void *arg,
		     uint32_t lcore, uint64_t period_microseconds)
{
}

void
spdk_poller_unregister(struct spdk_poller **ppoller,
		       struct spdk_event *complete)
{
}

uint32_t
spdk_env_get_current_core(void)
{
	return 0;
}

void spdk_trace_record(uint16_t tpoint_id, uint16_t poller_id, uint32_t size,
		       uint64_t object_id, uint64_t arg1)
{
}

static void
spdk_lun_ut_cpl_task(struct spdk_scsi_task *task)
{
	SPDK_CU_ASSERT_FATAL(g_task_count > 0);
	g_task_count--;
}

static void
spdk_lun_ut_free_task(struct spdk_scsi_task *task)
{
	/* This should never get called since we never call spdk_scsi_task_put(). */
	SPDK_CU_ASSERT_FATAL(0);
}

static void
ut_init_task(struct spdk_scsi_task *task)
{
	memset(task, 0, sizeof(*task));
	spdk_scsi_task_construct(task, spdk_lun_ut_cpl_task,
				 spdk_lun_ut_free_task,
				 NULL);
	g_task_count++;
}

void *
spdk_dma_malloc(size_t size, size_t align, uint64_t *phys_addr)
{
	void *buf = malloc(size);
	if (phys_addr)
		*phys_addr = (uint64_t)buf;
	return buf;
}

void *
spdk_dma_zmalloc(size_t size, size_t align, uint64_t *phys_addr)
{
	void *buf = calloc(size, 1);
	if (phys_addr)
		*phys_addr = (uint64_t)buf;
	return buf;
}

void
spdk_dma_free(void *buf)
{
	free(buf);
}

int
spdk_bdev_free_io(struct spdk_bdev_io *bdev_io)
{
	CU_ASSERT(0);
	return -1;
}

int
spdk_bdev_open(struct spdk_bdev *bdev, bool write, spdk_bdev_remove_cb_t remove_cb,
	       void *remove_ctx, struct spdk_bdev_desc **desc)
{
	return 0;
}

void
spdk_bdev_close(struct spdk_bdev_desc *desc)
{
}

const char *
spdk_bdev_get_name(const struct spdk_bdev *bdev)
{
	return "test";
}

void spdk_scsi_dev_queue_mgmt_task(struct spdk_scsi_dev *dev,
				   struct spdk_scsi_task *task,
				   enum spdk_scsi_task_func func)
{
}

void spdk_scsi_dev_delete_lun(struct spdk_scsi_dev *dev,
			      struct spdk_scsi_lun *lun)
{
	return;
}

int
spdk_bdev_scsi_reset(struct spdk_bdev *bdev, struct spdk_scsi_task *task)
{
	return 0;
}

int
spdk_bdev_scsi_execute(struct spdk_bdev *bdev, struct spdk_scsi_task *task)
{
	if (g_lun_execute_fail)
		return -EINVAL;
	else {
		task->status = SPDK_SCSI_STATUS_GOOD;

		if (g_lun_execute_status == SPDK_SCSI_TASK_PENDING)
			return g_lun_execute_status;
		else if (g_lun_execute_status == SPDK_SCSI_TASK_COMPLETE)
			return g_lun_execute_status;
		else
			return 0;
	}
}

struct spdk_io_channel *
spdk_bdev_get_io_channel(struct spdk_bdev_desc *desc)
{
	return NULL;
}

void
spdk_put_io_channel(struct spdk_io_channel *ch)
{
}

DEFINE_STUB(spdk_io_channel_get_thread, struct spdk_thread *, (struct spdk_io_channel *ch), NULL)
DEFINE_STUB(spdk_get_thread, struct spdk_thread *, (void), NULL)

static _spdk_scsi_lun *
lun_construct(void)
{
	struct spdk_scsi_lun		*lun;
	struct spdk_bdev		bdev;

	lun = spdk_scsi_lun_construct("lun0", &bdev, NULL, NULL);

	SPDK_CU_ASSERT_FATAL(lun != NULL);
	if (lun != NULL) {
		SPDK_CU_ASSERT_FATAL(TAILQ_EMPTY(&lun->pending_tasks));
	}

	return lun;
}

static void
lun_destruct(struct spdk_scsi_lun *lun)
{
	spdk_scsi_lun_destruct(lun);
}

static void
lun_task_mgmt_execute_null_task(void)
{
	int rc;

	rc = spdk_scsi_lun_task_mgmt_execute(NULL, SPDK_SCSI_TASK_FUNC_ABORT_TASK);

	/* returns -1 since we passed NULL for the task */
	CU_ASSERT_TRUE(rc < 0);
	CU_ASSERT_EQUAL(g_task_count, 0);
}

static void
lun_task_mgmt_execute_abort_task_null_lun_failure(void)
{
	struct spdk_scsi_task mgmt_task = { 0 };
	struct spdk_scsi_port initiator_port = { 0 };
	int rc;

	ut_init_task(&mgmt_task);
	mgmt_task.lun = NULL;
	mgmt_task.initiator_port = &initiator_port;

	rc = spdk_scsi_lun_task_mgmt_execute(&mgmt_task, SPDK_SCSI_TASK_FUNC_ABORT_TASK);

	/* returns -1 since we passed NULL for LUN */
	CU_ASSERT_TRUE(rc < 0);
	CU_ASSERT_EQUAL(g_task_count, 0);
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
	int rc;

	lun = lun_construct();
	lun->dev = &dev;

	ut_init_task(&mgmt_task);
	mgmt_task.lun = lun;
	mgmt_task.initiator_port = &initiator_port;

	/* Params to add regular task to the lun->tasks */
	ut_init_task(&task);
	task.lun = lun;
	task.cdb = cdb;

	spdk_scsi_lun_append_task(lun, &task);

	/* task should now be on the pending_task list */
	CU_ASSERT(!TAILQ_EMPTY(&lun->pending_tasks));

	spdk_scsi_lun_execute_tasks(lun);

	/* task should now be on the tasks list */
	CU_ASSERT(!TAILQ_EMPTY(&lun->tasks));

	rc = spdk_scsi_lun_task_mgmt_execute(&mgmt_task, SPDK_SCSI_TASK_FUNC_ABORT_TASK);

	/* returns -1 since task abort is not supported */
	CU_ASSERT_TRUE(rc < 0);
	CU_ASSERT(mgmt_task.response == SPDK_SCSI_TASK_MGMT_RESP_REJECT_FUNC_NOT_SUPPORTED);

	lun_destruct(lun);

	/* task is still on the tasks list */
	CU_ASSERT_EQUAL(g_task_count, 1);
	g_task_count = 0;
}

static void
lun_task_mgmt_execute_abort_task_all_null_lun_failure(void)
{
	struct spdk_scsi_task mgmt_task = { 0 };
	struct spdk_scsi_port initiator_port = { 0 };
	int rc;

	ut_init_task(&mgmt_task);
	mgmt_task.lun = NULL;
	mgmt_task.initiator_port = &initiator_port;

	rc = spdk_scsi_lun_task_mgmt_execute(&mgmt_task, SPDK_SCSI_TASK_FUNC_ABORT_TASK_SET);

	/* Returns -1 since we passed NULL for lun */
	CU_ASSERT_TRUE(rc < 0);

	CU_ASSERT_EQUAL(g_task_count, 0);
}

static void
lun_task_mgmt_execute_abort_task_all_not_supported(void)
{
	struct spdk_scsi_lun *lun;
	struct spdk_scsi_task task = { 0 };
	struct spdk_scsi_task mgmt_task = { 0 };
	struct spdk_scsi_port initiator_port = { 0 };
	struct spdk_scsi_dev dev = { 0 };
	int rc;
	uint8_t cdb[6] = { 0 };

	lun = lun_construct();
	lun->dev = &dev;

	ut_init_task(&mgmt_task);
	mgmt_task.lun = lun;
	mgmt_task.initiator_port = &initiator_port;

	/* Params to add regular task to the lun->tasks */
	ut_init_task(&task);
	task.initiator_port = &initiator_port;
	task.lun = lun;
	task.cdb = cdb;

	spdk_scsi_lun_append_task(lun, &task);

	/* task should now be on the pending_task list */
	CU_ASSERT(!TAILQ_EMPTY(&lun->pending_tasks));

	spdk_scsi_lun_execute_tasks(lun);

	/* task should now be on the tasks list */
	CU_ASSERT(!TAILQ_EMPTY(&lun->tasks));

	rc = spdk_scsi_lun_task_mgmt_execute(&mgmt_task, SPDK_SCSI_TASK_FUNC_ABORT_TASK_SET);

	/* returns -1 since task abort is not supported */
	CU_ASSERT_TRUE(rc < 0);
	CU_ASSERT(mgmt_task.response == SPDK_SCSI_TASK_MGMT_RESP_REJECT_FUNC_NOT_SUPPORTED);

	lun_destruct(lun);

	/* task is still on the tasks list */
	CU_ASSERT_EQUAL(g_task_count, 1);
	g_task_count = 0;
}

static void
lun_task_mgmt_execute_lun_reset_failure(void)
{
	struct spdk_scsi_task mgmt_task = { 0 };
	int rc;

	ut_init_task(&mgmt_task);
	mgmt_task.lun = NULL;

	rc = spdk_scsi_lun_task_mgmt_execute(&mgmt_task, SPDK_SCSI_TASK_FUNC_LUN_RESET);

	/* Returns -1 since we passed NULL for lun */
	CU_ASSERT_TRUE(rc < 0);

	CU_ASSERT_EQUAL(g_task_count, 0);
}

static void
lun_task_mgmt_execute_lun_reset(void)
{
	struct spdk_scsi_lun *lun;
	struct spdk_scsi_task mgmt_task = { 0 };
	struct spdk_scsi_dev dev = { 0 };
	int rc;

	lun = lun_construct();
	lun->dev = &dev;

	ut_init_task(&mgmt_task);
	mgmt_task.lun = lun;

	rc = spdk_scsi_lun_task_mgmt_execute(&mgmt_task, SPDK_SCSI_TASK_FUNC_LUN_RESET);

	/* Returns success */
	CU_ASSERT_EQUAL(rc, 0);

	lun_destruct(lun);

	/* task is still on the tasks list */
	CU_ASSERT_EQUAL(g_task_count, 1);
	g_task_count = 0;
}

static void
lun_task_mgmt_execute_invalid_case(void)
{
	struct spdk_scsi_lun *lun;
	struct spdk_scsi_task mgmt_task = { 0 };
	struct spdk_scsi_dev dev = { 0 };
	int rc;

	lun = lun_construct();
	lun->dev = &dev;

	ut_init_task(&mgmt_task);
	/* Pass an invalid value to the switch statement */
	rc = spdk_scsi_lun_task_mgmt_execute(&mgmt_task, 5);

	/* Returns -1 on passing an invalid value to the switch case */
	CU_ASSERT_TRUE(rc < 0);

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

	spdk_scsi_task_free_data(&task);

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

	spdk_scsi_lun_append_task(lun, &task);

	/* task should now be on the pending_task list */
	CU_ASSERT(!TAILQ_EMPTY(&lun->pending_tasks));

	/* but the tasks list should still be empty since it has not been
	   executed yet
	 */
	CU_ASSERT(TAILQ_EMPTY(&lun->tasks));

	spdk_scsi_lun_execute_tasks(lun);

	/* Assert the task has been successfully added to the tasks queue */
	CU_ASSERT(!TAILQ_EMPTY(&lun->tasks));

	lun_destruct(lun);

	/* task is still on the tasks list */
	CU_ASSERT_EQUAL(g_task_count, 1);
	g_task_count = 0;
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

	spdk_scsi_lun_append_task(lun, &task);

	/* task should now be on the pending_task list */
	CU_ASSERT(!TAILQ_EMPTY(&lun->pending_tasks));

	/* but the tasks list should still be empty since it has not been
	   executed yet
	 */
	CU_ASSERT(TAILQ_EMPTY(&lun->tasks));

	spdk_scsi_lun_execute_tasks(lun);

	/* Assert the task has not been added to the tasks queue */
	CU_ASSERT(TAILQ_EMPTY(&lun->tasks));

	lun_destruct(lun);

	CU_ASSERT_EQUAL(g_task_count, 0);
}

static void
lun_destruct_success(void)
{
	struct spdk_scsi_lun *lun;
	int rc;

	lun = lun_construct();

	rc = spdk_scsi_lun_destruct(lun);

	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(g_task_count, 0);
}

static void
lun_construct_null_ctx(void)
{
	struct spdk_scsi_lun		*lun;

	lun = spdk_scsi_lun_construct("lun0", NULL, NULL, NULL);

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
lun_construct_same_same_twice(void)
{
	struct spdk_scsi_lun		*lun, *lun2;
	struct spdk_bdev		bdev, bdev2;

	lun = spdk_scsi_lun_construct("lun0", &bdev, NULL, NULL);

	/* Successfully constructs and returns lun */
	SPDK_CU_ASSERT_FATAL(lun != NULL);

	lun2 = spdk_scsi_lun_construct("lun0", &bdev2, NULL, NULL);

	/* Fails to construct the same lun on another bdev and returns NULL */
	CU_ASSERT(lun2 == NULL);

	lun_destruct(lun);

	CU_ASSERT_EQUAL(g_task_count, 0);
}

static void
lun_delete(void)
{
	struct spdk_scsi_lun *lun;
	int rc;

	lun = lun_construct();

	rc = spdk_scsi_lun_delete(lun->name);
	CU_ASSERT_EQUAL(rc, 0);

	lun = lun_construct();

	rc = spdk_scsi_lun_claim(lun);
	CU_ASSERT_EQUAL(rc, 0);

	rc = spdk_scsi_lun_delete(lun->name);
	CU_ASSERT_EQUAL(rc, 0);

	rc = spdk_scsi_lun_delete("test");
	CU_ASSERT_EQUAL(rc, -1);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int 	num_failures;
	int		rc;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("lun_suite", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "task management - null task failure",
			    lun_task_mgmt_execute_null_task) == NULL
		|| CU_add_test(suite, "task management abort task - null lun failure",
			       lun_task_mgmt_execute_abort_task_null_lun_failure) == NULL
		|| CU_add_test(suite, "task management abort task - not supported",
			       lun_task_mgmt_execute_abort_task_not_supported) == NULL
		|| CU_add_test(suite, "task management abort task set - null lun failure",
			       lun_task_mgmt_execute_abort_task_all_null_lun_failure) == NULL
		|| CU_add_test(suite, "task management abort task set - success",
			       lun_task_mgmt_execute_abort_task_all_not_supported) == NULL
		|| CU_add_test(suite, "task management - lun reset failure",
			       lun_task_mgmt_execute_lun_reset_failure) == NULL
		|| CU_add_test(suite, "task management - lun reset success",
			       lun_task_mgmt_execute_lun_reset) == NULL
		|| CU_add_test(suite, "task management - invalid option",
			       lun_task_mgmt_execute_invalid_case) == NULL
		|| CU_add_test(suite, "append task - null lun SPDK_SPC_INQUIRY",
			       lun_append_task_null_lun_task_cdb_spc_inquiry) == NULL
		|| CU_add_test(suite, "append task - allocated length less than 4096",
			       lun_append_task_null_lun_alloc_len_lt_4096) == NULL
		|| CU_add_test(suite, "append task - unsupported lun",
			       lun_append_task_null_lun_not_supported) == NULL
		|| CU_add_test(suite, "execute task - scsi task pending",
			       lun_execute_scsi_task_pending) == NULL
		|| CU_add_test(suite, "execute task - scsi task complete",
			       lun_execute_scsi_task_complete) == NULL
		|| CU_add_test(suite, "destruct task - success", lun_destruct_success) == NULL
		|| CU_add_test(suite, "construct - null ctx", lun_construct_null_ctx) == NULL
		|| CU_add_test(suite, "construct - success", lun_construct_success) == NULL
		|| CU_add_test(suite, "construct - same lun twice", lun_construct_same_same_twice) == NULL
		|| CU_add_test(suite, "lun_delete", lun_delete) == NULL
	) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();

	if (argc > 1) {
		rc = spdk_cunit_print_results(argv[1]);
		if (rc != 0) {
			CU_cleanup_registry();
			return rc;
		}
	}

	CU_cleanup_registry();
	return num_failures;
}
