/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk_internal/cunit.h"

#include "reduce/queue_internal.h"

static void
test_queue_create(void)
{
	struct reduce_queue queue;

	queue_init(&queue);
	CU_ASSERT(queue_empty(&queue) == true);
	CU_ASSERT(queue_full(&queue) == false);
	CU_ASSERT(queue_size(&queue) == 0);
}

static void
test_queue_enqueue_dequeue(void)
{
	int64_t value;
	struct reduce_queue queue;

	queue_init(&queue);

	CU_ASSERT(queue_enqueue(&queue, 10) == true);
	CU_ASSERT(queue_enqueue(&queue, 20) == true);
	CU_ASSERT(queue_enqueue(&queue, 30) == true);
	CU_ASSERT(queue_size(&queue) == 3);

	CU_ASSERT(queue_dequeue(&queue, &value) == true);
	CU_ASSERT(value == 10);
	CU_ASSERT(queue_size(&queue) == 2);

	CU_ASSERT(queue_dequeue(&queue, &value) == true);
	CU_ASSERT(value == 20);
	CU_ASSERT(queue_size(&queue) == 1);
}

static void
test_queue_full(void)
{
	int i;
	struct reduce_queue queue;

	queue_init(&queue);

	for (i = 1; i < REDUCE_QUEUE_CAPACITY_SIZE; i++) {
		CU_ASSERT(queue_enqueue(&queue, i) == true);
	}
	CU_ASSERT(queue_full(&queue) == true);

	CU_ASSERT(queue_enqueue(&queue, 40) == false);  /* Queue is full */
}

static void
test_queue_empty(void)
{
	int64_t value;

	struct reduce_queue queue;

	queue_init(&queue);

	CU_ASSERT(queue_empty(&queue) == true);
	CU_ASSERT(queue_enqueue(&queue, 10) == true);
	CU_ASSERT(queue_empty(&queue) == false);

	CU_ASSERT(queue_dequeue(&queue, &value) == true);
	CU_ASSERT(queue_empty(&queue) == true);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("reduce_queue", NULL, NULL);

	CU_ADD_TEST(suite, test_queue_create);
	CU_ADD_TEST(suite, test_queue_enqueue_dequeue);
	CU_ADD_TEST(suite, test_queue_empty);
	CU_ADD_TEST(suite, test_queue_create);
	CU_ADD_TEST(suite, test_queue_full);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();

	return num_failures;
}
