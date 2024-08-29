/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#ifndef __REDUCE_STACK_H_
#define __REDUCE_STACK_H_

#include "spdk/stdinc.h"

#define REDUCE_QUEUE_CAPACITY_SIZE 32

struct reduce_queue {
	uint64_t items[REDUCE_QUEUE_CAPACITY_SIZE];
	uint32_t head;
	uint32_t tail;
};

static inline void
queue_init(struct reduce_queue *queue)
{
	queue->head = queue->tail = 0;
}

static inline bool
queue_empty(struct reduce_queue *queue)
{
	return queue->head == queue->tail;
}

static inline bool
queue_full(struct reduce_queue *queue)
{
	return (queue->head == ((queue->tail + 1) % REDUCE_QUEUE_CAPACITY_SIZE));
}

static inline bool
queue_enqueue(struct reduce_queue *queue, uint64_t value)
{
	if (queue_full(queue)) {
		return false;
	}

	queue->items[queue->tail] = value;
	queue->tail = (queue->tail + 1) % REDUCE_QUEUE_CAPACITY_SIZE;
	return true;
}

static inline bool
queue_dequeue(struct reduce_queue *queue, uint64_t *value)
{
	if (queue_empty(queue)) {
		return false;
	}

	*value = queue->items[queue->head];
	queue->head = (queue->head + 1) % REDUCE_QUEUE_CAPACITY_SIZE;
	return true;
}

static inline uint32_t
queue_size(struct reduce_queue *queue)
{
	return (queue->tail + REDUCE_QUEUE_CAPACITY_SIZE - queue->head) % REDUCE_QUEUE_CAPACITY_SIZE;
}

#endif /* __REDUCE_STACK_H_ */
