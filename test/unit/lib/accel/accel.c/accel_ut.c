/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk_cunit.h"
#include "spdk_internal/mock.h"
#include "spdk_internal/accel_module.h"
#include "thread/thread_internal.h"
#include "common/lib/ut_multithread.c"
#include "accel/accel.c"
#include "accel/accel_sw.c"
#include "unit/lib/json_mock.c"

#ifdef SPDK_CONFIG_PMDK
DEFINE_STUB(pmem_msync, int, (const void *addr, size_t len), 0);
DEFINE_STUB(pmem_memcpy_persist, void *, (void *pmemdest, const void *src, size_t len), NULL);
DEFINE_STUB(pmem_is_pmem, int, (const void *addr, size_t len), 0);
DEFINE_STUB(pmem_memset_persist, void *, (void *pmemdest, int c, size_t len), NULL);
#endif
DEFINE_STUB(spdk_memory_domain_create, int,
	    (struct spdk_memory_domain **domain, enum spdk_dma_device_type type,
	     struct spdk_memory_domain_ctx *ctx, const char *id), 0);
DEFINE_STUB_V(spdk_memory_domain_destroy, (struct spdk_memory_domain *domain));

/* global vars and setup/cleanup functions used for all test functions */
struct spdk_accel_module_if g_module = {};
struct spdk_io_channel *g_ch = NULL;
struct accel_io_channel *g_accel_ch = NULL;
struct sw_accel_io_channel *g_sw_ch = NULL;
struct spdk_io_channel *g_module_ch = NULL;

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
	g_module_ch = calloc(1, sizeof(struct spdk_io_channel) + sizeof(struct sw_accel_io_channel));
	if (g_module_ch == NULL) {
		CU_ASSERT(false);
		return -1;
	}

	g_module.submit_tasks = sw_accel_submit_tasks;
	g_module.name = "software";
	for (i = 0; i < ACCEL_OPC_LAST; i++) {
		g_accel_ch->module_ch[i] = g_module_ch;
		g_modules_opc[i] = &g_module;
	}
	g_sw_ch = (struct sw_accel_io_channel *)((char *)g_module_ch + sizeof(
				struct spdk_io_channel));
	TAILQ_INIT(&g_sw_ch->tasks_to_complete);
	g_module.supports_opcode = _supports_opcode;
	return 0;
}

static int
test_cleanup(void)
{
	free(g_ch);
	free(g_module_ch);

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
	/* SW module does the dualcast. */
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
	CU_ASSERT(task.s.iovcnt == 0);
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
	CU_ASSERT(task.s.iovs == iov);
	CU_ASSERT(task.s.iovcnt == iov_cnt);
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
	CU_ASSERT(task.s.iovcnt == 0);
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

struct ut_sequence {
	bool complete;
	int status;
};

static void
ut_sequence_step_cb(void *cb_arg)
{
	int *completed = cb_arg;

	(*completed)++;
}

static void
ut_sequence_complete_cb(void *cb_arg, int status)
{
	struct ut_sequence *seq = cb_arg;

	seq->complete = true;
	seq->status = status;
}

static void
test_sequence_fill_copy(void)
{
	struct spdk_accel_sequence *seq = NULL;
	struct spdk_io_channel *ioch;
	struct ut_sequence ut_seq;
	char buf[4096], tmp[2][4096], expected[4096];
	struct iovec src_iovs[2], dst_iovs[2];
	int rc, completed;

	ioch = spdk_accel_get_io_channel();
	SPDK_CU_ASSERT_FATAL(ioch != NULL);

	/* First check the simplest case - single task in a sequence */
	memset(buf, 0, sizeof(buf));
	memset(expected, 0xa5, sizeof(expected));
	completed = 0;
	rc = spdk_accel_append_fill(&seq, ioch, buf, sizeof(buf), NULL, NULL, 0xa5, 0,
				    ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(completed, 0);

	ut_seq.complete = false;
	rc = spdk_accel_sequence_finish(seq, ut_sequence_complete_cb, &ut_seq);
	CU_ASSERT_EQUAL(rc, 0);

	poll_threads();
	CU_ASSERT_EQUAL(completed, 1);
	CU_ASSERT(ut_seq.complete);
	CU_ASSERT_EQUAL(ut_seq.status, 0);
	CU_ASSERT_EQUAL(memcmp(buf, expected, sizeof(buf)), 0);

	/* Check a single copy operation */
	memset(buf, 0, sizeof(buf));
	memset(tmp[0], 0xa5, sizeof(tmp[0]));
	memset(expected, 0xa5, sizeof(expected));
	completed = 0;
	seq = NULL;

	dst_iovs[0].iov_base = buf;
	dst_iovs[0].iov_len = sizeof(buf);
	src_iovs[0].iov_base = tmp[0];
	src_iovs[0].iov_len = sizeof(tmp[0]);

	rc = spdk_accel_append_copy(&seq, ioch, &dst_iovs[0], 1, NULL, NULL,
				    &src_iovs[0], 1, NULL, NULL, 0,
				    ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	ut_seq.complete = false;
	rc = spdk_accel_sequence_finish(seq, ut_sequence_complete_cb, &ut_seq);
	CU_ASSERT_EQUAL(rc, 0);

	poll_threads();
	CU_ASSERT_EQUAL(completed, 1);
	CU_ASSERT(ut_seq.complete);
	CU_ASSERT_EQUAL(ut_seq.status, 0);
	CU_ASSERT_EQUAL(memcmp(buf, expected, sizeof(buf)), 0);

	/* Check multiple fill operations */
	memset(buf, 0, sizeof(buf));
	memset(expected, 0xfe, 4096);
	memset(expected, 0xde, 2048);
	memset(expected, 0xa5, 1024);
	seq = NULL;
	completed = 0;
	rc = spdk_accel_append_fill(&seq, ioch, buf, 4096, NULL, NULL, 0xfe, 0,
				    ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);
	rc = spdk_accel_append_fill(&seq, ioch, buf, 2048, NULL, NULL, 0xde, 0,
				    ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);
	rc = spdk_accel_append_fill(&seq, ioch, buf, 1024, NULL, NULL, 0xa5, 0,
				    ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	ut_seq.complete = false;
	rc = spdk_accel_sequence_finish(seq, ut_sequence_complete_cb, &ut_seq);
	CU_ASSERT_EQUAL(rc, 0);

	poll_threads();
	CU_ASSERT_EQUAL(completed, 3);
	CU_ASSERT(ut_seq.complete);
	CU_ASSERT_EQUAL(ut_seq.status, 0);
	CU_ASSERT_EQUAL(memcmp(buf, expected, sizeof(buf)), 0);

	/* Check multiple copy operations */
	memset(buf, 0, sizeof(buf));
	memset(tmp[0], 0, sizeof(tmp[0]));
	memset(tmp[1], 0, sizeof(tmp[1]));
	memset(expected, 0xa5, sizeof(expected));
	seq = NULL;
	completed = 0;

	rc = spdk_accel_append_fill(&seq, ioch, tmp[0], sizeof(tmp[0]), NULL, NULL, 0xa5, 0,
				    ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	dst_iovs[0].iov_base = tmp[1];
	dst_iovs[0].iov_len = sizeof(tmp[1]);
	src_iovs[0].iov_base = tmp[0];
	src_iovs[0].iov_len = sizeof(tmp[0]);
	rc = spdk_accel_append_copy(&seq, ioch, &dst_iovs[0], 1, NULL, NULL,
				    &src_iovs[0], 1, NULL, NULL, 0,
				    ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	dst_iovs[1].iov_base = buf;
	dst_iovs[1].iov_len = sizeof(buf);
	src_iovs[1].iov_base = tmp[1];
	src_iovs[1].iov_len = sizeof(tmp[1]);
	rc = spdk_accel_append_copy(&seq, ioch, &dst_iovs[1], 1, NULL, NULL,
				    &src_iovs[1], 1, NULL, NULL, 0,
				    ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	ut_seq.complete = false;
	rc = spdk_accel_sequence_finish(seq, ut_sequence_complete_cb, &ut_seq);
	CU_ASSERT_EQUAL(rc, 0);

	poll_threads();
	CU_ASSERT_EQUAL(completed, 3);
	CU_ASSERT(ut_seq.complete);
	CU_ASSERT_EQUAL(ut_seq.status, 0);
	CU_ASSERT_EQUAL(memcmp(buf, expected, sizeof(buf)), 0);

	/* Check that adding a copy operation at the end will change destination buffer */
	memset(buf, 0, sizeof(buf));
	memset(tmp[0], 0, sizeof(tmp[0]));
	memset(expected, 0xa5, sizeof(buf));
	seq = NULL;
	completed = 0;
	rc = spdk_accel_append_fill(&seq, ioch, tmp[0], sizeof(tmp[0]), NULL, NULL, 0xa5, 0,
				    ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	dst_iovs[0].iov_base = buf;
	dst_iovs[0].iov_len = sizeof(buf);
	src_iovs[0].iov_base = tmp[0];
	src_iovs[0].iov_len = sizeof(tmp[0]);
	rc = spdk_accel_append_copy(&seq, ioch, &dst_iovs[0], 1, NULL, NULL,
				    &src_iovs[0], 1, NULL, NULL, 0,
				    ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	ut_seq.complete = false;
	rc = spdk_accel_sequence_finish(seq, ut_sequence_complete_cb, &ut_seq);
	CU_ASSERT_EQUAL(rc, 0);

	poll_threads();
	CU_ASSERT_EQUAL(completed, 2);
	CU_ASSERT(ut_seq.complete);
	CU_ASSERT_EQUAL(ut_seq.status, 0);
	CU_ASSERT_EQUAL(memcmp(buf, expected, sizeof(buf)), 0);

	/* Check that it's also possible to add copy operation at the beginning */
	memset(buf, 0, sizeof(buf));
	memset(tmp[0], 0xde, sizeof(tmp[0]));
	memset(tmp[1], 0, sizeof(tmp[1]));
	memset(expected, 0xa5, sizeof(expected));
	seq = NULL;
	completed = 0;

	dst_iovs[0].iov_base = tmp[1];
	dst_iovs[0].iov_len = sizeof(tmp[1]);
	src_iovs[0].iov_base = tmp[0];
	src_iovs[0].iov_len = sizeof(tmp[0]);
	rc = spdk_accel_append_copy(&seq, ioch, &dst_iovs[0], 1, NULL, NULL,
				    &src_iovs[0], 1, NULL, NULL, 0,
				    ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	rc = spdk_accel_append_fill(&seq, ioch, tmp[1], sizeof(tmp[1]), NULL, NULL, 0xa5, 0,
				    ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	dst_iovs[1].iov_base = buf;
	dst_iovs[1].iov_len = sizeof(buf);
	src_iovs[1].iov_base = tmp[1];
	src_iovs[1].iov_len = sizeof(tmp[1]);
	rc = spdk_accel_append_copy(&seq, ioch, &dst_iovs[1], 1, NULL, NULL,
				    &src_iovs[1], 1, NULL, NULL, 0,
				    ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	ut_seq.complete = false;
	rc = spdk_accel_sequence_finish(seq, ut_sequence_complete_cb, &ut_seq);
	CU_ASSERT_EQUAL(rc, 0);

	poll_threads();
	CU_ASSERT_EQUAL(completed, 3);
	CU_ASSERT(ut_seq.complete);
	CU_ASSERT_EQUAL(ut_seq.status, 0);
	CU_ASSERT_EQUAL(memcmp(buf, expected, sizeof(buf)), 0);

	spdk_put_io_channel(ioch);
	poll_threads();
}

static void
test_sequence_abort(void)
{
	struct spdk_accel_sequence *seq = NULL;
	struct spdk_io_channel *ioch;
	char buf[4096], tmp[2][4096], expected[4096];
	struct iovec src_iovs[2], dst_iovs[2];
	int rc, completed;

	ioch = spdk_accel_get_io_channel();
	SPDK_CU_ASSERT_FATAL(ioch != NULL);

	/* Check that aborting a sequence calls operation's callback, the operation is not executed
	 * and the sequence is freed
	 */
	memset(buf, 0, sizeof(buf));
	memset(expected, 0, sizeof(buf));
	completed = 0;
	seq = NULL;
	rc = spdk_accel_append_fill(&seq, ioch, buf, sizeof(buf), NULL, NULL, 0xa5, 0,
				    ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	spdk_accel_sequence_abort(seq);
	CU_ASSERT_EQUAL(completed, 1);
	CU_ASSERT_EQUAL(memcmp(buf, expected, sizeof(buf)), 0);

	/* Check sequence with multiple operations */
	memset(buf, 0, sizeof(buf));
	memset(expected, 0, sizeof(buf));
	completed = 0;
	seq = NULL;

	dst_iovs[0].iov_base = tmp[1];
	dst_iovs[0].iov_len = sizeof(tmp[1]);
	src_iovs[0].iov_base = tmp[0];
	src_iovs[0].iov_len = sizeof(tmp[0]);
	rc = spdk_accel_append_copy(&seq, ioch, &dst_iovs[0], 1, NULL, NULL,
				    &src_iovs[0], 1, NULL, NULL, 0,
				    ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	rc = spdk_accel_append_fill(&seq, ioch, tmp[1], 4096, NULL, NULL, 0xa5, 0,
				    ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	rc = spdk_accel_append_fill(&seq, ioch, tmp[1], 2048, NULL, NULL, 0xde, 0,
				    ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	dst_iovs[1].iov_base = buf;
	dst_iovs[1].iov_len = sizeof(buf);
	src_iovs[1].iov_base = tmp[1];
	src_iovs[1].iov_len = sizeof(tmp[1]);
	rc = spdk_accel_append_copy(&seq, ioch, &dst_iovs[1], 1, NULL, NULL,
				    &src_iovs[1], 1, NULL, NULL, 0,
				    ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	spdk_accel_sequence_abort(seq);
	CU_ASSERT_EQUAL(completed, 4);
	CU_ASSERT_EQUAL(memcmp(buf, expected, sizeof(buf)), 0);

	/* This should be a no-op */
	spdk_accel_sequence_abort(NULL);

	spdk_put_io_channel(ioch);
	poll_threads();
}

static void
test_sequence_append_error(void)
{
	struct spdk_accel_sequence *seq = NULL;
	struct spdk_io_channel *ioch;
	struct accel_io_channel *accel_ch;
	struct iovec src_iovs, dst_iovs;
	char buf[4096];
	TAILQ_HEAD(, spdk_accel_task) tasks = TAILQ_HEAD_INITIALIZER(tasks);
	TAILQ_HEAD(, spdk_accel_sequence) seqs = TAILQ_HEAD_INITIALIZER(seqs);
	int rc;

	ioch = spdk_accel_get_io_channel();
	SPDK_CU_ASSERT_FATAL(ioch != NULL);
	accel_ch = spdk_io_channel_get_ctx(ioch);

	/* Check that append fails and no sequence object is allocated when there are no more free
	 * tasks */
	TAILQ_SWAP(&tasks, &accel_ch->task_pool, spdk_accel_task, link);

	rc = spdk_accel_append_fill(&seq, ioch, buf, sizeof(buf), NULL, NULL, 0xa5, 0,
				    ut_sequence_step_cb, NULL);
	CU_ASSERT_EQUAL(rc, -ENOMEM);
	CU_ASSERT_PTR_NULL(seq);

	dst_iovs.iov_base = buf;
	dst_iovs.iov_len = 2048;
	src_iovs.iov_base = &buf[2048];
	src_iovs.iov_len = 2048;
	rc = spdk_accel_append_copy(&seq, ioch, &dst_iovs, 1, NULL, NULL,
				    &src_iovs, 1, NULL, NULL, 0, ut_sequence_step_cb, NULL);
	CU_ASSERT_EQUAL(rc, -ENOMEM);
	CU_ASSERT_PTR_NULL(seq);

	dst_iovs.iov_base = buf;
	dst_iovs.iov_len = 2048;
	src_iovs.iov_base = &buf[2048];
	src_iovs.iov_len = 2048;
	rc = spdk_accel_append_decompress(&seq, ioch, &dst_iovs, 1, NULL, NULL,
					  &src_iovs, 1, NULL, NULL, 0, ut_sequence_step_cb, NULL);
	CU_ASSERT_EQUAL(rc, -ENOMEM);
	CU_ASSERT_PTR_NULL(seq);

	/* Check that the same happens when the sequence queue is empty */
	TAILQ_SWAP(&tasks, &accel_ch->task_pool, spdk_accel_task, link);
	TAILQ_SWAP(&seqs, &accel_ch->seq_pool, spdk_accel_sequence, link);

	rc = spdk_accel_append_fill(&seq, ioch, buf, sizeof(buf), NULL, NULL, 0xa5, 0,
				    ut_sequence_step_cb, NULL);
	CU_ASSERT_EQUAL(rc, -ENOMEM);
	CU_ASSERT_PTR_NULL(seq);

	dst_iovs.iov_base = buf;
	dst_iovs.iov_len = 2048;
	src_iovs.iov_base = &buf[2048];
	src_iovs.iov_len = 2048;
	rc = spdk_accel_append_copy(&seq, ioch, &dst_iovs, 1, NULL, NULL,
				    &src_iovs, 1, NULL, NULL, 0, ut_sequence_step_cb, NULL);
	CU_ASSERT_EQUAL(rc, -ENOMEM);
	CU_ASSERT_PTR_NULL(seq);

	dst_iovs.iov_base = buf;
	dst_iovs.iov_len = 2048;
	src_iovs.iov_base = &buf[2048];
	src_iovs.iov_len = 2048;
	rc = spdk_accel_append_decompress(&seq, ioch, &dst_iovs, 1, NULL, NULL,
					  &src_iovs, 1, NULL, NULL, 0, ut_sequence_step_cb, NULL);
	CU_ASSERT_EQUAL(rc, -ENOMEM);
	CU_ASSERT_PTR_NULL(seq);

	TAILQ_SWAP(&tasks, &accel_ch->task_pool, spdk_accel_task, link);

	spdk_put_io_channel(ioch);
	poll_threads();
}

struct ut_sequence_operation {
	int complete_status;
	int submit_status;
	int count;
	struct iovec *src_iovs;
	uint32_t src_iovcnt;
	struct iovec *dst_iovs;
	uint32_t dst_iovcnt;
};

static struct ut_sequence_operation g_seq_operations[ACCEL_OPC_LAST];

static int
ut_sequnce_submit_tasks(struct spdk_io_channel *ch, struct spdk_accel_task *task)
{
	struct ut_sequence_operation *op = &g_seq_operations[task->op_code];

	if (op->src_iovs != NULL) {
		CU_ASSERT_EQUAL(task->s.iovcnt, op->src_iovcnt);
		CU_ASSERT_EQUAL(memcmp(task->s.iovs, op->src_iovs,
				       sizeof(struct iovec) * op->src_iovcnt), 0);
	}
	if (op->dst_iovs != NULL) {
		CU_ASSERT_EQUAL(task->d.iovcnt, op->dst_iovcnt);
		CU_ASSERT_EQUAL(memcmp(task->d.iovs, op->dst_iovs,
				       sizeof(struct iovec) * op->dst_iovcnt), 0);
	}

	op->count++;
	if (op->submit_status != 0) {
		return op->submit_status;
	}

	spdk_accel_task_complete(task, op->complete_status);

	return 0;
}

static void
test_sequence_completion_error(void)
{
	struct spdk_accel_sequence *seq = NULL;
	struct spdk_io_channel *ioch;
	struct ut_sequence ut_seq;
	struct iovec src_iovs, dst_iovs;
	char buf[4096], tmp[4096];
	struct spdk_accel_module_if *modules[ACCEL_OPC_LAST];
	int i, rc, completed;

	ioch = spdk_accel_get_io_channel();
	SPDK_CU_ASSERT_FATAL(ioch != NULL);

	/* Override the submit_tasks function */
	g_module.submit_tasks = ut_sequnce_submit_tasks;
	for (i = 0; i < ACCEL_OPC_LAST; ++i) {
		modules[i] = g_modules_opc[i];
		g_modules_opc[i] = &g_module;
	}

	memset(buf, 0, sizeof(buf));
	memset(tmp, 0, sizeof(tmp));

	/* Check that if the first operation completes with an error, the whole sequence is
	 * completed with that error and that all operations' completion callbacks are executed
	 */
	g_seq_operations[ACCEL_OPC_FILL].complete_status = -E2BIG;
	completed = 0;
	seq = NULL;
	rc = spdk_accel_append_fill(&seq, ioch, tmp, sizeof(tmp), NULL, NULL, 0xa5, 0,
				    ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	dst_iovs.iov_base = buf;
	dst_iovs.iov_len = sizeof(buf);
	src_iovs.iov_base = tmp;
	src_iovs.iov_len = sizeof(tmp);

	rc = spdk_accel_append_copy(&seq, ioch, &dst_iovs, 1, NULL, NULL,
				    &src_iovs, 1, NULL, NULL, 0, ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	ut_seq.complete = false;
	rc = spdk_accel_sequence_finish(seq, ut_sequence_complete_cb, &ut_seq);
	CU_ASSERT_EQUAL(rc, 0);

	poll_threads();
	CU_ASSERT_EQUAL(completed, 2);
	CU_ASSERT_EQUAL(ut_seq.status, -E2BIG);

	/* Check the same with a second operation in the sequence */
	g_seq_operations[ACCEL_OPC_COPY].complete_status = -EACCES;
	g_seq_operations[ACCEL_OPC_FILL].complete_status = 0;
	completed = 0;
	seq = NULL;
	rc = spdk_accel_append_fill(&seq, ioch, tmp, sizeof(tmp), NULL, NULL, 0xa5, 0,
				    ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	dst_iovs.iov_base = buf;
	dst_iovs.iov_len = sizeof(buf);
	src_iovs.iov_base = tmp;
	src_iovs.iov_len = sizeof(tmp);

	rc = spdk_accel_append_copy(&seq, ioch, &dst_iovs, 1, NULL, NULL,
				    &src_iovs, 1, NULL, NULL, 0, ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	ut_seq.complete = false;
	rc = spdk_accel_sequence_finish(seq, ut_sequence_complete_cb, &ut_seq);
	CU_ASSERT_EQUAL(rc, 0);

	poll_threads();
	CU_ASSERT_EQUAL(completed, 2);
	CU_ASSERT_EQUAL(ut_seq.status, -EACCES);

	g_seq_operations[ACCEL_OPC_COPY].complete_status = 0;
	g_seq_operations[ACCEL_OPC_FILL].complete_status = 0;

	/* Check submission failure of the first operation */
	g_seq_operations[ACCEL_OPC_FILL].submit_status = -EADDRINUSE;
	completed = 0;
	seq = NULL;
	rc = spdk_accel_append_fill(&seq, ioch, tmp, sizeof(tmp), NULL, NULL, 0xa5, 0,
				    ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	dst_iovs.iov_base = buf;
	dst_iovs.iov_len = sizeof(buf);
	src_iovs.iov_base = tmp;
	src_iovs.iov_len = sizeof(tmp);

	rc = spdk_accel_append_copy(&seq, ioch, &dst_iovs, 1, NULL, NULL,
				    &src_iovs, 1, NULL, NULL, 0, ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	ut_seq.complete = false;
	rc = spdk_accel_sequence_finish(seq, ut_sequence_complete_cb, &ut_seq);
	CU_ASSERT_EQUAL(rc, 0);

	poll_threads();
	CU_ASSERT_EQUAL(completed, 2);
	CU_ASSERT_EQUAL(ut_seq.status, -EADDRINUSE);

	/* Check the same with a second operation */
	g_seq_operations[ACCEL_OPC_COPY].submit_status = -EADDRNOTAVAIL;
	g_seq_operations[ACCEL_OPC_FILL].submit_status = 0;
	completed = 0;
	seq = NULL;
	rc = spdk_accel_append_fill(&seq, ioch, tmp, sizeof(tmp), NULL, NULL, 0xa5, 0,
				    ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	dst_iovs.iov_base = buf;
	dst_iovs.iov_len = sizeof(buf);
	src_iovs.iov_base = tmp;
	src_iovs.iov_len = sizeof(tmp);

	rc = spdk_accel_append_copy(&seq, ioch, &dst_iovs, 1, NULL, NULL,
				    &src_iovs, 1, NULL, NULL, 0, ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	ut_seq.complete = false;
	rc = spdk_accel_sequence_finish(seq, ut_sequence_complete_cb, &ut_seq);
	CU_ASSERT_EQUAL(rc, 0);

	poll_threads();
	CU_ASSERT_EQUAL(completed, 2);
	CU_ASSERT_EQUAL(ut_seq.status, -EADDRNOTAVAIL);

	/* Cleanup module pointers to make subsequent tests work correctly */
	for (i = 0; i < ACCEL_OPC_LAST; ++i) {
		g_modules_opc[i] = modules[i];
	}

	spdk_put_io_channel(ioch);
	poll_threads();
}

#ifdef SPDK_CONFIG_ISAL
static void
ut_compress_cb(void *cb_arg, int status)
{
	int *completed = cb_arg;

	CU_ASSERT_EQUAL(status, 0);

	*completed = 1;
}

static void
test_sequence_decompress(void)
{
	struct spdk_accel_sequence *seq = NULL;
	struct spdk_io_channel *ioch;
	struct ut_sequence ut_seq;
	char buf[4096], tmp[2][4096], expected[4096];
	struct iovec src_iovs[2], dst_iovs[2];
	uint32_t compressed_size;
	int rc, completed = 0;

	ioch = spdk_accel_get_io_channel();
	SPDK_CU_ASSERT_FATAL(ioch != NULL);

	memset(expected, 0xa5, sizeof(expected));
	src_iovs[0].iov_base = expected;
	src_iovs[0].iov_len = sizeof(expected);
	rc = spdk_accel_submit_compress(ioch, tmp[0], sizeof(tmp[0]), &src_iovs[0], 1,
					&compressed_size, 0, ut_compress_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	while (!completed) {
		poll_threads();
	}

	/* Check a single decompress operation in a sequence */
	seq = NULL;
	completed = 0;

	dst_iovs[0].iov_base = buf;
	dst_iovs[0].iov_len = sizeof(buf);
	src_iovs[0].iov_base = tmp[0];
	src_iovs[0].iov_len = compressed_size;
	rc = spdk_accel_append_decompress(&seq, ioch, &dst_iovs[0], 1, NULL, NULL,
					  &src_iovs[0], 1, NULL, NULL, 0,
					  ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	ut_seq.complete = false;
	rc = spdk_accel_sequence_finish(seq, ut_sequence_complete_cb, &ut_seq);
	CU_ASSERT_EQUAL(rc, 0);

	poll_threads();

	CU_ASSERT_EQUAL(completed, 1);
	CU_ASSERT(ut_seq.complete);
	CU_ASSERT_EQUAL(ut_seq.status, 0);
	CU_ASSERT_EQUAL(memcmp(buf, expected, sizeof(buf)), 0);

	/* Put the decompress operation in the middle of a sequence with a copy operation at the
	 * beginning and a fill at the end modifying the first 2048B of the buffer.
	 */
	memset(expected, 0xfe, 2048);
	memset(buf, 0, sizeof(buf));
	seq = NULL;
	completed = 0;

	dst_iovs[0].iov_base = tmp[1];
	dst_iovs[0].iov_len = compressed_size;
	src_iovs[0].iov_base = tmp[0];
	src_iovs[0].iov_len = compressed_size;
	rc = spdk_accel_append_copy(&seq, ioch, &dst_iovs[0], 1, NULL, NULL,
				    &src_iovs[0], 1, NULL, NULL, 0,
				    ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	dst_iovs[1].iov_base = buf;
	dst_iovs[1].iov_len = sizeof(buf);
	src_iovs[1].iov_base = tmp[1];
	src_iovs[1].iov_len = compressed_size;
	rc = spdk_accel_append_decompress(&seq, ioch, &dst_iovs[1], 1, NULL, NULL,
					  &src_iovs[1], 1, NULL, NULL, 0,
					  ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	rc = spdk_accel_append_fill(&seq, ioch, buf, 2048, NULL, NULL, 0xfe, 0,
				    ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	ut_seq.complete = false;
	rc = spdk_accel_sequence_finish(seq, ut_sequence_complete_cb, &ut_seq);
	CU_ASSERT_EQUAL(rc, 0);

	poll_threads();

	CU_ASSERT_EQUAL(completed, 3);
	CU_ASSERT(ut_seq.complete);
	CU_ASSERT_EQUAL(ut_seq.status, 0);
	CU_ASSERT_EQUAL(memcmp(buf, expected, sizeof(buf)), 0);

	/* Check sequence with decompress at the beginning: decompress -> copy */
	memset(expected, 0xa5, sizeof(expected));
	memset(buf, 0, sizeof(buf));
	seq = NULL;
	completed = 0;

	dst_iovs[0].iov_base = tmp[1];
	dst_iovs[0].iov_len = sizeof(tmp[1]);
	src_iovs[0].iov_base = tmp[0];
	src_iovs[0].iov_len = compressed_size;
	rc = spdk_accel_append_decompress(&seq, ioch, &dst_iovs[0], 1, NULL, NULL,
					  &src_iovs[0], 1, NULL, NULL, 0,
					  ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	dst_iovs[1].iov_base = buf;
	dst_iovs[1].iov_len = sizeof(buf);
	src_iovs[1].iov_base = tmp[1];
	src_iovs[1].iov_len = sizeof(tmp[1]);
	rc = spdk_accel_append_copy(&seq, ioch, &dst_iovs[1], 1, NULL, NULL,
				    &src_iovs[1], 1, NULL, NULL, 0,
				    ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	ut_seq.complete = false;
	rc = spdk_accel_sequence_finish(seq, ut_sequence_complete_cb, &ut_seq);
	CU_ASSERT_EQUAL(rc, 0);

	poll_threads();

	CU_ASSERT_EQUAL(completed, 2);
	CU_ASSERT(ut_seq.complete);
	CU_ASSERT_EQUAL(ut_seq.status, 0);
	CU_ASSERT_EQUAL(memcmp(buf, expected, sizeof(buf)), 0);

	spdk_put_io_channel(ioch);
	poll_threads();
}

static void
test_sequence_reverse(void)
{
	struct spdk_accel_sequence *seq = NULL;
	struct spdk_io_channel *ioch;
	struct ut_sequence ut_seq;
	char buf[4096], tmp[2][4096], expected[4096];
	struct iovec src_iovs[2], dst_iovs[2];
	uint32_t compressed_size;
	int rc, completed = 0;

	ioch = spdk_accel_get_io_channel();
	SPDK_CU_ASSERT_FATAL(ioch != NULL);

	memset(expected, 0xa5, sizeof(expected));
	src_iovs[0].iov_base = expected;
	src_iovs[0].iov_len = sizeof(expected);
	rc = spdk_accel_submit_compress(ioch, tmp[0], sizeof(tmp[0]), &src_iovs[0], 1,
					&compressed_size, 0, ut_compress_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	while (!completed) {
		poll_threads();
	}

	/* First check that reversing a sequnce with a single operation is a no-op */
	memset(buf, 0, sizeof(buf));
	seq = NULL;
	completed = 0;

	dst_iovs[0].iov_base = buf;
	dst_iovs[0].iov_len = sizeof(buf);
	src_iovs[0].iov_base = tmp[0];
	src_iovs[0].iov_len = compressed_size;
	rc = spdk_accel_append_decompress(&seq, ioch, &dst_iovs[0], 1, NULL, NULL,
					  &src_iovs[0], 1, NULL, NULL, 0,
					  ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	spdk_accel_sequence_reverse(seq);

	ut_seq.complete = false;
	rc = spdk_accel_sequence_finish(seq, ut_sequence_complete_cb, &ut_seq);
	CU_ASSERT_EQUAL(rc, 0);

	poll_threads();

	CU_ASSERT_EQUAL(completed, 1);
	CU_ASSERT(ut_seq.complete);
	CU_ASSERT_EQUAL(ut_seq.status, 0);
	CU_ASSERT_EQUAL(memcmp(buf, expected, sizeof(buf)), 0);

	/* Add a copy operation at the end with src set to the compressed data.  After reverse(),
	 * that copy operation should be first, so decompress() should receive compressed data in
	 * its src buffer.
	 */
	memset(buf, 0, sizeof(buf));
	memset(tmp[1], 0, sizeof(tmp[1]));
	seq = NULL;
	completed = 0;

	dst_iovs[0].iov_base = buf;
	dst_iovs[0].iov_len = sizeof(buf);
	src_iovs[0].iov_base = tmp[1];
	src_iovs[0].iov_len = compressed_size;
	rc = spdk_accel_append_decompress(&seq, ioch, &dst_iovs[0], 1, NULL, NULL,
					  &src_iovs[0], 1, NULL, NULL, 0,
					  ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	dst_iovs[1].iov_base = tmp[1];
	dst_iovs[1].iov_len = compressed_size;
	src_iovs[1].iov_base = tmp[0];
	src_iovs[1].iov_len = compressed_size;
	rc = spdk_accel_append_copy(&seq, ioch, &dst_iovs[1], 1, NULL, NULL,
				    &src_iovs[1], 1, NULL, NULL, 0,
				    ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	spdk_accel_sequence_reverse(seq);

	ut_seq.complete = false;
	rc = spdk_accel_sequence_finish(seq, ut_sequence_complete_cb, &ut_seq);
	CU_ASSERT_EQUAL(rc, 0);

	poll_threads();

	CU_ASSERT_EQUAL(completed, 2);
	CU_ASSERT(ut_seq.complete);
	CU_ASSERT_EQUAL(ut_seq.status, 0);
	CU_ASSERT_EQUAL(memcmp(buf, expected, sizeof(buf)), 0);

	/* Check the same, but add an extra fill operation at the beginning that should execute last
	 * after reverse().
	 */
	memset(buf, 0, sizeof(buf));
	memset(tmp[1], 0, sizeof(tmp[1]));
	memset(expected, 0xfe, 2048);
	seq = NULL;
	completed = 0;

	rc = spdk_accel_append_fill(&seq, ioch, buf, 2048, NULL, NULL, 0xfe, 0,
				    ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	dst_iovs[0].iov_base = buf;
	dst_iovs[0].iov_len = sizeof(buf);
	src_iovs[0].iov_base = tmp[1];
	src_iovs[0].iov_len = compressed_size;
	rc = spdk_accel_append_decompress(&seq, ioch, &dst_iovs[0], 1, NULL, NULL,
					  &src_iovs[0], 1, NULL, NULL, 0,
					  ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	dst_iovs[1].iov_base = tmp[1];
	dst_iovs[1].iov_len = compressed_size;
	src_iovs[1].iov_base = tmp[0];
	src_iovs[1].iov_len = compressed_size;
	rc = spdk_accel_append_copy(&seq, ioch, &dst_iovs[1], 1, NULL, NULL,
				    &src_iovs[1], 1, NULL, NULL, 0,
				    ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	spdk_accel_sequence_reverse(seq);

	ut_seq.complete = false;
	rc = spdk_accel_sequence_finish(seq, ut_sequence_complete_cb, &ut_seq);
	CU_ASSERT_EQUAL(rc, 0);

	poll_threads();

	CU_ASSERT_EQUAL(completed, 3);
	CU_ASSERT(ut_seq.complete);
	CU_ASSERT_EQUAL(ut_seq.status, 0);
	CU_ASSERT_EQUAL(memcmp(buf, expected, sizeof(buf)), 0);

	/* Build the sequence in order and then reverse it twice */
	memset(buf, 0, sizeof(buf));
	memset(tmp[1], 0, sizeof(tmp[1]));
	seq = NULL;
	completed = 0;

	dst_iovs[0].iov_base = tmp[1];
	dst_iovs[0].iov_len = compressed_size;
	src_iovs[0].iov_base = tmp[0];
	src_iovs[0].iov_len = compressed_size;
	rc = spdk_accel_append_copy(&seq, ioch, &dst_iovs[0], 1, NULL, NULL,
				    &src_iovs[0], 1, NULL, NULL, 0,
				    ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	dst_iovs[1].iov_base = buf;
	dst_iovs[1].iov_len = sizeof(buf);
	src_iovs[1].iov_base = tmp[1];
	src_iovs[1].iov_len = compressed_size;
	rc = spdk_accel_append_decompress(&seq, ioch, &dst_iovs[1], 1, NULL, NULL,
					  &src_iovs[1], 1, NULL, NULL, 0,
					  ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	rc = spdk_accel_append_fill(&seq, ioch, buf, 2048, NULL, NULL, 0xfe, 0,
				    ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	spdk_accel_sequence_reverse(seq);
	spdk_accel_sequence_reverse(seq);

	ut_seq.complete = false;
	rc = spdk_accel_sequence_finish(seq, ut_sequence_complete_cb, &ut_seq);
	CU_ASSERT_EQUAL(rc, 0);

	poll_threads();

	CU_ASSERT_EQUAL(completed, 3);
	CU_ASSERT(ut_seq.complete);
	CU_ASSERT_EQUAL(ut_seq.status, 0);
	CU_ASSERT_EQUAL(memcmp(buf, expected, sizeof(buf)), 0);

	spdk_put_io_channel(ioch);
	poll_threads();
}
#endif

static void
test_sequence_copy_elision(void)
{
	struct spdk_accel_sequence *seq = NULL;
	struct spdk_io_channel *ioch;
	struct ut_sequence ut_seq;
	struct iovec src_iovs[4], dst_iovs[4], exp_iovs[2];
	char buf[4096], tmp[4][4096];
	struct spdk_accel_module_if *modules[ACCEL_OPC_LAST];
	int i, rc, completed;

	ioch = spdk_accel_get_io_channel();
	SPDK_CU_ASSERT_FATAL(ioch != NULL);

	/* Override the submit_tasks function */
	g_module.submit_tasks = ut_sequnce_submit_tasks;
	for (i = 0; i < ACCEL_OPC_LAST; ++i) {
		g_seq_operations[i].complete_status = 0;
		g_seq_operations[i].submit_status = 0;
		g_seq_operations[i].count = 0;

		modules[i] = g_modules_opc[i];
		g_modules_opc[i] = &g_module;
	}

	/* Check that a copy operation at the beginning is removed */
	seq = NULL;
	completed = 0;
	g_seq_operations[ACCEL_OPC_DECOMPRESS].dst_iovcnt = 1;
	g_seq_operations[ACCEL_OPC_DECOMPRESS].src_iovcnt = 1;
	g_seq_operations[ACCEL_OPC_DECOMPRESS].src_iovs = &exp_iovs[0];
	g_seq_operations[ACCEL_OPC_DECOMPRESS].dst_iovs = &exp_iovs[1];
	exp_iovs[0].iov_base = tmp[0];
	exp_iovs[0].iov_len = sizeof(tmp[0]);
	exp_iovs[1].iov_base = buf;
	exp_iovs[1].iov_len = 2048;

	dst_iovs[0].iov_base = tmp[1];
	dst_iovs[0].iov_len = sizeof(tmp[1]);
	src_iovs[0].iov_base = tmp[0];
	src_iovs[0].iov_len = sizeof(tmp[0]);
	rc = spdk_accel_append_copy(&seq, ioch, &dst_iovs[0], 1, NULL, NULL,
				    &src_iovs[0], 1, NULL, NULL, 0,
				    ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	dst_iovs[1].iov_base = buf;
	dst_iovs[1].iov_len = 2048;
	src_iovs[1].iov_base = tmp[1];
	src_iovs[1].iov_len = sizeof(tmp[1]);
	rc = spdk_accel_append_decompress(&seq, ioch, &dst_iovs[1], 1, NULL, NULL,
					  &src_iovs[1], 1, NULL, NULL, 0,
					  ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	ut_seq.complete = false;
	rc = spdk_accel_sequence_finish(seq, ut_sequence_complete_cb, &ut_seq);
	CU_ASSERT_EQUAL(rc, 0);

	poll_threads();

	CU_ASSERT_EQUAL(completed, 2);
	CU_ASSERT(ut_seq.complete);
	CU_ASSERT_EQUAL(ut_seq.status, 0);
	CU_ASSERT_EQUAL(g_seq_operations[ACCEL_OPC_COPY].count, 0);
	CU_ASSERT_EQUAL(g_seq_operations[ACCEL_OPC_DECOMPRESS].count, 1);

	/* Check that a copy operation at the end is removed too */
	seq = NULL;
	completed = 0;
	g_seq_operations[ACCEL_OPC_COPY].count = 0;
	g_seq_operations[ACCEL_OPC_DECOMPRESS].count = 0;
	g_seq_operations[ACCEL_OPC_DECOMPRESS].src_iovs = &exp_iovs[0];
	g_seq_operations[ACCEL_OPC_DECOMPRESS].dst_iovs = &exp_iovs[1];
	exp_iovs[0].iov_base = tmp[0];
	exp_iovs[0].iov_len = sizeof(tmp[0]);
	exp_iovs[1].iov_base = buf;
	exp_iovs[1].iov_len = 2048;

	dst_iovs[0].iov_base = tmp[1];
	dst_iovs[0].iov_len = 2048;
	src_iovs[0].iov_base = tmp[0];
	src_iovs[0].iov_len = sizeof(tmp[0]);
	rc = spdk_accel_append_decompress(&seq, ioch, &dst_iovs[0], 1, NULL, NULL,
					  &src_iovs[0], 1, NULL, NULL, 0,
					  ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	dst_iovs[1].iov_base = buf;
	dst_iovs[1].iov_len = 2048;
	src_iovs[1].iov_base = tmp[1];
	src_iovs[1].iov_len = 2048;
	rc = spdk_accel_append_copy(&seq, ioch, &dst_iovs[1], 1, NULL, NULL,
				    &src_iovs[1], 1, NULL, NULL, 0,
				    ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	ut_seq.complete = false;
	rc = spdk_accel_sequence_finish(seq, ut_sequence_complete_cb, &ut_seq);
	CU_ASSERT_EQUAL(rc, 0);

	poll_threads();

	CU_ASSERT_EQUAL(completed, 2);
	CU_ASSERT(ut_seq.complete);
	CU_ASSERT_EQUAL(ut_seq.status, 0);
	CU_ASSERT_EQUAL(g_seq_operations[ACCEL_OPC_COPY].count, 0);
	CU_ASSERT_EQUAL(g_seq_operations[ACCEL_OPC_DECOMPRESS].count, 1);

	/* Check a copy operation both at the beginning and the end */
	seq = NULL;
	completed = 0;
	g_seq_operations[ACCEL_OPC_COPY].count = 0;
	g_seq_operations[ACCEL_OPC_DECOMPRESS].count = 0;
	g_seq_operations[ACCEL_OPC_DECOMPRESS].src_iovs = &exp_iovs[0];
	g_seq_operations[ACCEL_OPC_DECOMPRESS].dst_iovs = &exp_iovs[1];
	exp_iovs[0].iov_base = tmp[0];
	exp_iovs[0].iov_len = sizeof(tmp[0]);
	exp_iovs[1].iov_base = buf;
	exp_iovs[1].iov_len = 2048;

	dst_iovs[0].iov_base = tmp[1];
	dst_iovs[0].iov_len = sizeof(tmp[1]);
	src_iovs[0].iov_base = tmp[0];
	src_iovs[0].iov_len = sizeof(tmp[0]);
	rc = spdk_accel_append_copy(&seq, ioch, &dst_iovs[0], 1, NULL, NULL,
				    &src_iovs[0], 1, NULL, NULL, 0,
				    ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	dst_iovs[1].iov_base = tmp[2];
	dst_iovs[1].iov_len = 2048;
	src_iovs[1].iov_base = tmp[1];
	src_iovs[1].iov_len = sizeof(tmp[1]);
	rc = spdk_accel_append_decompress(&seq, ioch, &dst_iovs[1], 1, NULL, NULL,
					  &src_iovs[1], 1, NULL, NULL, 0,
					  ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	dst_iovs[2].iov_base = buf;
	dst_iovs[2].iov_len = 2048;
	src_iovs[2].iov_base = tmp[2];
	src_iovs[2].iov_len = 2048;
	rc = spdk_accel_append_copy(&seq, ioch, &dst_iovs[2], 1, NULL, NULL,
				    &src_iovs[2], 1, NULL, NULL, 0,
				    ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	ut_seq.complete = false;
	rc = spdk_accel_sequence_finish(seq, ut_sequence_complete_cb, &ut_seq);
	CU_ASSERT_EQUAL(rc, 0);

	poll_threads();

	CU_ASSERT_EQUAL(completed, 3);
	CU_ASSERT(ut_seq.complete);
	CU_ASSERT_EQUAL(ut_seq.status, 0);
	CU_ASSERT_EQUAL(g_seq_operations[ACCEL_OPC_COPY].count, 0);
	CU_ASSERT_EQUAL(g_seq_operations[ACCEL_OPC_DECOMPRESS].count, 1);

	/* Check decompress + copy + decompress + copy */
	seq = NULL;
	completed = 0;
	g_seq_operations[ACCEL_OPC_COPY].count = 0;
	g_seq_operations[ACCEL_OPC_DECOMPRESS].count = 0;
	g_seq_operations[ACCEL_OPC_DECOMPRESS].src_iovs = NULL;
	g_seq_operations[ACCEL_OPC_DECOMPRESS].dst_iovs = NULL;

	dst_iovs[0].iov_base = tmp[1];
	dst_iovs[0].iov_len = sizeof(tmp[1]);
	src_iovs[0].iov_base = tmp[0];
	src_iovs[0].iov_len = sizeof(tmp[0]);
	rc = spdk_accel_append_decompress(&seq, ioch, &dst_iovs[0], 1, NULL, NULL,
					  &src_iovs[0], 1, NULL, NULL, 0,
					  ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	dst_iovs[1].iov_base = tmp[2];
	dst_iovs[1].iov_len = 2048;
	src_iovs[1].iov_base = tmp[1];
	src_iovs[1].iov_len = sizeof(tmp[1]);
	rc = spdk_accel_append_copy(&seq, ioch, &dst_iovs[1], 1, NULL, NULL,
				    &src_iovs[1], 1, NULL, NULL, 0,
				    ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	dst_iovs[2].iov_base = tmp[3];
	dst_iovs[2].iov_len = 1024;
	src_iovs[2].iov_base = tmp[2];
	src_iovs[2].iov_len = 2048;
	rc = spdk_accel_append_decompress(&seq, ioch, &dst_iovs[2], 1, NULL, NULL,
					  &src_iovs[2], 1, NULL, NULL, 0,
					  ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	dst_iovs[3].iov_base = buf;
	dst_iovs[3].iov_len = 1024;
	src_iovs[3].iov_base = tmp[3];
	src_iovs[3].iov_len = 1024;
	rc = spdk_accel_append_copy(&seq, ioch, &dst_iovs[3], 1, NULL, NULL,
				    &src_iovs[3], 1, NULL, NULL, 0,
				    ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	ut_seq.complete = false;
	rc = spdk_accel_sequence_finish(seq, ut_sequence_complete_cb, &ut_seq);
	CU_ASSERT_EQUAL(rc, 0);

	poll_threads();

	CU_ASSERT_EQUAL(completed, 4);
	CU_ASSERT(ut_seq.complete);
	CU_ASSERT_EQUAL(ut_seq.status, 0);
	CU_ASSERT_EQUAL(g_seq_operations[ACCEL_OPC_COPY].count, 0);
	CU_ASSERT_EQUAL(g_seq_operations[ACCEL_OPC_DECOMPRESS].count, 2);

	/* Check two copy operations - one of them should be removed, while the other should be
	 * executed normally */
	seq = NULL;
	completed = 0;
	g_seq_operations[ACCEL_OPC_COPY].count = 0;

	dst_iovs[0].iov_base = tmp[1];
	dst_iovs[0].iov_len = sizeof(tmp[1]);
	src_iovs[0].iov_base = tmp[0];
	src_iovs[0].iov_len = sizeof(tmp[0]);
	rc = spdk_accel_append_copy(&seq, ioch, &dst_iovs[0], 1, NULL, NULL,
				    &src_iovs[0], 1, NULL, NULL, 0,
				    ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	dst_iovs[1].iov_base = buf;
	dst_iovs[1].iov_len = sizeof(buf);
	src_iovs[1].iov_base = tmp[1];
	src_iovs[1].iov_len = sizeof(tmp[1]);
	rc = spdk_accel_append_copy(&seq, ioch, &dst_iovs[1], 1, NULL, NULL,
				    &src_iovs[1], 1, NULL, NULL, 0,
				    ut_sequence_step_cb, &completed);
	CU_ASSERT_EQUAL(rc, 0);

	ut_seq.complete = false;
	rc = spdk_accel_sequence_finish(seq, ut_sequence_complete_cb, &ut_seq);
	CU_ASSERT_EQUAL(rc, 0);

	poll_threads();

	CU_ASSERT_EQUAL(completed, 2);
	CU_ASSERT(ut_seq.complete);
	CU_ASSERT_EQUAL(ut_seq.status, 0);
	CU_ASSERT_EQUAL(g_seq_operations[ACCEL_OPC_COPY].count, 1);

	/* Cleanup module pointers to make subsequent tests work correctly */
	for (i = 0; i < ACCEL_OPC_LAST; ++i) {
		g_modules_opc[i] = modules[i];
	}

	g_seq_operations[ACCEL_OPC_DECOMPRESS].src_iovs = NULL;
	g_seq_operations[ACCEL_OPC_DECOMPRESS].dst_iovs = NULL;

	spdk_put_io_channel(ioch);
	poll_threads();
}

static int
test_sequence_setup(void)
{
	int rc;

	allocate_cores(1);
	allocate_threads(1);
	set_thread(0);

	rc = spdk_accel_initialize();
	if (rc != 0) {
		CU_ASSERT(false);
		return -1;
	}

	return 0;
}

static void
accel_finish_cb(void *cb_arg)
{
	bool *done = cb_arg;

	*done = true;
}

static int
test_sequence_cleanup(void)
{
	bool done = false;

	spdk_accel_finish(accel_finish_cb, &done);

	while (!done) {
		poll_threads();
	}

	free_threads();
	free_cores();

	return 0;
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL, seq_suite;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	/* Sequence tests require accel to be initialized normally, so run them before the other
	 * tests which register accel modules which aren't fully implemented, causing accel
	 * initialization to fail.
	 */
	seq_suite = CU_add_suite("accel_sequence", test_sequence_setup, test_sequence_cleanup);
	CU_ADD_TEST(seq_suite, test_sequence_fill_copy);
	CU_ADD_TEST(seq_suite, test_sequence_abort);
	CU_ADD_TEST(seq_suite, test_sequence_append_error);
	CU_ADD_TEST(seq_suite, test_sequence_completion_error);
#ifdef SPDK_CONFIG_ISAL /* accel_sw requires isa-l for compression */
	CU_ADD_TEST(seq_suite, test_sequence_decompress);
	CU_ADD_TEST(seq_suite, test_sequence_reverse);
#endif
	CU_ADD_TEST(seq_suite, test_sequence_copy_elision);

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
