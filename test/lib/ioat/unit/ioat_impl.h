/* Unit test stubbed version of ioat_impl.h */

#ifndef __IOAT_IMPL_H__
#define __IOAT_IMPL_H__

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

struct spdk_pci_device;

static inline void *
ioat_zmalloc(size_t size, unsigned align, uint64_t *phys_addr)
{
	return calloc(1, size);
}

#define ioat_noop()					do { } while (0)

#define ioat_calloc(tag, num, size, align)		calloc(num, size)
#define ioat_malloc(tag, size, align)			malloc(size)
#define ioat_free(buf)					free(buf)
#define ioat_vtophys(buf)				(uint64_t)(buf)
#define ioat_delay_us(us)				ioat_noop()

#endif /* __IOAT_IMPL_H__ */
