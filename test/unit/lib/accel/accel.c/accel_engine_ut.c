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
#include "spdk_internal/mock.h"
#include "thread/thread_internal.h"
#include "common/lib/test_env.c"

#include "accel/accel_engine.c"

DEFINE_STUB(spdk_json_write_array_begin, int, (struct spdk_json_write_ctx *w), 0);
DEFINE_STUB(spdk_json_write_array_end, int, (struct spdk_json_write_ctx *w), 0);

/* global vars and setup/cleanup functions used for all test functions */
struct spdk_accel_engine g_accel_engine = {};
struct spdk_io_channel *g_ch = NULL;
struct accel_io_channel *g_accel_ch = NULL;
struct sw_accel_io_channel *g_sw_ch = NULL;
struct spdk_io_channel *g_engine_ch = NULL;

static int
test_setup(void)
{
	g_ch = calloc(1, sizeof(struct spdk_io_channel) + sizeof(struct accel_io_channel));
	if (g_ch == NULL) {
		/* for some reason the assert fatal macro doesn't work in the setup function. */
		CU_ASSERT(false);
		return -1;
	}
	g_accel_ch = (struct accel_io_channel *)((char *)g_ch + sizeof(struct spdk_io_channel));
	g_engine_ch = calloc(1, sizeof(struct spdk_io_channel) + sizeof(struct sw_accel_io_channel));
	if (g_engine_ch == NULL) {
		CU_ASSERT(false);
		return -1;
	}
	g_accel_ch->engine_ch = g_engine_ch;
	g_sw_ch = (struct sw_accel_io_channel *)((char *)g_accel_ch->engine_ch + sizeof(
				struct spdk_io_channel));
	TAILQ_INIT(&g_sw_ch->tasks_to_complete);
	return 0;
}

static int
test_cleanup(void)
{
	free(g_ch);
	free(g_engine_ch);

	return 0;
}

static void
test_spdk_accel_hw_engine_register(void)
{
	/* Run once with no engine assigned, assign it. */
	g_hw_accel_engine = NULL;
	spdk_accel_hw_engine_register(&g_accel_engine);
	CU_ASSERT(g_hw_accel_engine == &g_accel_engine);

	/* Run with one assigned, should not change. */
	spdk_accel_hw_engine_register(&g_accel_engine);
	CU_ASSERT(g_hw_accel_engine == &g_accel_engine);
}

static int
test_accel_sw_register(void)
{
	/* Run once with no engine assigned, assign it. */
	g_sw_accel_engine = NULL;
	accel_sw_register(&g_accel_engine);
	CU_ASSERT(g_sw_accel_engine == &g_accel_engine);

	return 0;
}

static void
test_accel_sw_unregister(void)
{
	/* Run once engine assigned, make sure it gets unassigned. */
	g_sw_accel_engine = &g_accel_engine;
	accel_sw_unregister();
	CU_ASSERT(g_sw_accel_engine == NULL);
}

static void
test_is_supported(void)
{
	g_accel_engine.capabilities = ACCEL_COPY | ACCEL_DUALCAST | ACCEL_CRC32C;
	CU_ASSERT(_is_supported(&g_accel_engine, ACCEL_COPY) == true);
	CU_ASSERT(_is_supported(&g_accel_engine, ACCEL_FILL) == false);
	CU_ASSERT(_is_supported(&g_accel_engine, ACCEL_DUALCAST) == true);
	CU_ASSERT(_is_supported(&g_accel_engine, ACCEL_COMPARE) == false);
	CU_ASSERT(_is_supported(&g_accel_engine, ACCEL_CRC32C) == true);
	CU_ASSERT(_is_supported(&g_accel_engine, ACCEL_DIF) == false);
}

#define DUMMY_ARG 0xDEADBEEF
static bool g_dummy_cb_called = false;
static void
dummy_cb_fn(void *cb_arg, int status)
{
	CU_ASSERT(*(uint32_t *)cb_arg == DUMMY_ARG);
	CU_ASSERT(status == 0);
	g_dummy_cb_called = true;
}

static bool g_dummy_batch_cb_called = false;
static void
dummy_batch_cb_fn(void *cb_arg, int status)
{
	CU_ASSERT(*(uint32_t *)cb_arg == DUMMY_ARG);
	CU_ASSERT(status == 0);
	g_dummy_batch_cb_called = true;
}

static void
test_spdk_accel_task_complete(void)
{
	struct spdk_accel_task accel_task = {};
	struct spdk_accel_task *expected_accel_task = NULL;
	struct spdk_accel_batch	batch = {};
	struct spdk_accel_batch	*expected_batch = NULL;
	uint32_t cb_arg = DUMMY_ARG;
	int status = 0;

	accel_task.accel_ch = g_accel_ch;
	accel_task.cb_fn = dummy_cb_fn;
	accel_task.cb_arg = &cb_arg;
	TAILQ_INIT(&g_accel_ch->task_pool);

	/* W/o batch, confirm cb is called and task added to list. */
	spdk_accel_task_complete(&accel_task, status);
	CU_ASSERT(g_dummy_cb_called == true);
	expected_accel_task = TAILQ_FIRST(&g_accel_ch->task_pool);
	TAILQ_REMOVE(&g_accel_ch->task_pool, expected_accel_task, link);
	CU_ASSERT(expected_accel_task == &accel_task);

	TAILQ_INIT(&g_accel_ch->task_pool);
	TAILQ_INIT(&g_accel_ch->batches);
	TAILQ_INIT(&g_accel_ch->batch_pool);
	batch.count = 2;
	batch.cb_fn = dummy_batch_cb_fn;
	batch.cb_arg = &cb_arg;
	accel_task.batch = &batch;

	/* W/batch, confirm task cb is called and task added to list.
	 * but batch not completed yet. */
	spdk_accel_task_complete(&accel_task, status);
	CU_ASSERT(batch.count == 1);
	CU_ASSERT(false == g_dummy_batch_cb_called);

	expected_accel_task = TAILQ_FIRST(&g_accel_ch->task_pool);
	TAILQ_REMOVE(&g_accel_ch->task_pool, expected_accel_task, link);
	CU_ASSERT(expected_accel_task == &accel_task);
	CU_ASSERT(true == TAILQ_EMPTY(&g_accel_ch->batch_pool));
	CU_ASSERT(true == TAILQ_EMPTY(&g_accel_ch->batches));

	TAILQ_INIT(&g_accel_ch->task_pool);
	TAILQ_INSERT_TAIL(&g_accel_ch->batches, &batch, link);

	/* Call it again and the batch should complete and lists updated accordingly. */
	spdk_accel_task_complete(&accel_task, status);
	CU_ASSERT(batch.count == 0);
	CU_ASSERT(true == g_dummy_batch_cb_called);
	CU_ASSERT(true == TAILQ_EMPTY(&g_accel_ch->batches));

	expected_accel_task = TAILQ_FIRST(&g_accel_ch->task_pool);
	TAILQ_REMOVE(&g_accel_ch->task_pool, expected_accel_task, link);
	CU_ASSERT(expected_accel_task == &accel_task);

	expected_batch = TAILQ_FIRST(&g_accel_ch->batch_pool);
	TAILQ_REMOVE(&g_accel_ch->batch_pool, expected_batch, link);
	CU_ASSERT(expected_batch == &batch);
}

static void
test_spdk_accel_get_capabilities(void)
{
	uint64_t cap, expected_cap;

	/* Setup a few capabilites and make sure they are reported as expected. */
	g_accel_ch->engine = &g_accel_engine;
	expected_cap = ACCEL_COPY | ACCEL_DUALCAST | ACCEL_CRC32C;
	g_accel_ch->engine->capabilities = expected_cap;

	cap = spdk_accel_get_capabilities(g_ch);
	CU_ASSERT(cap == expected_cap);
}

static void
test_is_batch_valid(void)
{
	struct spdk_accel_batch batch = {};
	bool rc;

	/* This batch doesn't go with this channel. */
	batch.accel_ch = (struct accel_io_channel *)0xDEADBEEF;
	rc = _is_batch_valid(&batch, g_accel_ch);
	CU_ASSERT(rc == false);

	/* This one does. */
	batch.accel_ch = g_accel_ch;
	rc = _is_batch_valid(&batch, g_accel_ch);
	CU_ASSERT(rc == true);
}

static void
test_get_task(void)
{
	struct spdk_accel_batch batch = {};
	struct spdk_accel_task *task;
	struct spdk_accel_task _task;
	void *cb_arg = NULL;

	/* NULL batch should return NULL task. */
	task = _get_task(g_accel_ch, NULL, dummy_cb_fn, cb_arg);
	CU_ASSERT(task == NULL);

	/* valid batch with bogus accel_ch should return NULL task. */
	task = _get_task(g_accel_ch, &batch, dummy_cb_fn, cb_arg);
	CU_ASSERT(task == NULL);

	TAILQ_INIT(&g_accel_ch->task_pool);
	batch.accel_ch = g_accel_ch;

	/* no tasks left, return NULL. */
	task = _get_task(g_accel_ch, &batch, dummy_cb_fn, cb_arg);
	CU_ASSERT(task == NULL);

	_task.cb_fn = dummy_cb_fn;
	_task.cb_arg = cb_arg;
	_task.accel_ch = g_accel_ch;
	_task.batch = &batch;
	TAILQ_INSERT_TAIL(&g_accel_ch->task_pool, &_task, link);

	/* Get a valid task. */
	task = _get_task(g_accel_ch, &batch, dummy_cb_fn, cb_arg);
	CU_ASSERT(task == &_task);
	CU_ASSERT(_task.cb_fn == dummy_cb_fn);
	CU_ASSERT(_task.cb_arg == cb_arg);
	CU_ASSERT(_task.accel_ch == g_accel_ch);
	CU_ASSERT(_task.batch->count == 1);
}

static bool g_dummy_submit_called = false;
static int
dummy_submit_tasks(struct spdk_io_channel *ch, struct spdk_accel_task *first_task)
{
	g_dummy_submit_called = true;
	return 0;
}

static bool g_dummy_submit_cb_called = false;
static void
dummy_submit_cb_fn(void *cb_arg, int status)
{
	g_dummy_submit_cb_called = true;
	CU_ASSERT(status == 0);
}

#define TEST_SUBMIT_SIZE 64
static void
test_spdk_accel_submit_copy(void)
{
	const uint64_t nbytes = TEST_SUBMIT_SIZE;
	uint8_t dst[TEST_SUBMIT_SIZE];
	uint8_t src[TEST_SUBMIT_SIZE];
	void *cb_arg = NULL;
	int rc;
	struct spdk_accel_task task;
	struct spdk_accel_task *expected_accel_task = NULL;

	TAILQ_INIT(&g_accel_ch->task_pool);

	/* Fail with no tasks on _get_task() */
	rc = spdk_accel_submit_copy(g_ch, src, dst, nbytes, dummy_submit_cb_fn, cb_arg);
	CU_ASSERT(rc == -ENOMEM);

	task.cb_fn = dummy_submit_cb_fn;
	task.cb_arg = cb_arg;
	task.accel_ch = g_accel_ch;
	task.batch = NULL;
	TAILQ_INSERT_TAIL(&g_accel_ch->task_pool, &task, link);

	g_accel_ch->engine = &g_accel_engine;
	g_accel_ch->engine->capabilities = ACCEL_COPY;
	g_accel_ch->engine->submit_tasks = dummy_submit_tasks;

	/* HW accel submission OK. */
	rc = spdk_accel_submit_copy(g_ch, dst, src, nbytes, dummy_submit_cb_fn, cb_arg);
	CU_ASSERT(rc == 0);
	CU_ASSERT(task.dst == dst);
	CU_ASSERT(task.src == src);
	CU_ASSERT(task.op_code == ACCEL_OPCODE_MEMMOVE);
	CU_ASSERT(task.nbytes == nbytes);
	CU_ASSERT(g_dummy_submit_called == true);

	TAILQ_INSERT_TAIL(&g_accel_ch->task_pool, &task, link);
	/* reset values before next case */
	g_dummy_submit_called = false;
	g_accel_ch->engine->capabilities = 0;
	task.dst = 0;
	task.src = 0;
	task.op_code = 0xff;
	task.nbytes = 0;

	/* SW engine does copy. */
	rc = spdk_accel_submit_copy(g_ch, dst, src, nbytes, dummy_submit_cb_fn, cb_arg);
	CU_ASSERT(rc == 0);
	CU_ASSERT(task.dst == dst);
	CU_ASSERT(task.src == src);
	CU_ASSERT(task.op_code == ACCEL_OPCODE_MEMMOVE);
	CU_ASSERT(task.nbytes == nbytes);
	CU_ASSERT(g_dummy_submit_cb_called == false);
	CU_ASSERT(memcmp(dst, src, TEST_SUBMIT_SIZE) == 0);
	expected_accel_task = TAILQ_FIRST(&g_sw_ch->tasks_to_complete);
	TAILQ_REMOVE(&g_sw_ch->tasks_to_complete, expected_accel_task, link);
	CU_ASSERT(expected_accel_task == &task);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("accel", test_setup, test_cleanup);

	CU_ADD_TEST(suite, test_spdk_accel_hw_engine_register);
	CU_ADD_TEST(suite, test_accel_sw_register);
	CU_ADD_TEST(suite, test_accel_sw_unregister);
	CU_ADD_TEST(suite, test_is_supported);
	CU_ADD_TEST(suite, test_spdk_accel_task_complete);
	CU_ADD_TEST(suite, test_spdk_accel_get_capabilities);
	CU_ADD_TEST(suite, test_is_batch_valid);
	CU_ADD_TEST(suite, test_get_task);
	CU_ADD_TEST(suite, test_spdk_accel_submit_copy);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
