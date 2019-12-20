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
#include "spdk/queue.h"
#include "spdk/util.h"

static uint32_t g_ut_num_cores;
static bool *g_ut_cores;

void allocate_cores(uint32_t num_cores);
void free_cores(void);

DEFINE_STUB(spdk_process_is_primary, bool, (void), true)
DEFINE_STUB(spdk_memzone_lookup, void *, (const char *name), NULL)
DEFINE_STUB(spdk_pci_nvme_get_driver, struct spdk_pci_driver *, (void), NULL)
DEFINE_STUB(spdk_pci_ioat_get_driver, struct spdk_pci_driver *, (void), NULL)
DEFINE_STUB(spdk_pci_virtio_get_driver, struct spdk_pci_driver *, (void), NULL)

void
allocate_cores(uint32_t num_cores)
{
	uint32_t i;

	g_ut_num_cores = num_cores;

	g_ut_cores = calloc(num_cores, sizeof(bool));
	assert(g_ut_cores != NULL);

	for (i = 0; i < num_cores; i++) {
		g_ut_cores[i] = true;
	}
}

void
free_cores(void)
{
	free(g_ut_cores);
	g_ut_cores = NULL;
	g_ut_num_cores = 0;
}

static uint32_t
ut_get_next_core(uint32_t i)
{
	i++;

	while (i < g_ut_num_cores) {
		if (!g_ut_cores[i]) {
			i++;
			continue;
		}
		break;
	}

	if (i < g_ut_num_cores) {
		return i;
	} else {
		return UINT32_MAX;
	}
}

uint32_t
spdk_env_get_first_core(void)
{
	return ut_get_next_core(-1);
}

uint32_t
spdk_env_get_next_core(uint32_t prev_core)
{
	return ut_get_next_core(prev_core);
}

uint32_t
spdk_env_get_core_count(void)
{
	return g_ut_num_cores;
}

uint32_t
spdk_env_get_last_core(void)
{
	uint32_t i;
	uint32_t last_core = UINT32_MAX;

	SPDK_ENV_FOREACH_CORE(i) {
		last_core = i;
	}

	return last_core;
}

DEFINE_RETURN_MOCK(spdk_env_get_current_core, uint32_t);
uint32_t
spdk_env_get_current_core(void)
{
	HANDLE_RETURN_MOCK(spdk_env_get_current_core);

	return UINT32_MAX;
}

DEFINE_RETURN_MOCK(spdk_env_get_socket_id, uint32_t);
uint32_t
spdk_env_get_socket_id(uint32_t core)
{
	HANDLE_RETURN_MOCK(spdk_env_get_socket_id);

	return SPDK_ENV_SOCKET_ID_ANY;
}

/*
 * These mocks don't use the DEFINE_STUB macros because
 * their default implementation is more complex.
 */

DEFINE_RETURN_MOCK(spdk_memzone_reserve, void *);
void *
spdk_memzone_reserve(const char *name, size_t len, int socket_id, unsigned flags)
{
	HANDLE_RETURN_MOCK(spdk_memzone_reserve);

	return malloc(len);
}

DEFINE_RETURN_MOCK(spdk_memzone_reserve_aligned, void *);
void *
spdk_memzone_reserve_aligned(const char *name, size_t len, int socket_id,
			     unsigned flags, unsigned align)
{
	HANDLE_RETURN_MOCK(spdk_memzone_reserve_aligned);

	return malloc(len);
}

DEFINE_RETURN_MOCK(spdk_malloc, void *);
void *
spdk_malloc(size_t size, size_t align, uint64_t *phys_addr, int socket_id, uint32_t flags)
{
	HANDLE_RETURN_MOCK(spdk_malloc);

	void *buf = NULL;

	if (align == 0) {
		align = 8;
	}

	if (posix_memalign(&buf, align, size)) {
		return NULL;
	}
	if (phys_addr) {
		*phys_addr = (uint64_t)buf;
	}

	return buf;
}

DEFINE_RETURN_MOCK(spdk_zmalloc, void *);
void *
spdk_zmalloc(size_t size, size_t align, uint64_t *phys_addr, int socket_id, uint32_t flags)
{
	HANDLE_RETURN_MOCK(spdk_zmalloc);

	void *buf = spdk_malloc(size, align, phys_addr, -1, 1);

	if (buf != NULL) {
		memset(buf, 0, size);
	}
	return buf;
}

DEFINE_RETURN_MOCK(spdk_dma_malloc, void *);
void *
spdk_dma_malloc(size_t size, size_t align, uint64_t *phys_addr)
{
	HANDLE_RETURN_MOCK(spdk_dma_malloc);

	return spdk_malloc(size, align, phys_addr, -1, 1);
}

DEFINE_RETURN_MOCK(spdk_realloc, void *);
void *
spdk_realloc(void *buf, size_t size, size_t align)
{
	HANDLE_RETURN_MOCK(spdk_realloc);

	return realloc(buf, size);
}

DEFINE_RETURN_MOCK(spdk_dma_zmalloc, void *);
void *
spdk_dma_zmalloc(size_t size, size_t align, uint64_t *phys_addr)
{
	HANDLE_RETURN_MOCK(spdk_dma_zmalloc);

	return spdk_zmalloc(size, align, phys_addr, -1, 1);
}

DEFINE_RETURN_MOCK(spdk_dma_malloc_socket, void *);
void *
spdk_dma_malloc_socket(size_t size, size_t align, uint64_t *phys_addr, int socket_id)
{
	HANDLE_RETURN_MOCK(spdk_dma_malloc_socket);

	return spdk_dma_malloc(size, align, phys_addr);
}

DEFINE_RETURN_MOCK(spdk_dma_zmalloc_socket, void *);
void *
spdk_dma_zmalloc_socket(size_t size, size_t align, uint64_t *phys_addr, int socket_id)
{
	HANDLE_RETURN_MOCK(spdk_dma_zmalloc_socket);

	return spdk_dma_zmalloc(size, align, phys_addr);
}

DEFINE_RETURN_MOCK(spdk_dma_realloc, void *);
void *
spdk_dma_realloc(void *buf, size_t size, size_t align, uint64_t *phys_addr)
{
	HANDLE_RETURN_MOCK(spdk_dma_realloc);

	return realloc(buf, size);
}

void
spdk_free(void *buf)
{
	/* fix for false-positives in *certain* static analysis tools. */
	assert((uintptr_t)buf != UINTPTR_MAX);
	free(buf);
}

void
spdk_dma_free(void *buf)
{
	return spdk_free(buf);
}

#ifndef UNIT_TEST_NO_VTOPHYS
DEFINE_RETURN_MOCK(spdk_vtophys, uint64_t);
uint64_t
spdk_vtophys(void *buf, uint64_t *size)
{
	HANDLE_RETURN_MOCK(spdk_vtophys);

	return (uintptr_t)buf;
}
#endif

void
spdk_memzone_dump(FILE *f)
{
	return;
}

DEFINE_RETURN_MOCK(spdk_memzone_free, int);
int
spdk_memzone_free(const char *name)
{
	HANDLE_RETURN_MOCK(spdk_memzone_free);

	return 0;
}

struct test_mempool {
	size_t	count;
	size_t	ele_size;
};

DEFINE_RETURN_MOCK(spdk_mempool_create, struct spdk_mempool *);
struct spdk_mempool *
spdk_mempool_create(const char *name, size_t count,
		    size_t ele_size, size_t cache_size, int socket_id)
{
	struct test_mempool *mp;

	HANDLE_RETURN_MOCK(spdk_mempool_create);

	mp = calloc(1, sizeof(*mp));
	if (mp == NULL) {
		return NULL;
	}

	mp->count = count;
	mp->ele_size = ele_size;

	return (struct spdk_mempool *)mp;
}

void
spdk_mempool_free(struct spdk_mempool *_mp)
{
	struct test_mempool *mp = (struct test_mempool *)_mp;

	free(mp);
}

DEFINE_RETURN_MOCK(spdk_mempool_get, void *);
void *
spdk_mempool_get(struct spdk_mempool *_mp)
{
	struct test_mempool *mp = (struct test_mempool *)_mp;
	size_t ele_size = 0x10000;
	void *buf;

	HANDLE_RETURN_MOCK(spdk_mempool_get);

	if (mp && mp->count == 0) {
		return NULL;
	}

	if (mp) {
		ele_size = mp->ele_size;
	}

	if (posix_memalign(&buf, 64, spdk_align32pow2(ele_size))) {
		return NULL;
	} else {
		if (mp) {
			mp->count--;
		}
		return buf;
	}
}

int
spdk_mempool_get_bulk(struct spdk_mempool *mp, void **ele_arr, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		ele_arr[i] = spdk_mempool_get(mp);
		if (ele_arr[i] == NULL) {
			return -1;
		}
	}
	return 0;
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

void
spdk_mempool_put_bulk(struct spdk_mempool *mp, void **ele_arr, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		spdk_mempool_put(mp, ele_arr[i]);
	}
}

DEFINE_RETURN_MOCK(spdk_mempool_count, size_t);
size_t
spdk_mempool_count(const struct spdk_mempool *_mp)
{
	struct test_mempool *mp = (struct test_mempool *)_mp;

	HANDLE_RETURN_MOCK(spdk_mempool_count);

	if (mp) {
		return mp->count;
	} else {
		return 1024;
	}
}

struct spdk_ring_ele {
	void *ele;
	TAILQ_ENTRY(spdk_ring_ele) link;
};

struct spdk_ring {
	TAILQ_HEAD(, spdk_ring_ele) elements;
	pthread_mutex_t lock;
	size_t count;
};

DEFINE_RETURN_MOCK(spdk_ring_create, struct spdk_ring *);
struct spdk_ring *
spdk_ring_create(enum spdk_ring_type type, size_t count, int socket_id)
{
	struct spdk_ring *ring;

	HANDLE_RETURN_MOCK(spdk_ring_create);

	ring = calloc(1, sizeof(*ring));
	if (!ring) {
		return NULL;
	}

	if (pthread_mutex_init(&ring->lock, NULL)) {
		free(ring);
		return NULL;
	}

	TAILQ_INIT(&ring->elements);
	return ring;
}

void
spdk_ring_free(struct spdk_ring *ring)
{
	struct spdk_ring_ele *ele, *tmp;

	if (!ring) {
		return;
	}

	TAILQ_FOREACH_SAFE(ele, &ring->elements, link, tmp) {
		free(ele);
	}

	pthread_mutex_destroy(&ring->lock);
	free(ring);
}

DEFINE_RETURN_MOCK(spdk_ring_enqueue, size_t);
size_t
spdk_ring_enqueue(struct spdk_ring *ring, void **objs, size_t count,
		  size_t *free_space)
{
	struct spdk_ring_ele *ele;
	size_t i;

	HANDLE_RETURN_MOCK(spdk_ring_enqueue);

	pthread_mutex_lock(&ring->lock);

	for (i = 0; i < count; i++) {
		ele = calloc(1, sizeof(*ele));
		if (!ele) {
			break;
		}

		ele->ele = objs[i];
		TAILQ_INSERT_TAIL(&ring->elements, ele, link);
		ring->count++;
	}

	pthread_mutex_unlock(&ring->lock);
	return i;
}

DEFINE_RETURN_MOCK(spdk_ring_dequeue, size_t);
size_t
spdk_ring_dequeue(struct spdk_ring *ring, void **objs, size_t count)
{
	struct spdk_ring_ele *ele, *tmp;
	size_t i = 0;

	HANDLE_RETURN_MOCK(spdk_ring_dequeue);

	if (count == 0) {
		return 0;
	}

	pthread_mutex_lock(&ring->lock);

	TAILQ_FOREACH_SAFE(ele, &ring->elements, link, tmp) {
		TAILQ_REMOVE(&ring->elements, ele, link);
		ring->count--;
		objs[i] = ele->ele;
		free(ele);
		i++;
		if (i >= count) {
			break;
		}
	}

	pthread_mutex_unlock(&ring->lock);
	return i;

}


DEFINE_RETURN_MOCK(spdk_ring_count, size_t);
size_t
spdk_ring_count(struct spdk_ring *ring)
{
	HANDLE_RETURN_MOCK(spdk_ring_count);
	return ring->count;
}

DEFINE_RETURN_MOCK(spdk_get_ticks, uint64_t);
uint64_t
spdk_get_ticks(void)
{
	HANDLE_RETURN_MOCK(spdk_get_ticks);

	return ut_spdk_get_ticks;
}

DEFINE_RETURN_MOCK(spdk_get_ticks_hz, uint64_t);
uint64_t
spdk_get_ticks_hz(void)
{
	HANDLE_RETURN_MOCK(spdk_get_ticks_hz);

	return 1000000;
}

void
spdk_delay_us(unsigned int us)
{
	/* spdk_get_ticks_hz is 1000000, meaning 1 tick per us. */
	ut_spdk_get_ticks += us;
}

#ifndef UNIT_TEST_NO_PCI_ADDR
DEFINE_RETURN_MOCK(spdk_pci_addr_parse, int);
int
spdk_pci_addr_parse(struct spdk_pci_addr *addr, const char *bdf)
{
	unsigned domain, bus, dev, func;

	HANDLE_RETURN_MOCK(spdk_pci_addr_parse);

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

DEFINE_RETURN_MOCK(spdk_pci_addr_fmt, int);
int
spdk_pci_addr_fmt(char *bdf, size_t sz, const struct spdk_pci_addr *addr)
{
	int rc;

	HANDLE_RETURN_MOCK(spdk_pci_addr_fmt);

	rc = snprintf(bdf, sz, "%04x:%02x:%02x.%x",
		      addr->domain, addr->bus,
		      addr->dev, addr->func);

	if (rc > 0 && (size_t)rc < sz) {
		return 0;
	}

	return -1;
}

DEFINE_RETURN_MOCK(spdk_pci_addr_compare, int);
int
spdk_pci_addr_compare(const struct spdk_pci_addr *a1, const struct spdk_pci_addr *a2)
{
	HANDLE_RETURN_MOCK(spdk_pci_addr_compare);

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
#endif
