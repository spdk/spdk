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

#ifndef __NVME_IMPL_H__
#define __NVME_IMPL_H__

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#include "spdk/nvme_spec.h"

struct spdk_pci_device;

static inline void *
nvme_malloc(size_t size, unsigned align, uint64_t *phys_addr)
{
	void *buf = NULL;
	if (posix_memalign(&buf, align, size)) {
		return NULL;
	}
	*phys_addr = (uint64_t)buf;
	return buf;
}

#define nvme_free(buf)			free(buf)
#define nvme_get_num_ioq()		8
#define nvme_get_ioq_idx()		0

uint64_t nvme_vtophys(void *buf);
#define NVME_VTOPHYS_ERROR	(0xFFFFFFFFFFFFFFFFULL)

extern uint64_t g_ut_tsc;
#define nvme_get_tsc()			(g_ut_tsc)
#define nvme_get_tsc_hz()		(1000000)

static inline void *
nvme_memzone_reserve(const char *name, size_t len, int socket_id, unsigned flags)
{
	return malloc(len);
}

static inline void *
nvme_memzone_lookup(const char *name)
{
	assert(0);
	return NULL;
}

static inline int
nvme_memzone_free(const char *name)
{
	assert(0);
	return 0;
}

static inline bool
nvme_process_is_primary(void)
{
	return true;
}

#define NVME_SOCKET_ID_ANY -1

typedef unsigned nvme_mempool_t;

static inline nvme_mempool_t *
nvme_mempool_create(const char *name, size_t n,
		    size_t elt_size, size_t cache_size)
{
	static int mp;

	return &mp;
}

static inline void *
nvme_mempool_get(nvme_mempool_t *mp)
{
	void *buf;

	if (posix_memalign(&buf, 64, 0x1000)) {
		buf = NULL;
	}

	return buf;
}

static inline void
nvme_mempool_put(nvme_mempool_t *mp, void *buf)
{
	free(buf);
}

#endif /* __NVME_IMPL_H__ */
