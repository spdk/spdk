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

#include "spdk/stdinc.h"

#include "spdk_internal/mock.h"

#include "spdk/env.h"

/*
 * NOTE:
 * Functions in this file are mocks for SPDK based functions
 * and work conceptually in the same way that mocks work in
 * /lib/ut_mock. However, the globals that control the behavior
 * of the mock are defined here, with each function, as
 * opposed to being defined as part of the macro that defines
 * the stub or wrapper for other types of functions. Be sure
 * to use the correct global variable naming convention when
 * working with these functions. See /lib/ut_mock for details.
 */

/*
 * these stubs have a return value set with one of the MOCK_SET macros
 */
DEFINE_STUB(spdk_process_is_primary, bool, (void), true)

DEFINE_STUB_VP(spdk_memzone_lookup, (const char *name), NULL)

/*
 * these mocks don't fit well with the library macro model because
 * they do 'something' other than just return a pre-set value
 */

/* setup the mock control to pass thru by default */
void *ut_p_spdk_memzone_reserve = MOCK_PASS_THRU_P;
void *
spdk_memzone_reserve(const char *name, size_t len, int socket_id, unsigned flags)
{
	if (ut_p_spdk_memzone_reserve &&
	    ut_p_spdk_memzone_reserve == MOCK_PASS_THRU_P) {
		return malloc(len);
	} else {
		return ut_p_spdk_memzone_reserve;
	}
}

void *
spdk_dma_malloc(size_t size, size_t align, uint64_t *phys_addr)
{
	void *buf = NULL;
	if (posix_memalign(&buf, align, size)) {
		return NULL;
	}
	if (phys_addr) {
		*phys_addr = (uint64_t)buf;
	}
	return buf;
}

int ut_spdk_dma_zmalloc = (int)MOCK_PASS_THRU;
void *ut_p_spdk_dma_zmalloc = &ut_spdk_dma_zmalloc;
void *
spdk_dma_zmalloc(size_t size, size_t align, uint64_t *phys_addr)
{
	if (ut_p_spdk_dma_zmalloc &&
	    ut_spdk_dma_zmalloc == (int)MOCK_PASS_THRU) {
		void *buf = spdk_dma_malloc(size, align, phys_addr);

		if (buf != NULL) {
			memset(buf, 0, size);
		}
		return buf;
	} else {
		return ut_p_spdk_dma_zmalloc;
	}
}

void *
spdk_dma_malloc_socket(size_t size, size_t align, uint64_t *phys_addr, int socket_id)
{
	return spdk_dma_malloc(size, align, phys_addr);
}

void *
spdk_dma_zmalloc_socket(size_t size, size_t align, uint64_t *phys_addr, int socket_id)
{
	return spdk_dma_zmalloc(size, align, phys_addr);
}

void *
spdk_dma_realloc(void *buf, size_t size, size_t align, uint64_t *phys_addr)
{
	return realloc(buf, size);
}

void spdk_dma_free(void *buf)
{
	if (ut_p_spdk_dma_zmalloc &&
	    ut_spdk_dma_zmalloc == (int)MOCK_PASS_THRU) {
		free(buf);
	}
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

void spdk_vtophys_add_phys_region(struct spdk_phys_region *phys)
{
	return;
}

void
spdk_memzone_dump(FILE *f)
{
	return;
}

int
spdk_memzone_free(const char *name)
{
	return 0;
}

struct test_mempool {
	size_t	count;
};

struct spdk_mempool *
spdk_mempool_create(const char *name, size_t count,
		    size_t ele_size, size_t cache_size, int socket_id)
{
	struct test_mempool *mp;

	mp = calloc(1, sizeof(*mp));
	if (mp == NULL) {
		return NULL;
	}

	mp->count = count;

	return (struct spdk_mempool *)mp;
}

void
spdk_mempool_free(struct spdk_mempool *_mp)
{
	struct test_mempool *mp = (struct test_mempool *)_mp;

	free(mp);
}

void *
spdk_mempool_get(struct spdk_mempool *_mp)
{
	struct test_mempool *mp = (struct test_mempool *)_mp;
	void *buf;

	if (mp && mp->count == 0) {
		return NULL;
	}

	if (posix_memalign(&buf, 64, 0x1000)) {
		return NULL;
	} else {
		if (mp) {
			mp->count--;
		}
		return buf;
	}
}

void
spdk_mempool_put(struct spdk_mempool *_mp, void *ele)
{
	struct test_mempool *mp = (struct test_mempool *)_mp;

	if (mp) {
		mp->count++;
	}
	free(ele);
}

size_t
spdk_mempool_count(const struct spdk_mempool *_mp)
{
	struct test_mempool *mp = (struct test_mempool *)_mp;

	if (mp) {
		return mp->count;
	} else {
		return 1024;
	}
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

void spdk_delay_us(unsigned int us)
{
	ut_tsc += us;
}

int
spdk_pci_addr_parse(struct spdk_pci_addr *addr, const char *bdf)
{
	unsigned domain, bus, dev, func;

	if (addr == NULL || bdf == NULL) {
		return -EINVAL;
	}

	if ((sscanf(bdf, "%x:%x:%x.%x", &domain, &bus, &dev, &func) == 4) ||
	    (sscanf(bdf, "%x.%x.%x.%x", &domain, &bus, &dev, &func) == 4)) {
		/* Matched a full address - all variables are initialized */
	} else if (sscanf(bdf, "%x:%x:%x", &domain, &bus, &dev) == 3) {
		func = 0;
	} else if ((sscanf(bdf, "%x:%x.%x", &bus, &dev, &func) == 3) ||
		   (sscanf(bdf, "%x.%x.%x", &bus, &dev, &func) == 3)) {
		domain = 0;
	} else if ((sscanf(bdf, "%x:%x", &bus, &dev) == 2) ||
		   (sscanf(bdf, "%x.%x", &bus, &dev) == 2)) {
		domain = 0;
		func = 0;
	} else {
		return -EINVAL;
	}

	if (bus > 0xFF || dev > 0x1F || func > 7) {
		return -EINVAL;
	}

	addr->domain = domain;
	addr->bus = bus;
	addr->dev = dev;
	addr->func = func;

	return 0;
}

int
spdk_pci_addr_fmt(char *bdf, size_t sz, const struct spdk_pci_addr *addr)
{
	int rc;

	rc = snprintf(bdf, sz, "%04x:%02x:%02x.%x",
		      addr->domain, addr->bus,
		      addr->dev, addr->func);

	if (rc > 0 && (size_t)rc < sz) {
		return 0;
	}

	return -1;
}

int
spdk_pci_addr_compare(const struct spdk_pci_addr *a1, const struct spdk_pci_addr *a2)
{
	if (a1->domain > a2->domain) {
		return 1;
	} else if (a1->domain < a2->domain) {
		return -1;
	} else if (a1->bus > a2->bus) {
		return 1;
	} else if (a1->bus < a2->bus) {
		return -1;
	} else if (a1->dev > a2->dev) {
		return 1;
	} else if (a1->dev < a2->dev) {
		return -1;
	} else if (a1->func > a2->func) {
		return 1;
	} else if (a1->func < a2->func) {
		return -1;
	}

	return 0;
}

uint32_t
spdk_env_get_core_count(void)
{
	return 1;
}
