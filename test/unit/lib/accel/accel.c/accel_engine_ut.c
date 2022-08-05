/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk_cunit.h"
#include "spdk_internal/mock.h"
#include "spdk_internal/accel_engine.h"
#include "thread/thread_internal.h"
#include "common/lib/test_env.c"
#include "accel/accel_engine.c"
#include "unit/lib/json_mock.c"

#ifdef SPDK_CONFIG_PMDK
DEFINE_STUB(pmem_msync, int, (const void *addr, size_t len), 0);
DEFINE_STUB(pmem_memcpy_persist, void *, (void *pmemdest, const void *src, size_t len), NULL);
DEFINE_STUB(pmem_is_pmem, int, (const void *addr, size_t len), 0);
DEFINE_STUB(pmem_memset_persist, void *, (void *pmemdest, int c, size_t len), NULL);
#endif

/* global vars and setup/cleanup functions used for all test functions */
struct spdk_accel_module_if g_accel_module = {};
struct spdk_io_channel *g_ch = NULL;
struct accel_io_channel *g_accel_ch = NULL;
struct sw_accel_io_channel *g_sw_ch = NULL;
struct spdk_io_channel *g_engine_ch = NULL;

static uint64_t g_opc_mask = 0;

static uint64_t
_accel_op_to_bit(enum accel_opcode opc)
{
	return (1 << opc);
}

static bool
_supports_opcode(enum accel_opcode opc)
{
	if (_accel_op_to_bit(opc) & g_opc_mask) {
		return true;
	}
	return false;
}

static int
test_setup(void)
{
	int i;

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

	g_accel_module.submit_tasks = sw_accel_submit_tasks;
	g_accel_module.name = "software";
	for (i = 0; i < ACCEL_OPC_LAST; i++) {
		g_accel_ch->engine_ch[i] = g_engine_ch;
		g_engines_opc[i] = &g_accel_module;
	}
	g_sw_ch = (struct sw_accel_io_channel *)((char *)g_engine_ch + sizeof(
				struct spdk_io_channel));
	TAILQ_INIT(&g_sw_ch->tasks_to_complete);
	g_accel_module.supports_opcode = _supports_opcode;
	return 0;
}

static int
test_cleanup(void)
{
	free(g_ch);
	free(g_engine_ch);

	return 0;
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
	int flags = 0;

	TAILQ_INIT(&g_accel_ch->task_pool);

	/* Fail with no tasks on _get_task() */
	rc = spdk_accel_submit_copy(g_ch, src, dst, nbytes, flags, NULL, cb_arg);
	CU_ASSERT(rc == -ENOMEM);

	task.accel_ch = g_accel_ch;
	task.flags = 1;
	TAILQ_INSERT_TAIL(&g_accel_ch->task_pool, &task, link);

	/* submission OK. */
	rc = spdk_accel_submit_copy(g_ch, dst, src, nbytes, flags, NULL, cb_arg);
	CU_ASSERT(rc == 0);
	CU_ASSERT(task.dst == dst);
	CU_ASSERT(task.src == src);
	CU_ASSERT(task.op_code == ACCEL_OPC_COPY);
	CU_ASSERT(task.nbytes == nbytes);
	CU_ASSERT(task.flags == 0);
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
	int flags = 0;

	TAILQ_INIT(&g_accel_ch->task_pool);

	/* Dualcast requires 4K alignment on dst addresses,
	 * hence using the hard coded address to test the buffer alignment
	 */
	dst1 = (void *)0x5000;
	dst2 = (void *)0x60f0;
	src = calloc(1, TEST_SUBMIT_SIZE);
	SPDK_CU_ASSERT_FATAL(src != NULL);
	memset(src, 0x5A, TEST_SUBMIT_SIZE);

	/* This should fail since dst2 is not 4k aligned */
	rc = spdk_accel_submit_dualcast(g_ch, dst1, dst2, src, nbytes, flags, NULL, cb_arg);
	CU_ASSERT(rc == -EINVAL);

	dst1 = (void *)0x7010;
	dst2 = (void *)0x6000;
	/* This should fail since dst1 is not 4k aligned */
	rc = spdk_accel_submit_dualcast(g_ch, dst1, dst2, src, nbytes, flags, NULL, cb_arg);
	CU_ASSERT(rc == -EINVAL);

	/* Dualcast requires 4K alignment on dst addresses */
	dst1 = (void *)0x7000;
	dst2 = (void *)0x6000;
	/* Fail with no tasks on _get_task() */
	rc = spdk_accel_submit_dualcast(g_ch, dst1, dst2, src, nbytes, flags, NULL, cb_arg);
	CU_ASSERT(rc == -ENOMEM);

	TAILQ_INSERT_TAIL(&g_accel_ch->task_pool, &task, link);

	/* accel submission OK., since we test the SW path , need to use valid memory addresses
	 * cannot hardcode them anymore */
	dst1 = spdk_dma_zmalloc(nbytes, align, NULL);
	SPDK_CU_ASSERT_FATAL(dst1 != NULL);
	dst2 = spdk_dma_zmalloc(nbytes, align, NULL);
	SPDK_CU_ASSERT_FATAL(dst2 != NULL);
	/* SW engine does the dualcast. */
	rc = spdk_accel_submit_dualcast(g_ch, dst1, dst2, src, nbytes, flags, NULL, cb_arg);
	CU_ASSERT(rc == 0);
	CU_ASSERT(task.dst == dst1);
	CU_ASSERT(task.dst2 == dst2);
	CU_ASSERT(task.src == src);
	CU_ASSERT(task.op_code == ACCEL_OPC_DUALCAST);
	CU_ASSERT(task.nbytes == nbytes);
	CU_ASSERT(task.flags == 0);
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

	TAILQ_INIT(&g_accel_ch->task_pool);

	src1 = calloc(1, TEST_SUBMIT_SIZE);
	SPDK_CU_ASSERT_FATAL(src1 != NULL);
	src2 = calloc(1, TEST_SUBMIT_SIZE);
	SPDK_CU_ASSERT_FATAL(src2 != NULL);

	/* Fail with no tasks on _get_task() */
	rc = spdk_accel_submit_compare(g_ch, src1, src2, nbytes, NULL, cb_arg);
	CU_ASSERT(rc == -ENOMEM);

	TAILQ_INSERT_TAIL(&g_accel_ch->task_pool, &task, link);

	/* accel submission OK. */
	rc = spdk_accel_submit_compare(g_ch, src1, src2, nbytes, NULL, cb_arg);
	CU_ASSERT(rc == 0);
	CU_ASSERT(task.src == src1);
	CU_ASSERT(task.src2 == src2);
	CU_ASSERT(task.op_code == ACCEL_OPC_COMPARE);
	CU_ASSERT(task.nbytes == nbytes);
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
	uint64_t fill64;
	uint64_t nbytes = TEST_SUBMIT_SIZE;
	void *cb_arg = NULL;
	int rc;
	struct spdk_accel_task task;
	struct spdk_accel_task *expected_accel_task = NULL;
	int flags = 0;

	TAILQ_INIT(&g_accel_ch->task_pool);

	dst = calloc(1, TEST_SUBMIT_SIZE);
	SPDK_CU_ASSERT_FATAL(dst != NULL);
	src = calloc(1, TEST_SUBMIT_SIZE);
	SPDK_CU_ASSERT_FATAL(src != NULL);
	memset(src, fill, TEST_SUBMIT_SIZE);
	memset(&fill64, fill, sizeof(uint64_t));

	/* Fail with no tasks on _get_task() */
	rc = spdk_accel_submit_fill(g_ch, dst, fill, nbytes, flags, NULL, cb_arg);
	CU_ASSERT(rc == -ENOMEM);

	TAILQ_INSERT_TAIL(&g_accel_ch->task_pool, &task, link);

	/* accel submission OK. */
	rc = spdk_accel_submit_fill(g_ch, dst, fill, nbytes, flags, NULL, cb_arg);
	CU_ASSERT(rc == 0);
	CU_ASSERT(task.dst == dst);
	CU_ASSERT(task.fill_pattern == fill64);
	CU_ASSERT(task.op_code == ACCEL_OPC_FILL);
	CU_ASSERT(task.nbytes == nbytes);
	CU_ASSERT(task.flags == 0);

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

	TAILQ_INIT(&g_accel_ch->task_pool);

	/* Fail with no tasks on _get_task() */
	rc = spdk_accel_submit_crc32c(g_ch, &crc_dst, src, seed, nbytes, NULL, cb_arg);
	CU_ASSERT(rc == -ENOMEM);

	TAILQ_INSERT_TAIL(&g_accel_ch->task_pool, &task, link);

	/* accel submission OK. */
	rc = spdk_accel_submit_crc32c(g_ch, &crc_dst, src, seed, nbytes, NULL, cb_arg);
	CU_ASSERT(rc == 0);
	CU_ASSERT(task.crc_dst == &crc_dst);
	CU_ASSERT(task.src == src);
	CU_ASSERT(task.v.iovcnt == 0);
	CU_ASSERT(task.seed == seed);
	CU_ASSERT(task.op_code == ACCEL_OPC_CRC32C);
	CU_ASSERT(task.nbytes == nbytes);
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

	TAILQ_INIT(&g_accel_ch->task_pool);

	for (i = 0; i < iov_cnt; i++) {
		iov[i].iov_base = calloc(1, TEST_SUBMIT_SIZE);
		SPDK_CU_ASSERT_FATAL(iov[i].iov_base != NULL);
		iov[i].iov_len = TEST_SUBMIT_SIZE;
	}

	task.nbytes = TEST_SUBMIT_SIZE;
	TAILQ_INSERT_TAIL(&g_accel_ch->task_pool, &task, link);

	/* accel submission OK. */
	rc = spdk_accel_submit_crc32cv(g_ch, &crc_dst, iov, iov_cnt, seed, NULL, cb_arg);
	CU_ASSERT(rc == 0);
	CU_ASSERT(task.v.iovs == iov);
	CU_ASSERT(task.v.iovcnt == iov_cnt);
	CU_ASSERT(task.crc_dst == &crc_dst);
	CU_ASSERT(task.seed == seed);
	CU_ASSERT(task.op_code == ACCEL_OPC_CRC32C);
	CU_ASSERT(task.cb_arg == cb_arg);
	CU_ASSERT(task.nbytes == iov[0].iov_len);
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
	int flags = 0;

	TAILQ_INIT(&g_accel_ch->task_pool);

	/* Fail with no tasks on _get_task() */
	rc = spdk_accel_submit_copy_crc32c(g_ch, dst, src, &crc_dst, seed, nbytes, flags,
					   NULL, cb_arg);
	CU_ASSERT(rc == -ENOMEM);

	TAILQ_INSERT_TAIL(&g_accel_ch->task_pool, &task, link);

	/* accel submission OK. */
	rc = spdk_accel_submit_copy_crc32c(g_ch, dst, src, &crc_dst, seed, nbytes, flags,
					   NULL, cb_arg);
	CU_ASSERT(rc == 0);
	CU_ASSERT(task.dst == dst);
	CU_ASSERT(task.src == src);
	CU_ASSERT(task.crc_dst == &crc_dst);
	CU_ASSERT(task.v.iovcnt == 0);
	CU_ASSERT(task.seed == seed);
	CU_ASSERT(task.nbytes == nbytes);
	CU_ASSERT(task.flags == 0);
	CU_ASSERT(task.op_code == ACCEL_OPC_COPY_CRC32C);
	expected_accel_task = TAILQ_FIRST(&g_sw_ch->tasks_to_complete);
	TAILQ_REMOVE(&g_sw_ch->tasks_to_complete, expected_accel_task, link);
	CU_ASSERT(expected_accel_task == &task);
}

static void
test_spdk_accel_module_find_by_name(void)
{
	struct spdk_accel_module_if mod1 = {};
	struct spdk_accel_module_if mod2 = {};
	struct spdk_accel_module_if mod3 = {};
	struct spdk_accel_module_if *accel_module = NULL;

	mod1.name = "ioat";
	mod2.name = "idxd";
	mod3.name = "software";

	TAILQ_INIT(&spdk_accel_module_list);
	TAILQ_INSERT_TAIL(&spdk_accel_module_list, &mod1, tailq);
	TAILQ_INSERT_TAIL(&spdk_accel_module_list, &mod2, tailq);
	TAILQ_INSERT_TAIL(&spdk_accel_module_list, &mod3, tailq);

	/* Now let's find a valid engine */
	accel_module = _module_find_by_name("ioat");
	CU_ASSERT(accel_module != NULL);

	/* Try to find one that doesn't exist */
	accel_module = _module_find_by_name("XXX");
	CU_ASSERT(accel_module == NULL);
}

static void
test_spdk_accel_module_register(void)
{
	struct spdk_accel_module_if mod1 = {};
	struct spdk_accel_module_if mod2 = {};
	struct spdk_accel_module_if mod3 = {};
	struct spdk_accel_module_if mod4 = {};
	struct spdk_accel_module_if *accel_module = NULL;
	int i = 0;

	mod1.name = "ioat";
	mod2.name = "idxd";
	mod3.name = "software";
	mod4.name = "nothing";

	TAILQ_INIT(&spdk_accel_module_list);

	spdk_accel_module_list_add(&mod1);
	spdk_accel_module_list_add(&mod2);
	spdk_accel_module_list_add(&mod3);
	spdk_accel_module_list_add(&mod4);

	/* Now confirm they're in the right order. */
	TAILQ_FOREACH(accel_module, &spdk_accel_module_list, tailq) {
		switch (i++) {
		case 0:
			CU_ASSERT(strcmp(accel_module->name, "software") == 0);
			break;
		case 1:
			CU_ASSERT(strcmp(accel_module->name, "ioat") == 0);
			break;
		case 2:
			CU_ASSERT(strcmp(accel_module->name, "idxd") == 0);
			break;
		case 3:
			CU_ASSERT(strcmp(accel_module->name, "nothing") == 0);
			break;
		default:
			CU_ASSERT(false);
			break;
		}
	}
	CU_ASSERT(i == 4);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("accel", test_setup, test_cleanup);

	CU_ADD_TEST(suite, test_spdk_accel_task_complete);
	CU_ADD_TEST(suite, test_get_task);
	CU_ADD_TEST(suite, test_spdk_accel_submit_copy);
	CU_ADD_TEST(suite, test_spdk_accel_submit_dualcast);
	CU_ADD_TEST(suite, test_spdk_accel_submit_compare);
	CU_ADD_TEST(suite, test_spdk_accel_submit_fill);
	CU_ADD_TEST(suite, test_spdk_accel_submit_crc32c);
	CU_ADD_TEST(suite, test_spdk_accel_submit_crc32cv);
	CU_ADD_TEST(suite, test_spdk_accel_submit_copy_crc32c);
	CU_ADD_TEST(suite, test_spdk_accel_module_find_by_name);
	CU_ADD_TEST(suite, test_spdk_accel_module_register);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
