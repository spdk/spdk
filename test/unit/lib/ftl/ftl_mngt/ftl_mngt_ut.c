/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include <sys/queue.h>

#include "spdk/stdinc.h"

#include "spdk_cunit.h"
#include "common/lib/test_env.c"

#include "ftl/mngt/ftl_mngt.c"

#define CALLER_CB_RET_VALUE 999

/* list for structure with results of tests from callbacks */
struct entry {
	int data;
	TAILQ_ENTRY(entry) entries;
};

TAILQ_HEAD(listhead, entry) g_head;

struct thread_send_msg_container {
	spdk_msg_fn fn;
	void *ctx;
} g_thread_send_msg_container;

struct spdk_ftl_dev g_dev;

int
spdk_thread_send_msg(const struct spdk_thread *thread, spdk_msg_fn fn, void *ctx)
{
	g_thread_send_msg_container.fn = fn;
	g_thread_send_msg_container.ctx = ctx;
	return 0;
}

struct spdk_thread *
spdk_get_thread(void)
{
	struct spdk_thread *thd = (struct spdk_thread *)0x1;
	return thd;
}

static int
setup_test_list(void)
{
	TAILQ_INIT(&g_head);
	return 0;
}

static void
check_list_empty(void)
{
	CU_ASSERT_TRUE(TAILQ_EMPTY(&g_head));
}

static void
add_elem_to_test_list(int data)
{
	struct entry *en = calloc(1, sizeof(*en));
	SPDK_CU_ASSERT_FATAL(en != NULL);
	en->data = data;
	TAILQ_INSERT_TAIL(&g_head, en, entries);
}

static void
check_elem_on_list_and_remove(int compared_elem)
{
	struct entry *en = TAILQ_FIRST(&g_head);
	if (en != NULL) {
		CU_ASSERT_EQUAL(en->data, compared_elem);
		TAILQ_REMOVE(&g_head, en, entries);
		free(en);
	} else {
		CU_FAIL("not null value was expected");
	}
}

static void
fn_finish(struct spdk_ftl_dev *dev, void *ctx, int status)
{
	add_elem_to_test_list(CALLER_CB_RET_VALUE);
	g_thread_send_msg_container.fn = NULL;
	g_thread_send_msg_container.ctx = NULL;
}

typedef int (*ftl_execute_fn)(struct spdk_ftl_dev *dev,
			      const struct ftl_mngt_process_desc *process,
			      ftl_mngt_completion cb, void *cb_cntx);

static void
run_ftl_mngt_with_cb_cntx(ftl_execute_fn exec_fn,
			  const struct ftl_mngt_process_desc *process, void *cb_cntx)
{
	int result = exec_fn(&g_dev, process, fn_finish, cb_cntx);
	CU_ASSERT_EQUAL(result, 0);
	while (g_thread_send_msg_container.fn != NULL) {
		g_thread_send_msg_container.fn(g_thread_send_msg_container.ctx);
	}
}

static void
run_ftl_mngt(ftl_execute_fn exec_fn,
	     const struct ftl_mngt_process_desc *process)
{
	run_ftl_mngt_with_cb_cntx(exec_fn, process, NULL);
}

/*-
 * test 1
 * tests simple invoking next steps
 * it is shown if ftl_mngt_process_execute and ftl_mngt_process_rollback invoke functions in proper order
 * (functions call only ftl_mngt_next_step)
 */

static void
fn_1_1_action(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	add_elem_to_test_list(1);
	ftl_mngt_next_step(mngt);
}

static void
fn_1_1_cleanup(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	add_elem_to_test_list(-1);
	ftl_mngt_next_step(mngt);
}

static void
fn_1_2_action(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	add_elem_to_test_list(2);
	ftl_mngt_next_step(mngt);
}

static void
fn_1_3_action(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	add_elem_to_test_list(3);
	ftl_mngt_next_step(mngt);
}

static void
fn_1_3_cleanup(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	add_elem_to_test_list(-3);
	ftl_mngt_next_step(mngt);
}

static struct ftl_mngt_process_desc pdesc_test_1 = {
	.name = "process 1",
	.steps = {
		{
			.name = "step 1",
			.action = fn_1_1_action,
			.cleanup = fn_1_1_cleanup
		},
		{
			.name = "step 2",
			.action = fn_1_2_action
		},
		{
			.name = "step 3",
			.action = fn_1_3_action,
			.cleanup = fn_1_3_cleanup
		},
		{}
	}
};

static void
test_next_step(void)
{
	run_ftl_mngt(ftl_mngt_process_execute, &pdesc_test_1);

	/* check proper order of action functions */
	for (int i = 1; i <= 3; i++) {
		check_elem_on_list_and_remove(i);
	}

	/* check if caller callback was invoked */
	check_elem_on_list_and_remove(CALLER_CB_RET_VALUE);

	run_ftl_mngt(ftl_mngt_process_rollback, &pdesc_test_1);

	/* Check proper order of cleanup functions.
	 * Cleanup functions add to list opposite values to action functions.
	 * Cleanup functions are invoked in reverse order,
	 * moreover action 2 does not have cleanup,
	 * so expected values are -3, then -1 */
	check_elem_on_list_and_remove(-3);
	check_elem_on_list_and_remove(-1);

	/* check if caller callback was invoked */
	check_elem_on_list_and_remove(CALLER_CB_RET_VALUE);

	check_list_empty();
}

/*-
 * test 2
 * tests action and cleanup function which invoke
 * ftl_mngt_continue_step function
 */

static void
fn_2_common_part(struct ftl_mngt_process *mngt, int elem)
{
	struct entry *en = TAILQ_LAST(&g_head, listhead);

	if (en == NULL || en->data != elem) {
		/* if function was invoked 1st time, make it once again */
		add_elem_to_test_list(elem);
		ftl_mngt_continue_step(mngt);
	} else {
		/* otherwise go to the next function */
		add_elem_to_test_list(elem);
		ftl_mngt_next_step(mngt);
	}
}

static void
fn_2_1_action(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	fn_2_common_part(mngt, 1);
}

static void
fn_2_1_cleanup(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	fn_2_common_part(mngt, -1);
}

static void
fn_2_2_action(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	fn_2_common_part(mngt, 2);
}

static void
fn_2_2_cleanup(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	fn_2_common_part(mngt, -2);
}

static struct ftl_mngt_process_desc pdesc_test_2 = {
	.name = "process 2",
	.steps = {
		{
			.name = "step 1",
			.action = fn_2_1_action,
			.cleanup = fn_2_1_cleanup
		},
		{
			.name = "step 2",
			.action = fn_2_2_action,
			.cleanup = fn_2_2_cleanup
		},
		{}
	}
};

static void
test_continue_step(void)
{
	run_ftl_mngt(ftl_mngt_process_execute, &pdesc_test_2);

	/* check proper order of action functions */
	check_elem_on_list_and_remove(1);
	check_elem_on_list_and_remove(1);
	check_elem_on_list_and_remove(2);
	check_elem_on_list_and_remove(2);

	/* check if caller callback was invoked */
	check_elem_on_list_and_remove(CALLER_CB_RET_VALUE);

	run_ftl_mngt(ftl_mngt_process_rollback, &pdesc_test_2);

	/* check proper order of action functions */
	check_elem_on_list_and_remove(-2);
	check_elem_on_list_and_remove(-2);
	check_elem_on_list_and_remove(-1);
	check_elem_on_list_and_remove(-1);

	/* check if caller callback was invoked */
	check_elem_on_list_and_remove(CALLER_CB_RET_VALUE);

	check_list_empty();
}

/*-
 * test 3
 * tests ftl_mngt_alloc_step_cntx and all ftl_mngt_get functions
 */

const int PROCESS_CNTX_TEST_VAL_0 = 21;
const int PROCESS_CNTX_TEST_VAL_1 = 37;
const int STEP_CNTX_TEST_VAL = 1;

static void
put_on_list(void)
{
	struct entry *en = calloc(1, sizeof(*en));
	SPDK_CU_ASSERT_FATAL(en != NULL);
	TAILQ_INSERT_TAIL(&g_head, en, entries);
}

static bool
check_if_list_empty_and_clean(void)
{
	struct entry *en = TAILQ_FIRST(&g_head);
	if (en == NULL) {
		return true;
	} else {
		TAILQ_REMOVE(&g_head, en, entries);
		free(en);
		return false;
	}
}

static void
fn_3_1_action(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	int *step_cntx_ptr, *process_cntx_ptr;
	char *caller_cntx_ptr;
	int status;

	step_cntx_ptr = ftl_mngt_get_step_ctx(mngt);
	if (check_if_list_empty_and_clean()) {
		/* In 1st run of this function test list is empty
		 * and 'if' is true, this part of function is done.
		 * That 'if' part ends with ftl_mngt_continue_step,
		 * so function will be called once again.
		 * Element is added to the test list
		 * to invoke 'else' in second run */
		put_on_list();
		/* this step descriptor does not locate any context
		 * at the beginning,
		 * so pointer should contain NULL */
		CU_ASSERT_PTR_NULL(step_cntx_ptr);

		status = ftl_mngt_alloc_step_ctx(mngt, sizeof(*step_cntx_ptr));
		SPDK_CU_ASSERT_FATAL(status == 0);
		step_cntx_ptr = ftl_mngt_get_step_ctx(mngt);
		/* now pointer should point to allocated context */
		CU_ASSERT_PTR_NOT_NULL(step_cntx_ptr);

		/* this value should be retrieved in second run of function
		 * (in 'else' part) */
		*step_cntx_ptr = STEP_CNTX_TEST_VAL;

		ftl_mngt_continue_step(mngt);
	} else {
		/* In second run retrieved pointer is not empty.
		 * Moreover it should contain value allocated for this step
		 * in previous run of function */
		CU_ASSERT_PTR_NOT_NULL(step_cntx_ptr);
		CU_ASSERT_EQUAL(*step_cntx_ptr, STEP_CNTX_TEST_VAL);

		/* check getting device */
		CU_ASSERT_EQUAL(ftl_mngt_get_dev(mngt), dev);
		CU_ASSERT_EQUAL(ftl_mngt_get_dev(mngt), &g_dev);

		/* tests for process context */
		process_cntx_ptr = ftl_mngt_get_process_ctx(mngt);

		/* 1st get of process context, should be clear ('0' values) */
		CU_ASSERT_EQUAL(process_cntx_ptr[0], 0);
		CU_ASSERT_EQUAL(process_cntx_ptr[1], 0);

		/* Random values put in process context.
		 * Should be retrieved in the next function
		 * (it is common space for the entire process) */
		process_cntx_ptr[0] = PROCESS_CNTX_TEST_VAL_0;
		process_cntx_ptr[1] = PROCESS_CNTX_TEST_VAL_1;

		/* tests for caller context */
		caller_cntx_ptr = ftl_mngt_get_caller_ctx(mngt);

		/* check previously located values */
		CU_ASSERT_EQUAL(caller_cntx_ptr[0], 'd');
		CU_ASSERT_EQUAL(caller_cntx_ptr[1], 'a');
		CU_ASSERT_EQUAL(caller_cntx_ptr[2], 'j');

		/* insert new */
		caller_cntx_ptr[0] = ' ';
		caller_cntx_ptr[1] = 'k';
		caller_cntx_ptr[2] = 'a';

		ftl_mngt_next_step(mngt);
	}
}

static void
fn_3_2_action(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	int *step_cntx_ptr, *process_cntx_ptr;
	char *caller_cntx_ptr;
	int status;

	step_cntx_ptr = ftl_mngt_get_step_ctx(mngt);
	/* context of this step descriptor is never empty
	 * so pointer cannot contain NULL */
	CU_ASSERT_PTR_NOT_NULL(step_cntx_ptr);

	if (check_if_list_empty_and_clean()) {
		/* In 1st run of this function test list is empty
		 * and 'if' is true, this part of function is done.
		 * That 'if' part ends with ftl_mngt_continue_step,
		 * so function will be called once again.
		 * Element is added to the test list
		 * to invoke 'else' in second run */
		put_on_list();

		/* check getting device */
		CU_ASSERT_EQUAL(ftl_mngt_get_dev(mngt), dev);
		CU_ASSERT_EQUAL(ftl_mngt_get_dev(mngt), &g_dev);

		/* tests for process context */
		process_cntx_ptr = ftl_mngt_get_process_ctx(mngt);

		/* check if it is possible to retrieve values located
		 * in process context by previous function */
		CU_ASSERT_EQUAL(process_cntx_ptr[0], PROCESS_CNTX_TEST_VAL_0);
		CU_ASSERT_EQUAL(process_cntx_ptr[1], PROCESS_CNTX_TEST_VAL_1);

		/* tests for caller context */
		caller_cntx_ptr = ftl_mngt_get_caller_ctx(mngt);

		/* check previously located values */
		CU_ASSERT_EQUAL(caller_cntx_ptr[0], ' ');
		CU_ASSERT_EQUAL(caller_cntx_ptr[1], 'k');
		CU_ASSERT_EQUAL(caller_cntx_ptr[2], 'a');

		/* insert new */
		caller_cntx_ptr[0] = 'm';
		caller_cntx_ptr[1] = 'i';
		caller_cntx_ptr[2] = 'e';

		/* first run of step so reserved step context
		 * was never used before and should contain 0 */
		CU_ASSERT_EQUAL(*step_cntx_ptr, 0);

		/* this value should be retrieved in second run of function
		 * (in 'else' part) */
		*step_cntx_ptr = STEP_CNTX_TEST_VAL;

		ftl_mngt_continue_step(mngt);
	} else {
		/* In second run retrieved pointer should contain value
		 * allocated for this step in previous run of function */
		CU_ASSERT_EQUAL(*step_cntx_ptr, STEP_CNTX_TEST_VAL);

		status = ftl_mngt_alloc_step_ctx(mngt, sizeof(*step_cntx_ptr));
		SPDK_CU_ASSERT_FATAL(status == 0);
		step_cntx_ptr = ftl_mngt_get_step_ctx(mngt);

		/* now pointer should point to newly allocated context
		 * and be cleaned up (should contain '0') */
		CU_ASSERT_PTR_NOT_NULL(step_cntx_ptr);
		CU_ASSERT_EQUAL(*step_cntx_ptr, 0);

		ftl_mngt_next_step(mngt);
	}
}

static void
fn_3_2_cleanup(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	int *step_cntx_ptr, *process_cntx_ptr;
	char *caller_cntx_ptr;
	int status;

	step_cntx_ptr = ftl_mngt_get_step_ctx(mngt);
	/* context of this step descriptor is never empty
	 * so pointer cannot contain NULL */
	CU_ASSERT_PTR_NOT_NULL(step_cntx_ptr);

	if (check_if_list_empty_and_clean()) {
		/* In 1st run of this function test list is empty
		 * and 'if' is true, this part of function is done.
		 * That 'if' part ends with ftl_mngt_continue_step,
		 * so function will be called once again.
		 * Element is added to the test list
		 * to invoke 'else' in second run */
		put_on_list();

		/* first run of step so reserved step context
		 * was never used before and should contain 0 */
		CU_ASSERT_EQUAL(*step_cntx_ptr, 0);

		/* this value should be retrieved in second run of function
		 * (in 'else' part) */
		*step_cntx_ptr = STEP_CNTX_TEST_VAL;

		ftl_mngt_continue_step(mngt);
	} else {
		/* In second run retrieved pointer should contain value
		 * allocated for this step in previous run of function */
		CU_ASSERT_EQUAL(*step_cntx_ptr, STEP_CNTX_TEST_VAL);

		status = ftl_mngt_alloc_step_ctx(mngt, sizeof(*step_cntx_ptr));
		SPDK_CU_ASSERT_FATAL(status == 0);
		step_cntx_ptr = ftl_mngt_get_step_ctx(mngt);

		/* now pointer should point to newly allocated context
		 * and be cleaned up (should contain '0') */
		CU_ASSERT_PTR_NOT_NULL(step_cntx_ptr);
		CU_ASSERT_EQUAL(*step_cntx_ptr, 0);

		/* check getting device */
		CU_ASSERT_EQUAL(ftl_mngt_get_dev(mngt), dev);
		CU_ASSERT_EQUAL(ftl_mngt_get_dev(mngt), &g_dev);

		/* tests for process context */
		process_cntx_ptr = ftl_mngt_get_process_ctx(mngt);

		/* 1st get of process context, should be clear ('0' values) */
		CU_ASSERT_EQUAL(process_cntx_ptr[0], 0);
		CU_ASSERT_EQUAL(process_cntx_ptr[1], 0);

		/* Random values put in process context.
		 * Should be retrieved in the next function
		 * (it is common space for the entire process) */
		process_cntx_ptr[0] = PROCESS_CNTX_TEST_VAL_0;
		process_cntx_ptr[1] = PROCESS_CNTX_TEST_VAL_1;

		/* tests for caller context */
		caller_cntx_ptr = ftl_mngt_get_caller_ctx(mngt);

		/* check previously located values */
		CU_ASSERT_EQUAL(caller_cntx_ptr[0], 'm');
		CU_ASSERT_EQUAL(caller_cntx_ptr[1], 'i');
		CU_ASSERT_EQUAL(caller_cntx_ptr[2], 'e');

		/* insert new */
		caller_cntx_ptr[0] = 'n';
		caller_cntx_ptr[1] = 'i';
		caller_cntx_ptr[2] = 'a';

		ftl_mngt_next_step(mngt);
	}
}

static void
fn_3_1_cleanup(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	int *step_cntx_ptr, *process_cntx_ptr;
	char *caller_cntx_ptr;
	int status;

	step_cntx_ptr = ftl_mngt_get_step_ctx(mngt);
	if (check_if_list_empty_and_clean()) {
		/* In 1st run of this function test list is empty
		 * and 'if' is true, this part of function is done.
		 * That 'if' part ends with ftl_mngt_continue_step,
		 * so function will be called once again.
		 * Element is added to the test list
		 * to invoke 'else' in second run */
		put_on_list();
		/* this step descriptor does not locate any context
		 * at the beginning,
		 * so pointer should contain NULL */
		CU_ASSERT_PTR_NULL(step_cntx_ptr);

		/* check getting device */
		CU_ASSERT_EQUAL(ftl_mngt_get_dev(mngt), dev);
		CU_ASSERT_EQUAL(ftl_mngt_get_dev(mngt), &g_dev);

		/* tests for process context */
		process_cntx_ptr = ftl_mngt_get_process_ctx(mngt);

		/* check if it is possible to retrieve values located
		 * in process context by previous function */
		CU_ASSERT_EQUAL(process_cntx_ptr[0], PROCESS_CNTX_TEST_VAL_0);
		CU_ASSERT_EQUAL(process_cntx_ptr[1], PROCESS_CNTX_TEST_VAL_1);

		/* tests for caller context */
		caller_cntx_ptr = ftl_mngt_get_caller_ctx(mngt);

		/* check previously located values */
		CU_ASSERT_EQUAL(caller_cntx_ptr[0], 'n');
		CU_ASSERT_EQUAL(caller_cntx_ptr[1], 'i');
		CU_ASSERT_EQUAL(caller_cntx_ptr[2], 'a');

		/* insert new */
		caller_cntx_ptr[0] = '!';
		caller_cntx_ptr[1] = '!';
		caller_cntx_ptr[2] = '!';

		status = ftl_mngt_alloc_step_ctx(mngt, sizeof(*step_cntx_ptr));
		SPDK_CU_ASSERT_FATAL(status == 0);
		step_cntx_ptr = ftl_mngt_get_step_ctx(mngt);
		/* now pointer should point to allocated context */
		CU_ASSERT_PTR_NOT_NULL(step_cntx_ptr);

		/* this value should be retrieved in second run of function
		 * (in 'else' part) */
		*step_cntx_ptr = STEP_CNTX_TEST_VAL;

		ftl_mngt_continue_step(mngt);
	} else {
		/* In second run retrieved pointer is not empty.
		 * Moreover it should contain value allocated for this step
		 * in previous run of function */
		CU_ASSERT_PTR_NOT_NULL(step_cntx_ptr);
		CU_ASSERT_EQUAL(*step_cntx_ptr, STEP_CNTX_TEST_VAL);

		ftl_mngt_next_step(mngt);
	}
}

static struct ftl_mngt_process_desc pdesc_test_3 = {
	.name = "process 3",
	.ctx_size = 2 * sizeof(int),
	.steps = {
		{
			.name = "step 1",
			.action = fn_3_1_action,
			.cleanup = fn_3_1_cleanup
		},
		{
			.name = "step 2",
			.ctx_size = sizeof(int),
			.action = fn_3_2_action,
			.cleanup = fn_3_2_cleanup
		},
		{}
	}
};

static void
test_get_func_and_step_cntx_alloc(void)
{
	char cb_cntx[4] = "daj";

	run_ftl_mngt_with_cb_cntx(ftl_mngt_process_execute, &pdesc_test_3, cb_cntx);

	/* check if caller callback was invoked */
	check_elem_on_list_and_remove(CALLER_CB_RET_VALUE);

	/* check if steps changed cb_cntx correctly */
	CU_ASSERT_EQUAL(cb_cntx[0], 'm');
	CU_ASSERT_EQUAL(cb_cntx[1], 'i');
	CU_ASSERT_EQUAL(cb_cntx[2], 'e');

	run_ftl_mngt_with_cb_cntx(ftl_mngt_process_rollback, &pdesc_test_3, cb_cntx);

	/* check if caller callback was invoked */
	check_elem_on_list_and_remove(CALLER_CB_RET_VALUE);

	/* check if steps changed cb_cntx correctly */
	CU_ASSERT_EQUAL(cb_cntx[0], '!');
	CU_ASSERT_EQUAL(cb_cntx[1], '!');
	CU_ASSERT_EQUAL(cb_cntx[2], '!');

	check_list_empty();
}



/*-
 * test 4
 * tests ftl_mngt_fail_step function
 *
 * In that test one of the action functions fails (third one).
 * Because of that expected result (saved on the test result list)
 * are numbers of the next action function up to failing function.
 * After that cleanup functions are invoked in reversed order.
 */

static void
fn_4_1_action(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	add_elem_to_test_list(1);
	ftl_mngt_next_step(mngt);
}

static void
fn_4_1_cleanup(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	add_elem_to_test_list(-1);
	ftl_mngt_next_step(mngt);
}

static void
fn_4_2_action(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	add_elem_to_test_list(2);
	ftl_mngt_next_step(mngt);
}

static void
fn_4_2_cleanup(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	add_elem_to_test_list(-2);
	ftl_mngt_next_step(mngt);
}

static void
fn_4_3_action(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	add_elem_to_test_list(3);
	/* this action fails, so cleanup should begin now */
	ftl_mngt_fail_step(mngt);
}

static void
fn_4_3_cleanup(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	add_elem_to_test_list(-3);
	ftl_mngt_next_step(mngt);
}

static void
fn_4_4_action(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	CU_FAIL("failure cannot start another action");
	ftl_mngt_next_step(mngt);
}

static struct ftl_mngt_process_desc pdesc_test_4 = {
	.name = "process 4",
	.steps = {
		{
			.name = "step 1",
			.action = fn_4_1_action,
			.cleanup = fn_4_1_cleanup
		},
		{
			.name = "step 2",
			.action = fn_4_2_action,
			.cleanup = fn_4_2_cleanup
		},
		{
			.name = "step 3",
			.action = fn_4_3_action,
			.cleanup = fn_4_3_cleanup
		},
		{
			.name = "step 2",
			.action = fn_4_4_action
		},
		{}
	}
};

static void
test_fail_step(void)
{
	run_ftl_mngt(ftl_mngt_process_execute, &pdesc_test_4);

	/* check proper order of action functions */
	for (int i = 1; i <= 3; i++) {
		check_elem_on_list_and_remove(i);
	}

	/* 3rd action function fails, so now should be
	 * cleanup functions in reverse order */
	for (int i = 3; i > 0; i--) {
		check_elem_on_list_and_remove(-i);
	}

	/* check if caller callback was invoked */
	check_elem_on_list_and_remove(CALLER_CB_RET_VALUE);

	check_list_empty();
}

/*-
 * test 5
 * tests ftl_mngt_call_process and ftl_mngt_call_process_rollback functions
 * tests only proper flow without failures
 */

static void
fn_5_2_1_action(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	add_elem_to_test_list(21);
	ftl_mngt_next_step(mngt);
}

static void
fn_5_2_1_cleanup(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	add_elem_to_test_list(-21);
	ftl_mngt_next_step(mngt);
}

static void
fn_5_2_2_action(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	add_elem_to_test_list(22);
	ftl_mngt_next_step(mngt);
}

static void
fn_5_2_2_cleanup(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	add_elem_to_test_list(-22);
	ftl_mngt_next_step(mngt);
}

static struct ftl_mngt_process_desc pdesc_test_5_2 = {
	.name = "process nested inside step 2 from process 5",
	.steps = {
		{
			.name = "step 2_1",
			.action = fn_5_2_1_action,
			.cleanup = fn_5_2_1_cleanup
		},
		{
			.name = "step 2_2",
			.action = fn_5_2_2_action,
			.cleanup = fn_5_2_2_cleanup
		},
		{}
	}
};

static void
fn_5_3_1_action(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	add_elem_to_test_list(31);
	ftl_mngt_next_step(mngt);
}

static void
fn_5_3_1_cleanup(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	add_elem_to_test_list(-31);
	ftl_mngt_next_step(mngt);
}

static void
fn_5_3_2_action(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	add_elem_to_test_list(32);
	ftl_mngt_next_step(mngt);
}

static void
fn_5_3_2_cleanup(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	add_elem_to_test_list(-32);
	ftl_mngt_next_step(mngt);
}

static struct ftl_mngt_process_desc pdesc_test_5_3 = {
	.name = "process nested inside step 2 from process 5",
	.steps = {
		{
			.name = "step 3_1",
			.action = fn_5_3_1_action,
			.cleanup = fn_5_3_1_cleanup
		},
		{
			.name = "step 3_2",
			.action = fn_5_3_2_action,
			.cleanup = fn_5_3_2_cleanup
		},
		{}
	}
};

static void
fn_5_1_action(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	add_elem_to_test_list(1);
	ftl_mngt_next_step(mngt);
}

static void
fn_5_1_cleanup(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	add_elem_to_test_list(-1);
	ftl_mngt_next_step(mngt);
}

static void
fn_5_2_action(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	add_elem_to_test_list(2);
	ftl_mngt_call_process(mngt, &pdesc_test_5_2);
}

static void
fn_5_2_cleanup(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	add_elem_to_test_list(-2);
	ftl_mngt_call_process_rollback(mngt, &pdesc_test_5_2);
}

static void
fn_5_3_action(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	add_elem_to_test_list(3);
	ftl_mngt_call_process_rollback(mngt, &pdesc_test_5_3);
}

static void
fn_5_3_cleanup(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	add_elem_to_test_list(-3);
	ftl_mngt_call_process(mngt, &pdesc_test_5_3);
}

static struct ftl_mngt_process_desc pdesc_test_5 = {
	.name = "process 5 main",
	.steps = {
		{
			.name = "step 1",
			.action = fn_5_1_action,
			.cleanup = fn_5_1_cleanup
		},
		{
			.name = "step 2",
			.action = fn_5_2_action,
			.cleanup = fn_5_2_cleanup
		},
		{
			.name = "step 3",
			.action = fn_5_3_action,
			.cleanup = fn_5_3_cleanup
		},
		{}
	}
};

static void
test_mngt_call_and_call_rollback(void)
{
	run_ftl_mngt(ftl_mngt_process_execute, &pdesc_test_5);

	check_elem_on_list_and_remove(1);
	check_elem_on_list_and_remove(2);
	check_elem_on_list_and_remove(21);
	check_elem_on_list_and_remove(22);
	check_elem_on_list_and_remove(3);
	check_elem_on_list_and_remove(-32);
	check_elem_on_list_and_remove(-31);

	/* check if caller callback was invoked */
	check_elem_on_list_and_remove(CALLER_CB_RET_VALUE);

	run_ftl_mngt(ftl_mngt_process_rollback, &pdesc_test_5);

	check_elem_on_list_and_remove(-3);
	check_elem_on_list_and_remove(31);
	check_elem_on_list_and_remove(32);
	check_elem_on_list_and_remove(-2);
	check_elem_on_list_and_remove(-22);
	check_elem_on_list_and_remove(-21);
	check_elem_on_list_and_remove(-1);

	/* check if caller callback was invoked */
	check_elem_on_list_and_remove(CALLER_CB_RET_VALUE);

	check_list_empty();
}

/*
 * test 6
 * tests failure inside nested process
 */

static void
fn_6_2_1_action(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	add_elem_to_test_list(21);
	ftl_mngt_next_step(mngt);
}

static void
fn_6_2_1_cleanup(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	add_elem_to_test_list(-21);
	ftl_mngt_next_step(mngt);
}

static void
fn_6_2_2_action(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	add_elem_to_test_list(22);
	/* this action fails, so cleanup should begin now */
	ftl_mngt_fail_step(mngt);
}

static void
fn_6_2_3_action(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	CU_FAIL("failure cannot start another action");
	ftl_mngt_next_step(mngt);
}

static struct ftl_mngt_process_desc pdesc_test_6_2 = {
	.name = "process nested inside step 2 from process 6",
	.steps = {
		{
			.name = "step 6_1",
			.action = fn_6_2_1_action,
			.cleanup = fn_6_2_1_cleanup
		},
		{
			.name = "step 6_2",
			.action = fn_6_2_2_action
		},
		{
			.name = "step 6_3",
			.action = fn_6_2_3_action
		},
		{}
	}
};

static void
fn_6_1_action(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	add_elem_to_test_list(1);
	ftl_mngt_next_step(mngt);
}

static void
fn_6_2_action(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	add_elem_to_test_list(2);
	ftl_mngt_call_process(mngt, &pdesc_test_6_2);
}

static void
fn_6_2_cleanup(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	add_elem_to_test_list(-2);
	ftl_mngt_next_step(mngt);
}

static void
fn_6_3_action(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	CU_FAIL("failure cannot start another action");
	ftl_mngt_next_step(mngt);
}

static struct ftl_mngt_process_desc pdesc_test_6 = {
	.name = "process 6 main",
	.steps = {
		{
			.name = "step 1",
			.action = fn_6_1_action
		},
		{
			.name = "step 2",
			.action = fn_6_2_action,
			.cleanup = fn_6_2_cleanup
		},
		{
			.name = "step 3",
			.action = fn_6_3_action
		},
		{}
	}
};

static void
test_nested_process_failure(void)
{
	run_ftl_mngt(ftl_mngt_process_execute, &pdesc_test_6);

	check_elem_on_list_and_remove(1);
	check_elem_on_list_and_remove(2);
	check_elem_on_list_and_remove(21);
	check_elem_on_list_and_remove(22);
	check_elem_on_list_and_remove(-21);
	check_elem_on_list_and_remove(-2);

	/* check if caller callback was invoked */
	check_elem_on_list_and_remove(CALLER_CB_RET_VALUE);

	check_list_empty();
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("ftl_mngt", setup_test_list, NULL);

	CU_ADD_TEST(suite, test_next_step);
	CU_ADD_TEST(suite, test_continue_step);
	CU_ADD_TEST(suite, test_get_func_and_step_cntx_alloc);
	CU_ADD_TEST(suite, test_fail_step);
	CU_ADD_TEST(suite, test_mngt_call_and_call_rollback);
	CU_ADD_TEST(suite, test_nested_process_failure);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
