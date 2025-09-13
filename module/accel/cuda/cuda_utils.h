/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2021-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *   Copyright (c) 2025 StarWind Software, Inc. All rights reserved.
 */

#ifndef CUDA_UTILS_H
#define CUDA_UTILS_H

#ifdef __cplusplus
extern "C" {
#endif

struct cuda_mem_map;

/**
 * Create a memory map which is used to register memory with default cuda device
 *
  * \return Pointer to memory map or NULL on failure
 */
struct cuda_mem_map *cuda_utils_create_mem_map(void);

/**
 * Free previously allocated memory map
 *
 * \param map Pointer to memory map to free
 */
void cuda_utils_free_mem_map(struct cuda_mem_map **map);

#ifdef __cplusplus
}
#endif

#endif /* CUDA_UTILS_H */
