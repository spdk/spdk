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
