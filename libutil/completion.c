/*
 * Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *
 * This software product is a proprietary product of NVIDIA CORPORATION &
 * AFFILIATES (the "Company") and all right, title, and interest in and to the
 * software product, including all associated intellectual property rights, are
 * and shall remain exclusively with the Company.
 *
 * This software product is governed by the End User License Agreement
 * provided with the software product.
 */

#include "completion.h"
#include <stdio.h>
#include <util.h>
#include <errno.h>

static inline void set_timeout(struct timespec *timeout, unsigned int timeout_ms)
{
	clock_gettime(CLOCK_MONOTONIC, timeout);

	timeout->tv_sec += timeout_ms / 1000;

	if (timeout->tv_nsec + ((timeout_ms % 1000) * 1000000) >= 1E9) {
		timeout->tv_sec += 1;
		timeout->tv_nsec += ((timeout_ms % 1000) * 1000000) - 1E9;
	} else {
		timeout->tv_nsec += ((timeout_ms % 1000) * 1000000);
	}
}

void init_completion(struct completion *comp, bool init)
{
	pthread_condattr_t cond_attr;
	pthread_mutex_init(&comp->mutex, NULL);
	pthread_condattr_init(&cond_attr);
	pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
	pthread_cond_init(&comp->cond, &cond_attr);
	comp->completed = init;
}

void destroy_completion(struct completion *comp)
{
	pthread_cond_destroy(&comp->cond);
	pthread_mutex_destroy(&comp->mutex);
}

void complete(struct completion *comp)
{
	pthread_mutex_lock(&comp->mutex);
	comp->completed = true;
	pthread_cond_signal(&comp->cond);
	pthread_mutex_unlock(&comp->mutex);
}

void wait_for_completion(struct completion *comp)
{
	pthread_mutex_lock(&comp->mutex);
	while (!comp->completed)
		pthread_cond_wait(&comp->cond, &comp->mutex);
	comp->completed = false;
	pthread_mutex_unlock(&comp->mutex);
}

void reset_completion(struct completion *comp)
{
	pthread_mutex_lock(&comp->mutex);
	comp->completed = false;
	pthread_mutex_unlock(&comp->mutex);
}

void set_completion(struct completion *comp)
{
	pthread_mutex_lock(&comp->mutex);
	comp->completed = true;
	pthread_mutex_unlock(&comp->mutex);
}

bool is_completed(struct completion *comp)
{
	bool ret;

	pthread_mutex_lock(&comp->mutex);
	ret = comp->completed;
	pthread_mutex_unlock(&comp->mutex);

	return ret;
}

int wait_for_completion_timeout(struct completion *comp,
				unsigned int timeout_ms)
{
	struct timespec timeout;
	int ret = 0;

	set_timeout(&timeout, timeout_ms);
	pthread_mutex_lock(&comp->mutex);
	while (!comp->completed && ret != ETIMEDOUT)
		ret = pthread_cond_timedwait(&comp->cond, &comp->mutex,
					     &timeout);
	comp->completed = false;
	pthread_mutex_unlock(&comp->mutex);
	return ret;
}
