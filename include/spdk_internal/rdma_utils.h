/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2021-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#ifndef SPDK_UTILS_RDMA_H
#define SPDK_UTILS_RDMA_H

#ifdef __cplusplus
extern "C" {
#endif

/* Contains hooks definition */
#include "spdk/nvme.h"

#include <infiniband/verbs.h>

union spdk_rdma_utils_mr {
	struct ibv_mr	*mr;
	uint64_t	key;
};

enum SPDK_RDMA_UTILS_TRANSLATION_TYPE {
	SPDK_RDMA_UTILS_TRANSLATION_MR = 0,
	SPDK_RDMA_UTILS_TRANSLATION_KEY
};

struct spdk_rdma_utils_memory_translation {
	union spdk_rdma_utils_mr mr_or_key;
	uint8_t translation_type;
};

struct spdk_rdma_utils_mem_map;

/**
 * Create a memory map which is used to register Memory Regions and perform address -> memory
 * key translations
 *
 * \param pd Protection Domain which will be used to create Memory Regions
 * \param hooks Optional hooks which are used to create Protection Domain or ger RKey
 * \param access_flags Memory access permissions, bitwise combination of values from \b enum ibv_access_flags
 * \return Pointer to memory map or NULL on failure
 */
struct spdk_rdma_utils_mem_map *
spdk_rdma_utils_create_mem_map(struct ibv_pd *pd, struct spdk_nvme_rdma_hooks *hooks,
			       uint32_t access_flags);

/**
 * Free previously allocated memory map
 *
 * \param map Pointer to memory map to free
 */
void spdk_rdma_utils_free_mem_map(struct spdk_rdma_utils_mem_map **map);

/**
 * Get a translation for the given address and length.
 *
 * Note: the user of this function should use address returned in \b translation structure
 *
 * \param map Pointer to translation map
 * \param address Memory address for translation
 * \param length Length of the memory address
 * \param[in,out] translation Pointer to translation result to be filled by this function
 * \retval -EINVAL if translation is not found
 * \retval 0 translation succeed
 */
int spdk_rdma_utils_get_translation(struct spdk_rdma_utils_mem_map *map, void *address,
				    size_t length, struct spdk_rdma_utils_memory_translation *translation);

/**
 * Helper function for retrieving Local Memory Key. Should be applied to a translation
 * returned by \b spdk_rdma_utils_get_translation
 *
 * \param translation Memory translation
 * \return Local Memory Key
 */
static inline uint32_t
spdk_rdma_utils_memory_translation_get_lkey(
	struct spdk_rdma_utils_memory_translation *translation)
{
	return translation->translation_type == SPDK_RDMA_UTILS_TRANSLATION_MR ?
	       translation->mr_or_key.mr->lkey : (uint32_t)translation->mr_or_key.key;
}

/**
 * Helper function for retrieving Remote Memory Key. Should be applied to a translation
 * returned by \b spdk_rdma_utils_get_translation
 *
 * \param translation Memory translation
 * \return Remote Memory Key
 */
static inline uint32_t
spdk_rdma_utils_memory_translation_get_rkey(
	struct spdk_rdma_utils_memory_translation *translation)
{
	return translation->translation_type == SPDK_RDMA_UTILS_TRANSLATION_MR ?
	       translation->mr_or_key.mr->rkey : (uint32_t)translation->mr_or_key.key;
}

/**
 * Get a Protection Domain for an RDMA device context.
 *
 * \param context RDMA device context
 * \return Pointer to the allocated Protection Domain
 */
struct ibv_pd *
spdk_rdma_utils_get_pd(struct ibv_context *context);

/**
 * Return a Protection Domain.
 *
 * \param pd Pointer to the Protection Domain
 */
void spdk_rdma_utils_put_pd(struct ibv_pd *pd);

/**
 * Get memory domain for the specified protection domain.
 *
 * If memory domain does not exist for the specified protection domain, it will be allocated.
 * If memory domain already exists, reference will be increased.
 *
 * \param pd Protection domain of memory domain
 * \return Pointer to memory domain or NULL;
 */
struct spdk_memory_domain *spdk_rdma_utils_get_memory_domain(struct ibv_pd *pd);

/**
 * Release a reference to a memory domain, which will be destroyed when reference becomes 0.
 *
 * \param _domain Pointer to memory domain
 * \return 0 on success, negated errno on failure
 */
int spdk_rdma_utils_put_memory_domain(struct spdk_memory_domain *_domain);

struct rdma_cm_id;
/**
 * Get the NUMA ID for the local interface associated with the given cm_id.
 *
 * \param cm_id cm_id
 * \return NUMA ID
 */
int32_t spdk_rdma_cm_id_get_numa_id(struct rdma_cm_id *cm_id);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_RDMA_UTILS_H */
