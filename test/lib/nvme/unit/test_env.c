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

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "spdk/env.h"

void *
spdk_zmalloc(size_t size, size_t align, uint64_t *phys_addr)
{
	void *buf = NULL;
	if (posix_memalign(&buf, align, size)) {
		return NULL;
	}
	*phys_addr = (uint64_t)buf;
	return buf;
}

void spdk_free(void *buf)
{
	free(buf);
}

bool ut_fail_vtophys = false;
uint64_t spdk_vtophys(void *buf)
{
	if (ut_fail_vtophys) {
		return (uint64_t) - 1;
	} else {
		return (uintptr_t)buf;
	}
}

void *
spdk_memzone_reserve(const char *name, size_t len, int socket_id, unsigned flags)
{
	return malloc(len);
}

void *
spdk_memzone_lookup(const char *name)
{
	return NULL;
}

int
spdk_memzone_free(const char *name)
{
	return 0;
}

struct spdk_mempool *
spdk_mempool_create(const char *name, size_t count,
		    size_t ele_size, size_t cache_size)
{
	static int mp = 0;

	return (struct spdk_mempool *)&mp;
}

void
spdk_mempool_free(struct spdk_mempool *mp)
{

}

void *
spdk_mempool_get(struct spdk_mempool *mp)
{
	void *buf;

	if (posix_memalign(&buf, 64, 0x1000)) {
		buf = NULL;
	}

	return buf;
}

void
spdk_mempool_put(struct spdk_mempool *mp, void *ele)
{
	free(ele);
}

bool
spdk_process_is_primary(void)
{
	return true;
}

uint64_t ut_tsc = 0;
uint64_t spdk_get_ticks(void)
{
	return ut_tsc;
}

uint64_t spdk_get_ticks_hz(void)
{
	return 1000000;
}
