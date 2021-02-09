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
#include "common/lib/test_env.c"

#include "accel/accel_engine.c"

DEFINE_STUB(spdk_json_write_array_begin, int, (struct spdk_json_write_ctx *w), 0);
DEFINE_STUB(spdk_json_write_array_end, int, (struct spdk_json_write_ctx *w), 0);

static void
test_spdk_accel_hw_engine_register(void)
{
	struct spdk_accel_engine accel_engine;

	/* Run once with no engine assigned, assign it. */
	g_hw_accel_engine = NULL;
	spdk_accel_hw_engine_register(&accel_engine);
	CU_ASSERT(g_hw_accel_engine == &accel_engine);

	/* Run with one assigned, should not change. */
	spdk_accel_hw_engine_register(&accel_engine);
	CU_ASSERT(g_hw_accel_engine == &accel_engine);
}

static int
test_accel_sw_register(void)
{
	struct spdk_accel_engine accel_engine;

	/* Run once with no engine assigned, assign it. */
	g_sw_accel_engine = NULL;
	accel_sw_register(&accel_engine);
	CU_ASSERT(g_sw_accel_engine == &accel_engine);

	return 0;
}

static void
test_accel_sw_unregister(void)
{
	struct spdk_accel_engine accel_engine;

	/* Run once engine assigned, make sure it gets unassigned. */
	g_sw_accel_engine = &accel_engine;
	accel_sw_unregister();
	CU_ASSERT(g_sw_accel_engine == NULL);
}

static void
test_is_supported(void)
{
	struct spdk_accel_engine engine;

	engine.capabilities = ACCEL_COPY | ACCEL_DUALCAST | ACCEL_CRC32C;
	CU_ASSERT(_is_supported(&engine, ACCEL_COPY) == true);
	CU_ASSERT(_is_supported(&engine, ACCEL_FILL) == false);
	CU_ASSERT(_is_supported(&engine, ACCEL_DUALCAST) == true);
	CU_ASSERT(_is_supported(&engine, ACCEL_COMPARE) == false);
	CU_ASSERT(_is_supported(&engine, ACCEL_CRC32C) == true);
	CU_ASSERT(_is_supported(&engine, ACCEL_DIF) == false);
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
	struct accel_io_channel accel_ch = {};
	struct spdk_accel_task accel_task = {};
	struct spdk_accel_task *expected_accel_task = NULL;
	struct spdk_accel_batch	batch = {};
	struct spdk_accel_batch	*expected_batch = NULL;
	uint32_t cb_arg = DUMMY_ARG;
	int status = 0;

	accel_task.accel_ch = &accel_ch;
	accel_task.cb_fn = dummy_cb_fn;
	accel_task.cb_arg = &cb_arg;
	TAILQ_INIT(&accel_ch.task_pool);

	/* W/o batch, confirm cb is called and task added to list. */
	spdk_accel_task_complete(&accel_task, status);
	CU_ASSERT(g_dummy_cb_called == true);
	expected_accel_task = TAILQ_FIRST(&accel_ch.task_pool);
	TAILQ_REMOVE(&accel_ch.task_pool, expected_accel_task, link);
	CU_ASSERT(expected_accel_task == &accel_task);

	TAILQ_INIT(&accel_ch.task_pool);
	TAILQ_INIT(&accel_ch.batches);
	TAILQ_INIT(&accel_ch.batch_pool);
	batch.count = 2;
	batch.cb_fn = dummy_batch_cb_fn;
	batch.cb_arg = &cb_arg;
	accel_task.batch = &batch;

	/* W/batch, confirm task cb is called and task added to list.
	 * but batch not completed yet. */
	spdk_accel_task_complete(&accel_task, status);
	CU_ASSERT(batch.count == 1);
	CU_ASSERT(false == g_dummy_batch_cb_called);

	expected_accel_task = TAILQ_FIRST(&accel_ch.task_pool);
	TAILQ_REMOVE(&accel_ch.task_pool, expected_accel_task, link);
	CU_ASSERT(expected_accel_task == &accel_task);
	CU_ASSERT(true == TAILQ_EMPTY(&accel_ch.batch_pool));
	CU_ASSERT(true == TAILQ_EMPTY(&accel_ch.batches));

	TAILQ_INIT(&accel_ch.task_pool);
	TAILQ_INSERT_TAIL(&accel_ch.batches, &batch, link);

	/* Call it again and the batch should complete and lists updated accordingly. */
	spdk_accel_task_complete(&accel_task, status);
	CU_ASSERT(batch.count == 0);
	CU_ASSERT(true == g_dummy_batch_cb_called);
	CU_ASSERT(true == TAILQ_EMPTY(&accel_ch.batches));

	expected_accel_task = TAILQ_FIRST(&accel_ch.task_pool);
	TAILQ_REMOVE(&accel_ch.task_pool, expected_accel_task, link);
	CU_ASSERT(expected_accel_task == &accel_task);

	expected_batch = TAILQ_FIRST(&accel_ch.batch_pool);
	TAILQ_REMOVE(&accel_ch.batch_pool, expected_batch, link);
	CU_ASSERT(expected_batch == &batch);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("accel", NULL, NULL);

	CU_ADD_TEST(suite, test_spdk_accel_hw_engine_register);
	CU_ADD_TEST(suite, test_accel_sw_register);
	CU_ADD_TEST(suite, test_accel_sw_unregister);
	CU_ADD_TEST(suite, test_is_supported);
	CU_ADD_TEST(suite, test_spdk_accel_task_complete);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
