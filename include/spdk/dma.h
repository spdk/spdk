/*-
 *   BSD LICENSE
 *
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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
 *     * Neither the name of Nvidia Corporation nor the names of its
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


/** \file
 * SPDK DMA device framework
 */

#ifndef SPDK_DMA_H
#define SPDK_DMA_H

#include "spdk/assert.h"
#include "spdk/queue.h"
#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Identifier of SPDK internal DMA device of RDMA type
 */
#define SPDK_RDMA_DMA_DEVICE "SPDK_RDMA_DMA_DEVICE"

enum spdk_dma_device_type {
	/** RDMA devices are capable of performing DMA operations on memory domains using the standard
	 *  RDMA model (protection domain, remote key, address). */
	SPDK_DMA_DEVICE_TYPE_RDMA,
	/** DMA devices are capable of performing DMA operations on memory domains using physical or
	 *  I/O virtual addresses. */
	SPDK_DMA_DEVICE_TYPE_DMA,
	/**
	 * Start of the range of vendor-specific DMA device types
	 */
	SPDK_DMA_DEVICE_VENDOR_SPECIFIC_TYPE_START = 1000,
	/**
	 * End of the range of vendor-specific DMA device types
	 */
	SPDK_DMA_DEVICE_VENDOR_SPECIFIC_TYPE_END = SPDK_DMA_DEVICE_VENDOR_SPECIFIC_TYPE_START + 999
};

struct spdk_memory_domain;

/**
 * Definition of completion callback to be called by pull or push functions.
 *
 * \param ctx User context passed to pull of push functions
 * \param rc Result of asynchronous data pull or push function
 */
typedef void (*spdk_memory_domain_data_cpl_cb)(void *ctx, int rc);

/**
 * Definition of function which asynchronously pulles data from src_domain to local memory domain.
 * Implementation of this function must call \b cpl_cb only when it returns 0. All other return codes mean failure.
 *
 * \param src_domain Memory domain to which the data buffer belongs
 * \param src_domain_ctx Optional context passed by upper layer with IO request
 * \param src_iov Iov vector in \b src_domain space
 * \param src_iovcnt src_iov array size
 * \param dst_iov Iov vector in local memory domain space, data buffers must be allocated by the caller of
 * this function, total size of data buffers must not be less than the size of data in \b src_iov.
 * \param dst_iovcnt dst_iov array size
 * \param cpl_cb A callback to be called when pull operation completes
 * \param cpl_cb_arg Optional argument to be passed to \b cpl_cb
 * \return 0 on success, negated errno on failure
 */
typedef int (*spdk_memory_domain_pull_data_cb)(struct spdk_memory_domain *src_domain,
		void *src_domain_ctx,
		struct iovec *src_iov, uint32_t src_iovcnt, struct iovec *dst_iov, uint32_t dst_iovcnt,
		spdk_memory_domain_data_cpl_cb cpl_cb, void *cpl_cb_arg);

/**
 * Definition of function which asynchronously pushes data from local memory to destination memory domain.
 * Implementation of this function must call \b cpl_cb only when it returns 0. All other return codes mean failure.
 *
 * \param dst_domain Memory domain to which the data should be pushed
 * \param dst_domain_ctx Optional context passed by upper layer with IO request
 * \param dst_iov Iov vector in dst_domain space
 * \param dst_iovcnt dst_iov array size
 * \param src_iov Iov vector in local memory
 * \param src_iovcnt src_iov array size
 * \param cpl_cb A callback to be called when push operation completes
 * \param cpl_cb_arg Optional argument to be passed to \b cpl_cb
 * \return 0 on success, negated errno on failure
 */
typedef int (*spdk_memory_domain_push_data_cb)(struct spdk_memory_domain *dst_domain,
		void *dst_domain_ctx,
		struct iovec *dst_iov, uint32_t dst_iovcnt, struct iovec *src_iov, uint32_t src_iovcnt,
		spdk_memory_domain_data_cpl_cb cpl_cb, void *cpl_cb_arg);

struct spdk_memory_domain_translation_result {
	/** size of this structure in bytes */
	size_t size;
	/** Number of elements in iov */
	uint32_t iov_count;
	/** Translation results, holds single address, length pair. Should only be used if \b iov_count is 1 */
	struct iovec iov;
	/** Translation results, array of addresses and lengths. Should only be used if \b iov_count is
	 * bigger than 1. The implementer of the translation callback is responsible for allocating and
	 * storing of this array until IO request completes */
	struct iovec *iovs;
	/** Destination domain passed to translation function */
	struct spdk_memory_domain *dst_domain;
	union {
		struct {
			uint32_t lkey;
			uint32_t rkey;
		} rdma;
	};
};

struct spdk_memory_domain_translation_ctx {
	/** size of this structure in bytes */
	size_t size;
	union {
		struct {
			/* Opaque handle for ibv_qp */
			void *ibv_qp;
		} rdma;
	};
};

/**
 * Definition of function which translates data from src_domain to a form accessible by dst_domain.
 *
 * \param src_domain Memory domain to which the data buffer belongs
 * \param src_domain_ctx Optional context passed by upper layer with IO request
 * \param dst_domain Memory domain which determines type of translation function
 * \param dst_domain_ctx Ancillary data for dst_domain
 * \param addr Data buffer address in \b src_domain memory space which should be translated into \b dst_domain
 * \param len Length of data buffer
 * \param result Result of translation function
 * \return 0 on success, negated errno on failure
 */
typedef int (*spdk_memory_domain_translate_memory_cb)(struct spdk_memory_domain *src_domain,
		void *src_domain_ctx, struct spdk_memory_domain *dst_domain,
		struct spdk_memory_domain_translation_ctx *dst_domain_ctx, void *addr, size_t len,
		struct spdk_memory_domain_translation_result *result);

/** Context of memory domain of RDMA type */
struct spdk_memory_domain_rdma_ctx {
	/** size of this structure in bytes */
	size_t size;
	/** Opaque handle for ibv_pd */
	void *ibv_pd;
};

struct spdk_memory_domain_ctx {
	/** size of this structure in bytes */
	size_t size;
	/** Optional user context
	 * Depending on memory domain type, this pointer can be cast to a specific structure,
	 * e.g. to spdk_memory_domain_rdma_ctx structure for RDMA memory domain */
	void *user_ctx;
};

/**
 * Creates a new memory domain of the specified type.
 *
 * Translation functions can be provided to translate addresses from one memory domain to another.
 * If the two domains both use the same addressing scheme for, then this translation does nothing.
 * However, it is possible that the two memory domains may address the same physical memory
 * differently, so this translation step is required.
 *
 * \param domain Double pointer to memory domain to be allocated by this function
 * \param type Type of the DMA device which can access this memory domain
 * \param ctx Optional memory domain context to be copied by this function. Later \b ctx can be
 * retrieved using \ref spdk_memory_domain_get_context function
 * \param id String identifier representing the DMA device that can access this memory domain.
 * \return 0 on success, negated errno on failure
 */
int spdk_memory_domain_create(struct spdk_memory_domain **domain, enum spdk_dma_device_type type,
			      struct spdk_memory_domain_ctx *ctx, const char *id);

/**
 * Set translation function for memory domain. Overwrites existing translation function.
 *
 * \param domain Memory domain
 * \param translate_cb Translation function
 */
void spdk_memory_domain_set_translation(struct spdk_memory_domain *domain,
					spdk_memory_domain_translate_memory_cb translate_cb);

/**
 * Set pull function for memory domain. Overwrites existing pull function.
 *
 * \param domain Memory domain
 * \param pull_cb pull function
 */
void spdk_memory_domain_set_pull(struct spdk_memory_domain *domain,
				 spdk_memory_domain_pull_data_cb pull_cb);

/**
 * Set push function for memory domain. Overwrites existing push function.
 *
 * \param domain Memory domain
 * \param push_cb push function
 */
void spdk_memory_domain_set_push(struct spdk_memory_domain *domain,
				 spdk_memory_domain_push_data_cb push_cb);

/**
 * Get the context passed by the user in \ref spdk_memory_domain_create
 *
 * \param domain Memory domain
 * \return Memory domain context
 */
struct spdk_memory_domain_ctx *spdk_memory_domain_get_context(struct spdk_memory_domain *domain);

/**
 * Get type of the DMA device that can access this memory domain
 *
 * \param domain Memory domain
 * \return DMA device type
 */
enum spdk_dma_device_type spdk_memory_domain_get_dma_device_type(struct spdk_memory_domain *domain);

/**
 * Get an identifier representing the DMA device that can access this memory domain
 * \param domain Memory domain
 * \return DMA device identifier
 */
const char *spdk_memory_domain_get_dma_device_id(struct spdk_memory_domain *domain);

/**
 * Destroy memory domain
 *
 * \param domain Memory domain
 */
void spdk_memory_domain_destroy(struct spdk_memory_domain *domain);

/**
 * Asynchronously pull data which is described by \b src_domain and located in \b src_iov to a location
 * \b dst_iov local memory space.
 *
 * \param src_domain Memory domain in which space data buffer is located
 * \param src_domain_ctx User defined context
 * \param src_iov Source data iov
 * \param src_iov_cnt The number of elements in \b src_iov
 * \param dst_iov Destination iov
 * \param dst_iov_cnt The number of elements in \b dst_iov
 * \param cpl_cb Completion callback
 * \param cpl_cb_arg Completion callback argument
 * \return 0 on success, negated errno on failure. pull_cb implementation must only call the callback when 0
 * is returned
 */
int spdk_memory_domain_pull_data(struct spdk_memory_domain *src_domain, void *src_domain_ctx,
				 struct iovec *src_iov, uint32_t src_iov_cnt, struct iovec *dst_iov, uint32_t dst_iov_cnt,
				 spdk_memory_domain_data_cpl_cb cpl_cb, void *cpl_cb_arg);

/**
 * Asynchronously push data located in local memory to \b dst_domain
 *
 * \param dst_domain Memory domain to which the data should be pushed
 * \param dst_domain_ctx Optional context passed by upper layer with IO request
 * \param dst_iov Iov vector in dst_domain space
 * \param dst_iovcnt dst_iov array size
 * \param src_iov Iov vector in local memory
 * \param src_iovcnt src_iov array size
 * \param cpl_cb Completion callback
 * \param cpl_cb_arg Completion callback argument
 * \return 0 on success, negated errno on failure. push_cb implementation must only call the callback when 0
 * is returned
 */
int spdk_memory_domain_push_data(struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
				 struct iovec *dst_iov, uint32_t dst_iovcnt, struct iovec *src_iov, uint32_t src_iovcnt,
				 spdk_memory_domain_data_cpl_cb cpl_cb, void *cpl_cb_arg);

/**
 * Translate data located in \b src_domain space at address \b addr with size \b len into an equivalent
 * description of memory in dst_domain.
 *
 * This function calls \b src_domain translation callback, the callback needs to be set using \ref
 * spdk_memory_domain_set_translation function.
 * No data is moved during this operation. Both src_domain and dst_domain must describe the same physical memory,
 * just from the point of view of two different memory domain. This is a translation of the description of the memory only.
 * Result of translation is stored in \b result, its content depends on the type of \b dst_domain.
 *
 * \param src_domain Memory domain in which address space data buffer is located
 * \param src_domain_ctx User defined context
 * \param dst_domain Memory domain in which memory space data buffer should be translated
 * \param dst_domain_ctx Ancillary data for dst_domain
 * \param addr Address in \b src_domain memory space
 * \param len Length of the data
 * \param result Translation result. The content of the translation result is only valid if this
 * function returns 0.
 * \return 0 on success, negated errno on failure.
 */
int spdk_memory_domain_translate_data(struct spdk_memory_domain *src_domain, void *src_domain_ctx,
				      struct spdk_memory_domain *dst_domain, struct spdk_memory_domain_translation_ctx *dst_domain_ctx,
				      void *addr, size_t len, struct spdk_memory_domain_translation_result *result);

/**
 * Get the first memory domain.
 *
 * Combined with \ref spdk_memory_domain_get_next to iterate over all memory domains
 *
 * \param id Optional identifier representing the DMA device that can access a memory domain, if set
 * then this function returns the first memory domain which id matches or NULL
 * \return Pointer to the first memory domain or NULL
 */
struct spdk_memory_domain *spdk_memory_domain_get_first(const char *id);

/**
 * Get the next memory domain.
 *
 * \param prev Previous memory domain
 * \param id Optional identifier representing the DMA device that can access a memory domain, if set
 * then this function returns the next memory domain which id matches or NULL
 * \return Pointer to next memory domain or NULL;
 */
struct spdk_memory_domain *spdk_memory_domain_get_next(struct spdk_memory_domain *prev,
		const char *id);


#ifdef __cplusplus
}
#endif

#endif /* SPDK_DMA_H */
