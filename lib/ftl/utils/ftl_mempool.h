/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#ifndef FTL_MEMPOOL_H
#define FTL_MEMPOOL_H

#include "spdk/stdinc.h"

#include "ftl_df.h"

/* TODO: Consider porting this mempool to general SPDK utils */

/**
 * @brief Creates and initializes custom FTL memory pool using DMA kind memory
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
 * Allowed only for the initialized memory pool.
 *
 * @param mpool The memory pool
 *
 * @return Element from memory pool. If memory pool empty it returns NULL.
 */
void *ftl_mempool_get(struct ftl_mempool *mpool);

/**
 * @brief Puts (releases) the element to the memory pool
 *
 * Allowed only for the initialized memory pool.
 *
 * @param mpool The memory pool
 * @param element The element to be released
 */
void ftl_mempool_put(struct ftl_mempool *mpool, void *element);

/**
 * @brief Creates custom FTL memory pool using memory allocated externally
 *
 * The pool is uninitialized.
 * The uninitialized pool is accessible via ftl_mempool_claim_df() and
 * ftl_mempool_release_df() APIs. The pool's free buffer list is initialized
 * to contain only elements that were not claimed using ftl_mempool_claim_df()
 * after the call to ftl_mempool_initialize_ext.
 * See ftl_mempool_initialize_ext().
 *
 * @param buffer Externally allocated underlying memory buffer
 * @param count Count of element in the memory pool
 * @param size Size of elements in the memory pool
 * @param alignment Memory alignment of element in the memory pool
 *
 * @return Pointer to the memory pool
 */
struct ftl_mempool *ftl_mempool_create_ext(void *buffer, size_t count, size_t size,
		size_t alignment);

/**
 * @brief Destroys the FTL memory pool w/ externally allocated underlying mem buf
 *
 * The external buf is not being freed.
 *
 * @param mpool The memory pool to be destroyed
 */
void ftl_mempool_destroy_ext(struct ftl_mempool *mpool);

/**
 * @brief Initialize the FTL memory pool w/ externally allocated mem buf.
 *
 * The pool is initialized to contain only elements that were not claimed.
 * All claimed elements are considered to be in use and will be returned
 * to the pool via ftl_mempool_put() after initialization.
 * After the pool is initialized, it is only accessible via
 * ftl_mempool_get() and ftl_mempool_put() APIs.
 *
 * This function should only be called on an uninitialized pool (ie. created via ftl_mempool_create_ext).
 * Any attempt to initialize an already initialized pool (whether after calling ftl_mempool_create, or
 * calling ftl_mempool_initialize_ext twice) will result in an assert.
 *
 * Depending on the memory pool being initialized or not, the use of the
 * following APIs is as follows:
 * API					uninitialized pool		initialized pool
 * ftl_mempool_get()			disallowed			allowed
 * ftl_mempool_put()			disallowed			allowed
 * ftl_mempool_claim_df()		allowed				disallowed
 * ftl_mempool_release_df()		allowed				disallowed
 *
 * @param mpool The memory pool
 */
void ftl_mempool_initialize_ext(struct ftl_mempool *mpool);

/**
 * @brief Return a df object id for a given pool element.
 *
 * @param mpool			The memory pool
 * @param df_obj_ptr		Pointer to the pool element
 *
 * @return df object id
 */
ftl_df_obj_id ftl_mempool_get_df_obj_id(struct ftl_mempool *mpool, void *df_obj_ptr);

/**
 * @brief Return an element pointer for a given df object id.
 *
 * @param mpool			The memory pool
 * @param df_obj_id		Df object id of a pool element
 *
 * @return Element ptr
 */
void *ftl_mempool_get_df_ptr(struct ftl_mempool *mpool, ftl_df_obj_id df_obj_id);

/**
 * @brief Claim an element for use.
 *
 * Allowed only for the uninitialized memory pool.
 *
 * @param mpool			The memory pool
 * @param df_obj_id		Df object id of a pool element to claim
 *
 * @return Element ptr
 */
void *ftl_mempool_claim_df(struct ftl_mempool *mpool, ftl_df_obj_id df_obj_id);

/**
 * @brief Release an element to the pool.
 *
 * Allowed only for the uninitialized memory pool.
 *
 * @param mpool			The memory pool
 * @param df_obj_id		Df object id of a pool element to claim
 */
void ftl_mempool_release_df(struct ftl_mempool *mpool, ftl_df_obj_id df_obj_id);

/**
 * @brief Return an index for a given element in the memory pool.
 *
 * @param mpool			The memory pool
 * @param df_obj_ptr		Element from df memory pool. The pointer may be offset from the beginning of the element.
 *
 * @return Index (offset / element_size) of the element parameter from the beginning of the pool
 */
size_t ftl_mempool_get_df_obj_index(struct ftl_mempool *mpool, void *df_obj_ptr);
#endif /* FTL_MEMPOOL_H */
