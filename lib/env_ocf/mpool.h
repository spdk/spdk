/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.
 *   All rights reserved.
 */

#ifndef OCF_MPOOL_H
#define OCF_MPOOL_H

enum {
	env_mpool_1,
	env_mpool_2,
	env_mpool_4,
	env_mpool_8,
	env_mpool_16,
	env_mpool_32,
	env_mpool_64,
	env_mpool_128,

	env_mpool_max
};

struct env_mpool;

struct env_mpool *env_mpool_create(uint32_t hdr_size, uint32_t elem_size,
				   int flags, int mpool_max, bool fallback,
				   const uint32_t limits[env_mpool_max],
				   const char *name_perfix, bool zero);

void env_mpool_destroy(struct env_mpool *mpools);

void *env_mpool_new(struct env_mpool *mpool, uint32_t count);

bool env_mpool_del(struct env_mpool *mpool,
		   void *items, uint32_t count);

#endif
