/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2023-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *   Copyright (c) 2025 StarWind Software, Inc. All rights reserved.
 */

#include "spdk/log.h"
#include "spdk/likely.h"
#include "spdk/env.h"
#include "spdk_internal/assert.h"

#include <cuda_runtime_api.h>

#include "cuda_utils.h"

struct cuda_mem_map {
	LIST_ENTRY(cuda_mem_map) link;
	struct spdk_mem_map *map;
	uint32_t ref_count;
};

static struct cuda_mem_map *g_cuda_mem_map = NULL;
static pthread_mutex_t g_cuda_maps_mutex = PTHREAD_MUTEX_INITIALIZER;

static int
cuda_buf_reg(void *buf, size_t size)
{
	SPDK_INFOLOG(cuda_utils, "buf %p, len 0x%lx\n", buf, size);

	if (spdk_unlikely(cudaHostRegister(buf, size, cudaHostRegisterMapped) != cudaSuccess)) {
		SPDK_ERRLOG("failed for buf %p, len 0x%lx\n", buf, size);
		return -ENOMEM;
	}
	return 0;
}

static int
cuda_buf_unreg(void *buf)
{
	SPDK_INFOLOG(cuda_utils, "buf %p\n", buf);
	cudaHostUnregister(buf);
	return 0;
}

static int
cuda_mem_notify(void *cb_ctx, struct spdk_mem_map *map,
		enum spdk_mem_map_notify_action action,
		void *vaddr, size_t size)
{
	int rc;

	switch (action) {
	case SPDK_MEM_MAP_NOTIFY_REGISTER:
		rc = cuda_buf_reg(vaddr, size);
		break;
	case SPDK_MEM_MAP_NOTIFY_UNREGISTER:
		rc = cuda_buf_unreg(vaddr);
		break;
	default:
		SPDK_UNREACHABLE();
	}
	return rc;
}

const struct spdk_mem_map_ops g_cuda_map_ops = {
	.notify_cb = cuda_mem_notify,
	.are_contiguous = NULL
};

struct cuda_mem_map *
cuda_utils_create_mem_map(void)
{
	struct cuda_mem_map *map;

	pthread_mutex_lock(&g_cuda_maps_mutex);

	/* Look up existing mem map registration */
	if (g_cuda_mem_map != NULL) {
		g_cuda_mem_map->ref_count++;
		pthread_mutex_unlock(&g_cuda_maps_mutex);
		return g_cuda_mem_map;
	}

	map = calloc(1, sizeof(*map));
	if (!map) {
		pthread_mutex_unlock(&g_cuda_maps_mutex);
		SPDK_ERRLOG("Memory allocation failed\n");
		return NULL;
	}
	map->ref_count = 1;

	map->map = spdk_mem_map_alloc(0, &g_cuda_map_ops, map);
	if (!map->map) {
		SPDK_ERRLOG("Unable to create memory map\n");
		free(map);
		pthread_mutex_unlock(&g_cuda_maps_mutex);
		return NULL;
	}

	g_cuda_mem_map = map;

	pthread_mutex_unlock(&g_cuda_maps_mutex);

	return map;
}

void
cuda_utils_free_mem_map(struct cuda_mem_map **_map)
{
	struct cuda_mem_map *map;

	if (!_map) {
		return;
	}

	map = *_map;
	if (!map) {
		return;
	}
	*_map = NULL;

	pthread_mutex_lock(&g_cuda_maps_mutex);

	assert(map == g_cuda_mem_map);

	assert(map->ref_count > 0);
	if (--map->ref_count != 0) {
		pthread_mutex_unlock(&g_cuda_maps_mutex);
		return;
	}

	g_cuda_mem_map = NULL;

	pthread_mutex_unlock(&g_cuda_maps_mutex);

	if (map->map) {
		spdk_mem_map_free(&map->map);
	}

	free(map);
}

SPDK_LOG_REGISTER_COMPONENT(cuda_utils)
