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

#ifndef COMPLETION_H
#define COMPLETION_H

#include <pthread.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct completion {
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	bool completed;
};

void init_completion(struct completion *comp, bool init);
void destroy_completion(struct completion *comp);
void complete(struct completion *comp);
void reset_completion(struct completion *comp);
void set_completion(struct completion *comp);
bool is_completed(struct completion *comp);
void wait_for_completion(struct completion *comp);
int wait_for_completion_timeout(struct completion *comp,
				unsigned int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
