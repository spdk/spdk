/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 */

/** \file
 * General utility functions
 */

#ifndef SPDK_UTIL_H
#define SPDK_UTIL_H

/* memset_s is only available if __STDC_WANT_LIB_EXT1__ is set to 1 before including \<string.h\> */
#define __STDC_WANT_LIB_EXT1__ 1

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SPDK_CACHE_LINE_SIZE 64

#define spdk_min(a,b) (((a)<(b))?(a):(b))
#define spdk_max(a,b) (((a)>(b))?(a):(b))

#define SPDK_COUNTOF(arr) (sizeof(arr) / sizeof((arr)[0]))

#define SPDK_CONTAINEROF(ptr, type, member) ((type *)((uintptr_t)ptr - offsetof(type, member)))

/** Returns size of an object pointer by ptr up to and including member */
#define SPDK_SIZEOF(ptr, member) (offsetof(__typeof__(*(ptr)), member) + sizeof((ptr)->member))

/**
 * Get the size of a member of a struct.
 */
#define SPDK_SIZEOF_MEMBER(type, member) (sizeof(((type *)0)->member))

/**
 * Get the number of elements in an array of a struct member
 */
#define SPDK_COUNTOF_MEMBER(type, member) (SPDK_COUNTOF(((type *)0)->member))

#define SPDK_SEC_TO_MSEC 1000ULL
#define SPDK_SEC_TO_USEC 1000000ULL
#define SPDK_SEC_TO_NSEC 1000000000ULL

/* Ceiling division of unsigned integers */
#define SPDK_CEIL_DIV(x,y) (((x)+(y)-1)/(y))

/**
 * Macro to align a value to a given power-of-two. The resultant value
 * will be of the same type as the first parameter, and will be no
 * bigger than the first parameter. Second parameter must be a
 * power-of-two value.
 */
#define SPDK_ALIGN_FLOOR(val, align) \
	(__typeof__(val))((val) & (~((__typeof__(val))((align) - 1))))
/**
 * Macro to align a value to a given power-of-two. The resultant value
 * will be of the same type as the first parameter, and will be no lower
 * than the first parameter. Second parameter must be a power-of-two
 * value.
 */
#define SPDK_ALIGN_CEIL(val, align) \
	SPDK_ALIGN_FLOOR(((val) + ((__typeof__(val)) (align) - 1)), align)

#define SPDK_BIT(n) (1ul << (n))

/**
 * Get a field from a structure with size tracking.  The fourth parameter is
 * optional and can be used to specify the size of the object.  If unset,
 * (obj)->size will be used by default.
 */
#define SPDK_GET_FIELD(obj, field, defval, ...) \
	_SPDK_GET_FIELD(obj, field, defval, ## __VA_ARGS__, (obj)->size)

#define _SPDK_GET_FIELD(obj, field, defval, size, ...) \
	((size) >= (offsetof(__typeof__(*(obj)), field) + sizeof((obj)->field)) ? \
	 (obj)->field : (defval))

uint32_t spdk_u32log2(uint32_t x);

static inline uint32_t
spdk_align32pow2(uint32_t x)
{
	return 1u << (1 + spdk_u32log2(x - 1));
}

uint64_t spdk_u64log2(uint64_t x);

static inline uint64_t
spdk_align64pow2(uint64_t x)
{
	return 1ULL << (1 + spdk_u64log2(x - 1));
}

/**
 * Check if a uint32_t is a power of 2.
 */
static inline bool
spdk_u32_is_pow2(uint32_t x)
{
	if (x == 0) {
		return false;
	}

	return (x & (x - 1)) == 0;
}

/**
 * Check if a uint64_t is a power of 2.
 */
static inline bool
spdk_u64_is_pow2(uint64_t x)
{
	if (x == 0) {
		return false;
	}

	return (x & (x - 1)) == 0;
}

static inline uint64_t
spdk_divide_round_up(uint64_t num, uint64_t divisor)
{
	return (num + divisor - 1) / divisor;
}

struct spdk_single_ioviter {
	struct iovec	*iov;
	size_t		iovcnt;
	size_t		idx;
	size_t		iov_len;
	uint8_t		*iov_base;
};

/**
 * An N-way iovec iterator. Calculate the size, given N, using
 * SPDK_IOVITER_SIZE. For backward compatibility, the structure
 * has a default size of 2 iovecs.
 */
struct spdk_ioviter {
	uint32_t	count;

	union {
		struct spdk_single_ioviter iters_compat[2];
		struct spdk_single_ioviter iters[0];
	};
};

/* count must be greater than or equal to 2 */
#define SPDK_IOVITER_SIZE(count) (sizeof(struct spdk_single_ioviter) * (count - 2) + sizeof(struct spdk_ioviter))

/**
 * Initialize and move to the first common segment of the two given
 * iovecs. See spdk_ioviter_next().
 */
size_t spdk_ioviter_first(struct spdk_ioviter *iter,
			  struct iovec *siov, size_t siovcnt,
			  struct iovec *diov, size_t diovcnt,
			  void **src, void **dst);

/**
 * Initialize and move to the first common segment of the N given
 * iovecs. See spdk_ioviter_nextv().
 */
size_t spdk_ioviter_firstv(struct spdk_ioviter *iter,
			   uint32_t count,
			   struct iovec **iov,
			   size_t *iovcnt,
			   void **out);

/**
 * Move to the next segment in the iterator.
 *
 * This will iterate through the segments of the source and destination
 * and return the individual segments, one by one. For example, if the
 * source consists of one element of length 4k and the destination
 * consists of 4 elements each of length 1k, this function will return
 * 4 1k src+dst pairs of buffers, and then return 0 bytes to indicate
 * the iteration is complete on the fifth call.
 */
size_t spdk_ioviter_next(struct spdk_ioviter *iter, void **src, void **dst);

/**
 * Move to the next segment in the iterator.
 *
 * This will iterate through the segments of the iovecs in the iterator
 * and return the individual segments, one by one. For example, if the
 * set consists one iovec of one element of length 4k and another iovec
 * of 4 elements each of length 1k, this function will return
 * 4 1k pairs of buffers, and then return 0 bytes to indicate
 * the iteration is complete on the fifth call.
 */
size_t spdk_ioviter_nextv(struct spdk_ioviter *iter, void **out);

/**
 * Operate like memset across an iovec.
 */
void
spdk_iov_memset(struct iovec *iovs, int iovcnt, int c);

/**
 * Initialize an iovec with just the single given buffer.
 */
#define SPDK_IOV_ONE(piov, piovcnt, buf, buflen) do {	\
	(piov)->iov_base = (buf);			\
	(piov)->iov_len = (buflen);			\
	*(piovcnt) = 1;					\
} while (0)

/**
 * Copy the data described by the source iovec to the destination iovec.
 *
 * \return The number of bytes copied.
 */
size_t spdk_iovcpy(struct iovec *siov, size_t siovcnt, struct iovec *diov, size_t diovcnt);

/**
 * Same as spdk_iovcpy(), but the src/dst buffers might overlap.
 *
 * \return The number of bytes copied.
 */
size_t spdk_iovmove(struct iovec *siov, size_t siovcnt, struct iovec *diov, size_t diovcnt);

/**
 * Transfer state for iterative copying in or out of an iovec.
 */
struct spdk_iov_xfer {
	struct iovec *iovs;
	int iovcnt;
	int cur_iov_idx;
	size_t cur_iov_offset;
};

/**
 * Initialize a transfer context to point to the given iovec.
 */
void
spdk_iov_xfer_init(struct spdk_iov_xfer *ix, struct iovec *iovs, int iovcnt);

/**
 * Copy from the given buf up to buf_len bytes, into the given ix iovec
 * iterator, advancing the iterator as needed.. Returns the number of bytes
 * copied.
 */
size_t
spdk_iov_xfer_from_buf(struct spdk_iov_xfer *ix, const void *buf, size_t buf_len);

/**
 * Copy from the given ix iovec iterator into the given buf up to buf_len
 * bytes, advancing the iterator as needed. Returns the number of bytes copied.
 */
size_t
spdk_iov_xfer_to_buf(struct spdk_iov_xfer *ix, const void *buf, size_t buf_len);

/**
 * Copy iovs contents to buf through memcpy.
 */
void spdk_copy_iovs_to_buf(void *buf, size_t buf_len, struct iovec *iovs,
			   int iovcnt);

/**
 * Copy buf contents to iovs through memcpy.
 */
void spdk_copy_buf_to_iovs(struct iovec *iovs, int iovcnt, void *buf,
			   size_t buf_len);

/**
 * Scan build is really pessimistic and assumes that mempool functions can
 * dequeue NULL buffers even if they return success. This is obviously a false
 * positive, but the mempool dequeue can be done in a DPDK inline function that
 * we can't decorate with usual assert(buf != NULL). Instead, we'll
 * preinitialize the dequeued buffer array with some dummy objects.
 */
#define SPDK_CLANG_ANALYZER_PREINIT_PTR_ARRAY(arr, arr_size, buf_size) \
	do { \
		static char dummy_buf[buf_size]; \
		int i; \
		for (i = 0; i < arr_size; i++) { \
			arr[i] = (void *)dummy_buf; \
		} \
	} while (0)

/**
 * Add two sequence numbers s1 and s2
 *
 * \param s1 First sequence number
 * \param s2 Second sequence number
 *
 * \return Sum of s1 and s2 based on serial number arithmetic.
 */
static inline uint32_t
spdk_sn32_add(uint32_t s1, uint32_t s2)
{
	return (uint32_t)(s1 + s2);
}

#define SPDK_SN32_CMPMAX	(1U << (32 - 1))

/**
 * Compare if sequence number s1 is less than s2.
 *
 * \param s1 First sequence number
 * \param s2 Second sequence number
 *
 * \return true if s1 is less than s2, or false otherwise.
 */
static inline bool
spdk_sn32_lt(uint32_t s1, uint32_t s2)
{
	return (s1 != s2) &&
	       ((s1 < s2 && s2 - s1 < SPDK_SN32_CMPMAX) ||
		(s1 > s2 && s1 - s2 > SPDK_SN32_CMPMAX));
}

/**
 * Compare if sequence number s1 is greater than s2.
 *
 * \param s1 First sequence number
 * \param s2 Second sequence number
 *
 * \return true if s1 is greater than s2, or false otherwise.
 */
static inline bool
spdk_sn32_gt(uint32_t s1, uint32_t s2)
{
	return (s1 != s2) &&
	       ((s1 < s2 && s2 - s1 > SPDK_SN32_CMPMAX) ||
		(s1 > s2 && s1 - s2 < SPDK_SN32_CMPMAX));
}

/**
 * Copies the value (unsigned char)ch into each of the first \b count characters of the object pointed to by \b data
 * \b data_size is used to check that filling \b count bytes won't lead to buffer overflow
 *
 * \param data Buffer to fill
 * \param data_size Size of the buffer
 * \param ch Fill byte
 * \param count Number of bytes to fill
 */
static inline void
spdk_memset_s(void *data, size_t data_size, int ch, size_t count)
{
#ifdef __STDC_LIB_EXT1__
	/* memset_s was introduced as an optional feature in C11 */
	memset_s(data, data_size, ch, count);
#else
	size_t i;
	volatile unsigned char *buf = (volatile unsigned char *)data;

	if (!buf) {
		return;
	}
	if (count > data_size) {
		count = data_size;
	}

	for (i = 0; i < count; i++) {
		buf[i] = (unsigned char)ch;
	}
#endif
}

#ifdef __cplusplus
}
#endif

#endif
