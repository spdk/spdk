/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#ifndef FTL_MEMPOOL_H
#define FTL_MEMPOOL_H

#include "spdk/stdinc.h"

/* TODO: Consider porting this mempool to general SPDK utils */

/**
 * @brief Creates custom FTL memory pool using DMA kind memory
 *
 * The pool is being initialized.
 *
 * @param count Count of element in the memory pool
 * @param size Size of elements in the memory pool
 * @param alignment Memory alignment of element in the memory pool
 * @param socket_id It is the socket identifier in the case of NUMA. The value
 * can be *SOCKET_ID_ANY* if there is no NUMA constraint for the reserved zone.
 *
 * @return Pointer to the memory pool
 */
struct ftl_mempool *ftl_mempool_create(size_t count, size_t size,
				       size_t alignment, int socket_id);

/**
 * @brief Destroys the FTL memory pool

 * @param mpool The memory pool to be destroyed
 */
void ftl_mempool_destroy(struct ftl_mempool *mpool);

/**
 * @brief Gets (allocates) an element from the memory pool
 *
 * @param mpool The memory pool
 *
 * @return Element from memory pool. If memory pool empty it returns NULL.
 */
void *ftl_mempool_get(struct ftl_mempool *mpool);

/**
 * @brief Puts (releases) the element to the memory pool
 *
 * @param mpool The memory pool
 * @param element The element to be released
 */
void ftl_mempool_put(struct ftl_mempool *mpool, void *element);

#endif /* FTL_MEMPOOL_H */
