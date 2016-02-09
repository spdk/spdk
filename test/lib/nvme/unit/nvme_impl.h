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
#include <pthread.h>

struct spdk_pci_device;

static inline void *
nvme_malloc(const char *tag, size_t size, unsigned align, uint64_t *phys_addr)
{
	void *buf = calloc(1, size);
	*phys_addr = (uint64_t)buf;
	return buf;
}

#define nvme_free(buf)			free(buf)
#define OUTBUF_SIZE 1024
extern char outbuf[OUTBUF_SIZE];
#define nvme_printf(ctrlr, fmt, args...) snprintf(outbuf, OUTBUF_SIZE, fmt, ##args)
#define nvme_get_num_ioq()		8
#define nvme_get_ioq_idx()		0
#define nvme_assert(check, str)			\
do							\
	{						\
		if (!(check)) {				\
			printf str;			\
			assert(check);			\
		}					\
	}						\
	while (0)

uint64_t nvme_vtophys(void *buf);
#define NVME_VTOPHYS_ERROR	(0xFFFFFFFFFFFFFFFFULL)

#define nvme_alloc_request(bufp)	\
do					\
	{				\
		*bufp = malloc(sizeof(struct nvme_request));	\
	}				\
	while (0)

#define nvme_dealloc_request(buf)	free(buf)

static inline int
nvme_pci_enumerate(int (*enum_cb)(void *enum_ctx, struct spdk_pci_device *pci_dev), void *enum_ctx)
{
	/* TODO: enumeration is not needed in any unit tests yet, so it's not implemented */
	return -1;
}

#define nvme_pcicfg_read32(handle, var, offset)		do { *(var) = 0xFFFFFFFFu; } while (0)
#define nvme_pcicfg_write32(handle, var, offset)	do { (void)(var); } while (0)

static inline
int nvme_pcicfg_map_bar(void *pci_handle, int bar, int read_only, void **addr)
{
	*addr = NULL;
	return 0;
}

static inline int
nvme_pcicfg_unmap_bar(void *devhandle, uint32_t bar, void *addr)
{
	return 0;
}

typedef pthread_mutex_t nvme_mutex_t;

#define nvme_mutex_init(x) pthread_mutex_init((x), NULL)
#define nvme_mutex_destroy(x) pthread_mutex_destroy((x))
#define nvme_mutex_lock pthread_mutex_lock
#define nvme_mutex_unlock pthread_mutex_unlock
#define NVME_MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER

static inline int
nvme_mutex_init_recursive(nvme_mutex_t *mtx)
{
	pthread_mutexattr_t attr;
	int rc = 0;

	if (pthread_mutexattr_init(&attr)) {
		return -1;
	}
	if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) ||
	    pthread_mutex_init(mtx, &attr)) {
		rc = -1;
	}
	pthread_mutexattr_destroy(&attr);
	return rc;
}

/**
 * Copy a struct nvme_command from one memory location to another.
 */
#define nvme_copy_command(dst, src)	memcpy((dst), (src), sizeof(struct spdk_nvme_cmd))

#endif /* __NVME_IMPL_H__ */
