/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef FTL_MEMPOOL_H
#define FTL_MEMPOOL_H

#include "spdk/stdinc.h"

#include "ftl_df.h"

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
 * ftl_mempool_release_df() APIs. The elements claimed by the ftl_df_obj_id`s
 * are being tracked and can be released before the pool initialization.
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
 * Depending on the memory pool being initialized or not, the use of the
 * following APIs is as follows:
 * API                                  uninitialized pool              initialized pool
 * ftl_mempool_get()			disallowed			allowed
 * ftl_mempool_put()			disallowed			allowed
 * ftl_mempool_claim_df()		allowed				disallowed
 * ftl_mempool_release_df()		allowed				disallowed
 *
 * @param mpool The memory pool
 */
void
ftl_mempool_initialize_ext(struct ftl_mempool *mpool);

/**
 * @brief Return a df object id for a given pool element.
 *
 * @param mpool			The memory pool
 * @param df_obj_ptr	Pointer to the pool element
 *
 * @return df object id
 */
ftl_df_obj_id
ftl_mempool_get_df_obj_id(struct ftl_mempool *mpool, void *df_obj_ptr);

/**
 * @brief Return an element pointer for a given df object id.
 *
 * @param mpool			The memory pool
 * @param df_obj_id		Df object id of a pool element
 *
 * @return Element ptr
 */
void *
ftl_mempool_get_df_ptr(struct ftl_mempool *mpool, ftl_df_obj_id df_obj_id);

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
void *
ftl_mempool_claim_df(struct ftl_mempool *mpool, ftl_df_obj_id df_obj_id);

/**
 * @brief Release an element to the pool.
 *
 * Allowed only for the uninitialized memory pool.
 *
 * @param mpool			The memory pool
 * @param df_obj_id		Df object id of a pool element to claim
 */
void
ftl_mempool_release_df(struct ftl_mempool *mpool, ftl_df_obj_id df_obj_id);

size_t
ftl_mempool_get_elem_id(struct ftl_mempool *mpool, void *element);
#endif /* FTL_MEMPOOL_H */
