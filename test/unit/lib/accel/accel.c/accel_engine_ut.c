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
	g_accel_ch->sw_engine_ch = g_engine_ch;
	g_sw_ch = (struct sw_accel_io_channel *)((char *)g_accel_ch->sw_engine_ch + sizeof(
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

static void
test_spdk_accel_task_complete(void)
{
	struct spdk_accel_task accel_task = {};
	struct spdk_accel_task *expected_accel_task = NULL;
	uint32_t cb_arg = DUMMY_ARG;
	int status = 0;

	accel_task.accel_ch = g_accel_ch;
	accel_task.cb_fn = dummy_cb_fn;
	accel_task.cb_arg = &cb_arg;
	TAILQ_INIT(&g_accel_ch->task_pool);

	/* Confirm cb is called and task added to list. */
	spdk_accel_task_complete(&accel_task, status);
	CU_ASSERT(g_dummy_cb_called == true);
	expected_accel_task = TAILQ_FIRST(&g_accel_ch->task_pool);
	TAILQ_REMOVE(&g_accel_ch->task_pool, expected_accel_task, link);
	CU_ASSERT(expected_accel_task == &accel_task);
}

static void
test_spdk_accel_get_capabilities(void)
{
	uint64_t cap, expected_cap;

	/* Setup a few capabilities and make sure they are reported as expected. */
	g_accel_ch->engine = &g_accel_engine;
	expected_cap = ACCEL_COPY | ACCEL_DUALCAST | ACCEL_CRC32C;
	g_accel_ch->engine->capabilities = expected_cap;

	cap = spdk_accel_get_capabilities(g_ch);
	CU_ASSERT(cap == expected_cap);
}


static void
test_get_task(void)
{
	struct spdk_accel_task *task;
	struct spdk_accel_task _task;
	void *cb_arg = NULL;

	TAILQ_INIT(&g_accel_ch->task_pool);

	/* no tasks left, return NULL. */
	task = _get_task(g_accel_ch, dummy_cb_fn, cb_arg);
	CU_ASSERT(task == NULL);

	_task.cb_fn = dummy_cb_fn;
	_task.cb_arg = cb_arg;
	_task.accel_ch = g_accel_ch;
	TAILQ_INSERT_TAIL(&g_accel_ch->task_pool, &_task, link);

	/* Get a valid task. */
	task = _get_task(g_accel_ch, dummy_cb_fn, cb_arg);
	CU_ASSERT(task == &_task);
	CU_ASSERT(_task.cb_fn == dummy_cb_fn);
	CU_ASSERT(_task.cb_arg == cb_arg);
	CU_ASSERT(_task.accel_ch == g_accel_ch);
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
	uint8_t dst[TEST_SUBMIT_SIZE] = {0};
	uint8_t src[TEST_SUBMIT_SIZE] = {0};
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

static void
test_spdk_accel_submit_dualcast(void)
{
	void *dst1;
	void *dst2;
	void *src;
	uint32_t align = ALIGN_4K;
	uint64_t nbytes = TEST_SUBMIT_SIZE;
	void *cb_arg = NULL;
	int rc;
	struct spdk_accel_task task;
	struct spdk_accel_task *expected_accel_task = NULL;

	/* Dualcast requires 4K alignment on dst addresses,
	 * hence using the hard coded address to test the buffer alignment
	 */
	dst1 = (void *)0x5000;
	dst2 = (void *)0x60f0;
	src = calloc(1, TEST_SUBMIT_SIZE);
	SPDK_CU_ASSERT_FATAL(src != NULL);
	memset(src, 0x5A, TEST_SUBMIT_SIZE);

	TAILQ_INIT(&g_accel_ch->task_pool);

	/* This should fail since dst2 is not 4k aligned */
	rc = spdk_accel_submit_dualcast(g_ch, dst1, dst2, src, nbytes, dummy_submit_cb_fn,
					cb_arg);
	CU_ASSERT(rc == -EINVAL);

	dst1 = (void *)0x7010;
	dst2 = (void *)0x6000;
	/* This should fail since dst1 is not 4k aligned */
	rc = spdk_accel_submit_dualcast(g_ch, dst1, dst2, src, nbytes, dummy_submit_cb_fn,
					cb_arg);
	CU_ASSERT(rc == -EINVAL);

	/* Dualcast requires 4K alignment on dst addresses */
	dst1 = (void *)0x7000;
	dst2 = (void *)0x6000;
	/* Fail with no tasks on _get_task() */
	rc = spdk_accel_submit_dualcast(g_ch, dst1, dst2, src, nbytes, dummy_submit_cb_fn,
					cb_arg);
	CU_ASSERT(rc == -ENOMEM);

	task.cb_fn = dummy_submit_cb_fn;
	task.cb_arg = cb_arg;
	task.accel_ch = g_accel_ch;
	TAILQ_INSERT_TAIL(&g_accel_ch->task_pool, &task, link);

	g_accel_ch->engine = &g_accel_engine;
	g_accel_ch->engine->capabilities = ACCEL_DUALCAST;
	g_accel_ch->engine->submit_tasks = dummy_submit_tasks;

	/* HW accel submission OK. */
	rc = spdk_accel_submit_dualcast(g_ch, dst1, dst2, src, nbytes, dummy_submit_cb_fn,
					cb_arg);
	CU_ASSERT(rc == 0);
	CU_ASSERT(task.dst == dst1);
	CU_ASSERT(task.dst2 == dst2);
	CU_ASSERT(task.src == src);
	CU_ASSERT(task.op_code == ACCEL_OPCODE_DUALCAST);
	CU_ASSERT(task.nbytes == nbytes);
	CU_ASSERT(g_dummy_submit_called == true);

	TAILQ_INSERT_TAIL(&g_accel_ch->task_pool, &task, link);
	/* Reset values before next case */
	g_dummy_submit_called = false;
	g_accel_ch->engine->capabilities = 0;
	task.dst = 0;
	task.dst2 = 0;
	task.src = 0;
	task.op_code = 0xff;
	task.nbytes = 0;
	/* Since we test the SW path next, need to use valid memory addresses
	 * cannot hardcode them anymore
	 */
	dst1 = spdk_dma_zmalloc(nbytes, align, NULL);
	SPDK_CU_ASSERT_FATAL(dst1 != NULL);
	dst2 = spdk_dma_zmalloc(nbytes, align, NULL);
	SPDK_CU_ASSERT_FATAL(dst2 != NULL);
	/* SW engine does the dualcast. */
	rc = spdk_accel_submit_dualcast(g_ch, dst1, dst2, src, nbytes, dummy_submit_cb_fn,
					cb_arg);
	CU_ASSERT(rc == 0);
	CU_ASSERT(task.dst == dst1);
	CU_ASSERT(task.dst2 == dst2);
	CU_ASSERT(task.src == src);
	CU_ASSERT(task.op_code == ACCEL_OPCODE_DUALCAST);
	CU_ASSERT(task.nbytes == nbytes);
	CU_ASSERT(g_dummy_submit_cb_called == false);
	CU_ASSERT(memcmp(dst1, src, TEST_SUBMIT_SIZE) == 0);
	CU_ASSERT(memcmp(dst2, src, TEST_SUBMIT_SIZE) == 0);
	expected_accel_task = TAILQ_FIRST(&g_sw_ch->tasks_to_complete);
	TAILQ_REMOVE(&g_sw_ch->tasks_to_complete, expected_accel_task, link);
	CU_ASSERT(expected_accel_task == &task);

	free(src);
	spdk_free(dst1);
	spdk_free(dst2);
}

static void
test_spdk_accel_submit_compare(void)
{
	void *src1;
	void *src2;
	uint64_t nbytes = TEST_SUBMIT_SIZE;
	void *cb_arg = NULL;
	int rc;
	struct spdk_accel_task task;
	struct spdk_accel_task *expected_accel_task = NULL;

	src1 = calloc(1, TEST_SUBMIT_SIZE);
	SPDK_CU_ASSERT_FATAL(src1 != NULL);
	src2 = calloc(1, TEST_SUBMIT_SIZE);
	SPDK_CU_ASSERT_FATAL(src2 != NULL);

	/* Fail with no tasks on _get_task() */
	rc = spdk_accel_submit_compare(g_ch, src1, src2, nbytes, dummy_submit_cb_fn, cb_arg);
	CU_ASSERT(rc == -ENOMEM);

	TAILQ_INIT(&g_accel_ch->task_pool);
	task.cb_fn = dummy_submit_cb_fn;
	task.cb_arg = cb_arg;
	task.accel_ch = g_accel_ch;
	TAILQ_INSERT_TAIL(&g_accel_ch->task_pool, &task, link);

	g_accel_ch->engine = &g_accel_engine;
	g_accel_ch->engine->capabilities = ACCEL_COMPARE;
	g_accel_ch->engine->submit_tasks = dummy_submit_tasks;

	/* HW accel submission OK. */
	rc = spdk_accel_submit_compare(g_ch, src1, src2, nbytes, dummy_submit_cb_fn, cb_arg);
	CU_ASSERT(rc == 0);
	CU_ASSERT(task.src == src1);
	CU_ASSERT(task.src2 == src2);
	CU_ASSERT(task.op_code == ACCEL_OPCODE_COMPARE);
	CU_ASSERT(task.nbytes == nbytes);
	CU_ASSERT(g_dummy_submit_called == true);

	TAILQ_INSERT_TAIL(&g_accel_ch->task_pool, &task, link);
	/* Reset values before next case */
	g_dummy_submit_called = false;
	g_accel_ch->engine->capabilities = 0;
	task.src = 0;
	task.src2 = 0;
	task.op_code = 0xff;
	task.nbytes = 0;

	memset(src1, 0x5A, TEST_SUBMIT_SIZE);
	memset(src2, 0x5A, TEST_SUBMIT_SIZE);

	/* SW engine does compare. */
	rc = spdk_accel_submit_compare(g_ch, src1, src2, nbytes, dummy_submit_cb_fn, cb_arg);
	CU_ASSERT(rc == 0);
	CU_ASSERT(task.src == src1);
	CU_ASSERT(task.src2 == src2);
	CU_ASSERT(task.op_code == ACCEL_OPCODE_COMPARE);
	CU_ASSERT(task.nbytes == nbytes);
	CU_ASSERT(g_dummy_submit_cb_called == false);
	CU_ASSERT(memcmp(src1, src2, TEST_SUBMIT_SIZE) == 0);
	expected_accel_task = TAILQ_FIRST(&g_sw_ch->tasks_to_complete);
	TAILQ_REMOVE(&g_sw_ch->tasks_to_complete, expected_accel_task, link);
	CU_ASSERT(expected_accel_task == &task);

	free(src1);
	free(src2);
}

static void
test_spdk_accel_submit_fill(void)
{
	void *dst;
	void *src;
	uint8_t fill = 0xf;
	uint64_t nbytes = TEST_SUBMIT_SIZE;
	void *cb_arg = NULL;
	int rc;
	struct spdk_accel_task task;
	struct spdk_accel_task *expected_accel_task = NULL;

	dst = calloc(1, TEST_SUBMIT_SIZE);
	SPDK_CU_ASSERT_FATAL(dst != NULL);
	src = calloc(1, TEST_SUBMIT_SIZE);
	SPDK_CU_ASSERT_FATAL(src != NULL);
	memset(src, fill, TEST_SUBMIT_SIZE);

	/* Fail with no tasks on _get_task() */
	rc = spdk_accel_submit_fill(g_ch, dst, fill, nbytes, dummy_submit_cb_fn, cb_arg);
	CU_ASSERT(rc == -ENOMEM);

	TAILQ_INIT(&g_accel_ch->task_pool);
	task.cb_fn = dummy_submit_cb_fn;
	task.cb_arg = cb_arg;
	task.accel_ch = g_accel_ch;
	TAILQ_INSERT_TAIL(&g_accel_ch->task_pool, &task, link);

	g_accel_ch->engine = &g_accel_engine;
	g_accel_ch->engine->capabilities = ACCEL_FILL;
	g_accel_ch->engine->submit_tasks = dummy_submit_tasks;

	/* HW accel submission OK. */
	rc = spdk_accel_submit_fill(g_ch, dst, fill, nbytes, dummy_submit_cb_fn, cb_arg);
	CU_ASSERT(rc == 0);
	CU_ASSERT(task.dst == dst);
	CU_ASSERT(task.fill_pattern == fill);
	CU_ASSERT(task.op_code == ACCEL_OPCODE_MEMFILL);
	CU_ASSERT(task.nbytes == nbytes);
	CU_ASSERT(g_dummy_submit_called == true);

	TAILQ_INSERT_TAIL(&g_accel_ch->task_pool, &task, link);
	/* Reset values before next case */
	g_dummy_submit_called = false;
	g_accel_ch->engine->capabilities = 0;
	task.dst = 0;
	task.fill_pattern = 0;
	task.op_code = 0xff;
	task.nbytes = 0;

	/* SW engine does the fill. */
	rc = spdk_accel_submit_fill(g_ch, dst, fill, nbytes, dummy_submit_cb_fn, cb_arg);
	CU_ASSERT(rc == 0);
	CU_ASSERT(task.dst == dst);
	CU_ASSERT(task.fill_pattern == fill);
	CU_ASSERT(task.op_code == ACCEL_OPCODE_MEMFILL);
	CU_ASSERT(task.nbytes == nbytes);
	CU_ASSERT(g_dummy_submit_cb_called == false);
	CU_ASSERT(memcmp(dst, src, TEST_SUBMIT_SIZE) == 0);
	expected_accel_task = TAILQ_FIRST(&g_sw_ch->tasks_to_complete);
	TAILQ_REMOVE(&g_sw_ch->tasks_to_complete, expected_accel_task, link);
	CU_ASSERT(expected_accel_task == &task);

	free(dst);
	free(src);
}

static void
test_spdk_accel_submit_crc32c(void)
{
	const uint64_t nbytes = TEST_SUBMIT_SIZE;
	uint32_t crc_dst;
	uint8_t src[TEST_SUBMIT_SIZE];
	uint32_t seed = 1;
	void *cb_arg = NULL;
	int rc;
	struct spdk_accel_task task;
	struct spdk_accel_task *expected_accel_task = NULL;

	/* Fail with no tasks on _get_task() */
	rc = spdk_accel_submit_crc32c(g_ch, &crc_dst, src, seed, nbytes, dummy_submit_cb_fn, cb_arg);
	CU_ASSERT(rc == -ENOMEM);

	TAILQ_INIT(&g_accel_ch->task_pool);
	task.cb_fn = dummy_submit_cb_fn;
	task.cb_arg = cb_arg;
	task.accel_ch = g_accel_ch;
	TAILQ_INSERT_TAIL(&g_accel_ch->task_pool, &task, link);

	g_accel_ch->engine = &g_accel_engine;
	g_accel_ch->engine->capabilities = ACCEL_CRC32C;
	g_accel_ch->engine->submit_tasks = dummy_submit_tasks;

	/* HW accel submission OK. */
	rc = spdk_accel_submit_crc32c(g_ch, &crc_dst, src, seed, nbytes, dummy_submit_cb_fn, cb_arg);
	CU_ASSERT(rc == 0);
	CU_ASSERT(task.crc_dst == &crc_dst);
	CU_ASSERT(task.src == src);
	CU_ASSERT(task.v.iovcnt == 0);
	CU_ASSERT(task.seed == seed);
	CU_ASSERT(task.op_code == ACCEL_OPCODE_CRC32C);
	CU_ASSERT(task.nbytes == nbytes);
	CU_ASSERT(g_dummy_submit_called == true);

	TAILQ_INSERT_TAIL(&g_accel_ch->task_pool, &task, link);
	/* Reset values before next case */
	g_dummy_submit_called = false;
	g_accel_ch->engine->capabilities = 0;
	task.crc_dst = 0;
	task.src = 0;
	task.seed = 0;
	task.op_code = 0xff;
	task.nbytes = 0;

	/* SW engine does crc. */
	rc = spdk_accel_submit_crc32c(g_ch, &crc_dst, src, seed, nbytes, dummy_submit_cb_fn, cb_arg);
	CU_ASSERT(rc == 0);
	CU_ASSERT(task.crc_dst == &crc_dst);
	CU_ASSERT(task.src == src);
	CU_ASSERT(task.v.iovcnt == 0);
	CU_ASSERT(task.seed == seed);
	CU_ASSERT(task.op_code == ACCEL_OPCODE_CRC32C);
	CU_ASSERT(task.nbytes == nbytes);
	CU_ASSERT(g_dummy_submit_cb_called == false);
	expected_accel_task = TAILQ_FIRST(&g_sw_ch->tasks_to_complete);
	TAILQ_REMOVE(&g_sw_ch->tasks_to_complete, expected_accel_task, link);
	CU_ASSERT(expected_accel_task == &task);
}

static void
test_spdk_accel_submit_crc32c_hw_engine_unsupported(void)
{
	const uint64_t nbytes = TEST_SUBMIT_SIZE;
	uint32_t crc_dst;
	uint8_t src[TEST_SUBMIT_SIZE];
	uint32_t seed = 1;
	void *cb_arg = NULL;
	int rc;
	struct spdk_accel_task task;
	struct spdk_accel_task *expected_accel_task = NULL;

	/* Fail with no tasks on _get_task() */
	rc = spdk_accel_submit_crc32c(g_ch, &crc_dst, src, seed, nbytes, dummy_submit_cb_fn, cb_arg);
	CU_ASSERT(rc == -ENOMEM);

	TAILQ_INIT(&g_accel_ch->task_pool);
	task.cb_fn = dummy_submit_cb_fn;
	task.cb_arg = cb_arg;
	task.accel_ch = g_accel_ch;
	TAILQ_INSERT_TAIL(&g_accel_ch->task_pool, &task, link);

	g_accel_ch->engine = &g_accel_engine;
	/* HW engine only supports COPY and does not support CRC */
	g_accel_ch->engine->capabilities = ACCEL_COPY;
	g_accel_ch->engine->submit_tasks = dummy_submit_tasks;

	/* Summit to HW engine while eventually handled by SW engine. */
	rc = spdk_accel_submit_crc32c(g_ch, &crc_dst, src, seed, nbytes, dummy_submit_cb_fn, cb_arg);
	CU_ASSERT(rc == 0);
	CU_ASSERT(task.crc_dst == &crc_dst);
	CU_ASSERT(task.src == src);
	CU_ASSERT(task.v.iovcnt == 0);
	CU_ASSERT(task.seed == seed);
	CU_ASSERT(task.op_code == ACCEL_OPCODE_CRC32C);
	CU_ASSERT(task.nbytes == nbytes);
	/* Not set in HW engine callback while handled by SW engine instead. */
	CU_ASSERT(g_dummy_submit_called == false);

	/* SW engine does crc. */
	expected_accel_task = TAILQ_FIRST(&g_sw_ch->tasks_to_complete);
	TAILQ_REMOVE(&g_sw_ch->tasks_to_complete, expected_accel_task, link);
	CU_ASSERT(expected_accel_task == &task);
}

static void
test_spdk_accel_submit_crc32cv(void)
{
	uint32_t crc_dst;
	uint32_t seed = 0;
	uint32_t iov_cnt = 32;
	void *cb_arg = NULL;
	int rc;
	uint32_t i = 0;
	struct spdk_accel_task task;
	struct iovec iov[32];
	struct spdk_accel_task *expected_accel_task = NULL;

	for (i = 0; i < iov_cnt; i++) {
		iov[i].iov_base = calloc(1, TEST_SUBMIT_SIZE);
		SPDK_CU_ASSERT_FATAL(iov[i].iov_base != NULL);
		iov[i].iov_len = TEST_SUBMIT_SIZE;
	}

	TAILQ_INIT(&g_accel_ch->task_pool);
	task.cb_fn = dummy_submit_cb_fn;
	task.cb_arg = cb_arg;
	task.accel_ch = g_accel_ch;
	task.nbytes = TEST_SUBMIT_SIZE;
	TAILQ_INSERT_TAIL(&g_accel_ch->task_pool, &task, link);

	g_accel_ch->engine = &g_accel_engine;
	g_accel_ch->engine->capabilities = ACCEL_CRC32C;
	g_accel_ch->engine->submit_tasks = dummy_submit_tasks;

	/* HW accel submission OK. */
	rc = spdk_accel_submit_crc32cv(g_ch, &crc_dst, iov, iov_cnt, seed, dummy_submit_cb_fn, cb_arg);
	CU_ASSERT(rc == 0);
	CU_ASSERT(task.v.iovs == iov);
	CU_ASSERT(task.v.iovcnt == iov_cnt);
	CU_ASSERT(task.crc_dst == &crc_dst);
	CU_ASSERT(task.seed == seed);
	CU_ASSERT(task.op_code == ACCEL_OPCODE_CRC32C);
	CU_ASSERT(g_dummy_submit_called == true);
	CU_ASSERT(task.cb_fn == dummy_submit_cb_fn);
	CU_ASSERT(task.cb_arg == cb_arg);
	CU_ASSERT(task.nbytes == iov[0].iov_len);

	TAILQ_INSERT_TAIL(&g_accel_ch->task_pool, &task, link);
	g_dummy_submit_called = false;
	g_accel_ch->engine->capabilities = 0;
	task.v.iovs = 0;
	task.v.iovcnt = 0;
	task.crc_dst = 0;
	task.seed = 0;
	task.op_code = 0xff;

	/* SW engine submit crc. */
	rc = spdk_accel_submit_crc32cv(g_ch, &crc_dst, iov, iov_cnt, seed, dummy_submit_cb_fn, cb_arg);
	CU_ASSERT(rc == 0);
	CU_ASSERT(task.v.iovs == iov);
	CU_ASSERT(task.v.iovcnt == iov_cnt);
	CU_ASSERT(task.crc_dst == &crc_dst);
	CU_ASSERT(task.seed == seed);
	CU_ASSERT(task.op_code == ACCEL_OPCODE_CRC32C);
	CU_ASSERT(g_dummy_submit_cb_called == false);

	expected_accel_task = TAILQ_FIRST(&g_sw_ch->tasks_to_complete);
	TAILQ_REMOVE(&g_sw_ch->tasks_to_complete, expected_accel_task, link);
	CU_ASSERT(expected_accel_task == &task);

	for (i = 0; i < iov_cnt; i++) {
		free(iov[i].iov_base);
	}
}

static void
test_spdk_accel_submit_copy_crc32c(void)
{
	const uint64_t nbytes = TEST_SUBMIT_SIZE;
	uint32_t crc_dst;
	uint8_t dst[TEST_SUBMIT_SIZE];
	uint8_t src[TEST_SUBMIT_SIZE];
	uint32_t seed = 0;
	void *cb_arg = NULL;
	int rc;
	struct spdk_accel_task task;
	struct spdk_accel_task *expected_accel_task = NULL;

	/* Fail with no tasks on _get_task() */
	rc = spdk_accel_submit_copy_crc32c(g_ch, dst, src, &crc_dst, seed, nbytes, dummy_submit_cb_fn,
					   cb_arg);
	CU_ASSERT(rc == -ENOMEM);

	TAILQ_INIT(&g_accel_ch->task_pool);
	task.cb_fn = dummy_submit_cb_fn;
	task.cb_arg = cb_arg;
	task.accel_ch = g_accel_ch;
	TAILQ_INSERT_TAIL(&g_accel_ch->task_pool, &task, link);

	g_accel_ch->engine = &g_accel_engine;
	g_accel_ch->engine->capabilities = ACCEL_COPY_CRC32C;
	g_accel_ch->engine->submit_tasks = dummy_submit_tasks;

	/* HW accel submission OK. */
	rc = spdk_accel_submit_copy_crc32c(g_ch, dst, src, &crc_dst, seed, nbytes, dummy_submit_cb_fn,
					   cb_arg);
	CU_ASSERT(rc == 0);
	CU_ASSERT(task.dst == dst);
	CU_ASSERT(task.src == src);
	CU_ASSERT(task.crc_dst == &crc_dst);
	CU_ASSERT(task.v.iovcnt == 0);
	CU_ASSERT(task.seed == seed);
	CU_ASSERT(task.nbytes == nbytes);
	CU_ASSERT(task.op_code == ACCEL_OPCODE_COPY_CRC32C);
	CU_ASSERT(g_dummy_submit_called == true);

	TAILQ_INSERT_TAIL(&g_accel_ch->task_pool, &task, link);
	g_dummy_submit_called = false;
	task.dst = 0;
	task.src = 0;
	task.crc_dst = 0;
	task.v.iovcnt = 0;
	task.seed = 0;
	task.nbytes = 0;
	task.op_code = 0xff;
	g_accel_ch->engine->capabilities = 0;
	memset(src, 0x5A, TEST_SUBMIT_SIZE);

	/* SW engine does copy crc. */
	rc = spdk_accel_submit_copy_crc32c(g_ch, dst, src, &crc_dst, seed, nbytes, dummy_submit_cb_fn,
					   cb_arg);
	CU_ASSERT(rc == 0);
	CU_ASSERT(memcmp(dst, src, TEST_SUBMIT_SIZE) == 0);
	CU_ASSERT(task.dst == dst);
	CU_ASSERT(task.src == src);
	CU_ASSERT(task.crc_dst == &crc_dst);
	CU_ASSERT(task.v.iovcnt == 0);
	CU_ASSERT(task.seed == seed);
	CU_ASSERT(task.nbytes == nbytes);
	CU_ASSERT(task.op_code == ACCEL_OPCODE_COPY_CRC32C);
	CU_ASSERT(g_dummy_submit_cb_called == false);
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
	CU_ADD_TEST(suite, test_get_task);
	CU_ADD_TEST(suite, test_spdk_accel_submit_copy);
	CU_ADD_TEST(suite, test_spdk_accel_submit_dualcast);
	CU_ADD_TEST(suite, test_spdk_accel_submit_compare);
	CU_ADD_TEST(suite, test_spdk_accel_submit_fill);
	CU_ADD_TEST(suite, test_spdk_accel_submit_crc32c);
	CU_ADD_TEST(suite, test_spdk_accel_submit_crc32c_hw_engine_unsupported);
	CU_ADD_TEST(suite, test_spdk_accel_submit_crc32cv);
	CU_ADD_TEST(suite, test_spdk_accel_submit_copy_crc32c);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
