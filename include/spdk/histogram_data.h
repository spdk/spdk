/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

/**
 * \file
 * Generic histogram library
 */

#ifndef _SPDK_HISTOGRAM_DATA_H_
#define _SPDK_HISTOGRAM_DATA_H_

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SPDK_HISTOGRAM_GRANULARITY_DEFAULT	7
#define SPDK_HISTOGRAM_GRANULARITY(h)		h->granularity
#define SPDK_HISTOGRAM_BUCKET_LSB(h)		(64 - SPDK_HISTOGRAM_GRANULARITY(h))
#define SPDK_HISTOGRAM_NUM_BUCKETS_PER_RANGE(h)	(1ULL << SPDK_HISTOGRAM_GRANULARITY(h))
#define SPDK_HISTOGRAM_BUCKET_MASK(h)		(SPDK_HISTOGRAM_NUM_BUCKETS_PER_RANGE(h) - 1)
#define SPDK_HISTOGRAM_NUM_BUCKET_RANGES(h)	(h->max_range - h->min_range + 1)
#define SPDK_HISTOGRAM_NUM_BUCKETS(h)		(SPDK_HISTOGRAM_NUM_BUCKETS_PER_RANGE(h) * \
						 SPDK_HISTOGRAM_NUM_BUCKET_RANGES(h))

/*
 * SPDK histograms are implemented using ranges of bucket arrays.  The most common usage
 * model is using TSC datapoints to capture an I/O latency histogram.  For this usage model,
 * the histogram tracks only TSC deltas - any translation to microseconds is done by the
 * histogram user calling spdk_histogram_data_iterate() to iterate over the buckets to perform
 * the translations.
 *
 * Each range has a number of buckets determined by SPDK_HISTOGRAM_NUM_BUCKETS_PER_RANGE
 * which is 128.  The buckets in ranges 0 and 1 each map to one specific datapoint value.
 * The buckets in subsequent ranges each map to twice as many datapoint values as buckets
 * in the range before it:
 *
 * Range 0:  1 value each  - 128 buckets cover 0 to 127 (2^7-1)
 * Range 1:  1 value each  - 128 buckets cover 128 to 255 (2^8-1)
 * Range 2:  2 values each - 128 buckets cover 256 to 511 (2^9-1)
 * Range 3:  4 values each - 128 buckets cover 512 to 1023 (2^10-1)
 * Range 4:  8 values each - 128 buckets cover 1024 to 2047 (2^11-1)
 * Range 5: 16 values each - 128 buckets cover 2048 to 4095 (2^12-1)
 * ...
 * Range 55: 2^54 values each - 128 buckets cover 2^61 to 2^62-1
 * Range 56: 2^55 values each - 128 buckets cover 2^62 to 2^63-1
 * Range 57: 2^56 values each - 128 buckets cover 2^63 to 2^64-1
 *
 * On a 2.3GHz processor, this strategy results in 50ns buckets in the 7-14us range (sweet
 * spot for Intel Optane SSD latency testing).
 *
 * Buckets can be made more granular by increasing SPDK_HISTOGRAM_GRANULARITY.  This
 * comes at the cost of additional storage per namespace context to store the bucket data.
 * In order to lower number of ranges to shrink unnecessary low and high datapoints
 * min_val and max_val can be specified with spdk_histogram_data_alloc_sized_ext().
 * It will limit the values in histogram to a range [min_val, max_val).
 */

struct spdk_histogram_data {
	uint32_t	granularity;
	uint32_t	min_range;
	uint32_t	max_range;
	uint64_t	*bucket;
};

static inline void
__spdk_histogram_increment(struct spdk_histogram_data *h, uint32_t range, uint32_t index)
{
	uint64_t *count;

	count = &h->bucket[((range - h->min_range) << SPDK_HISTOGRAM_GRANULARITY(h)) + index];
	(*count)++;
}

static inline uint64_t
__spdk_histogram_get_count(const struct spdk_histogram_data *h, uint32_t range, uint32_t index)
{
	return h->bucket[((range - h->min_range) << SPDK_HISTOGRAM_GRANULARITY(h)) + index];
}

static inline uint64_t *
__spdk_histogram_get_bucket(const struct spdk_histogram_data *h, uint32_t range, uint32_t index)
{
	return &h->bucket[((range - h->min_range) << SPDK_HISTOGRAM_GRANULARITY(h)) + index];
}

static inline void
spdk_histogram_data_reset(struct spdk_histogram_data *histogram)
{
	memset(histogram->bucket, 0, SPDK_HISTOGRAM_NUM_BUCKETS(histogram) * sizeof(uint64_t));
}

static inline uint32_t
__spdk_histogram_data_get_bucket_range(struct spdk_histogram_data *h, uint64_t datapoint)
{
	uint32_t clz, range;

	clz = datapoint > 0 ? __builtin_clzll(datapoint) : 64;

	if (clz <= SPDK_HISTOGRAM_BUCKET_LSB(h)) {
		range = SPDK_HISTOGRAM_BUCKET_LSB(h) - clz;
	} else {
		range = 0;
	}

	return range;
}

static inline uint32_t
__spdk_histogram_data_get_bucket_index(struct spdk_histogram_data *h, uint64_t datapoint,
				       uint32_t range)
{
	uint32_t shift;

	if (range == 0) {
		shift = 0;
	} else {
		shift = range - 1;
	}

	return (datapoint >> shift) & SPDK_HISTOGRAM_BUCKET_MASK(h);
}

static inline void
spdk_histogram_data_tally(struct spdk_histogram_data *histogram, uint64_t datapoint)
{
	uint32_t range, index;

	range = __spdk_histogram_data_get_bucket_range(histogram, datapoint);

	if (range < histogram->min_range) {
		range = histogram->min_range;
		index = 0;
	} else if (range > histogram->max_range) {
		range = histogram->max_range;
		index = SPDK_HISTOGRAM_NUM_BUCKETS_PER_RANGE(histogram) - 1;
	} else {
		index = __spdk_histogram_data_get_bucket_index(histogram, datapoint, range);
	}

	__spdk_histogram_increment(histogram, range, index);
}

static inline uint64_t
__spdk_histogram_data_get_bucket_start(const struct spdk_histogram_data *h, uint32_t range,
				       uint32_t index)
{
	uint64_t bucket;

	index += 1;
	if (range > 0) {
		bucket = 1ULL << (range + SPDK_HISTOGRAM_GRANULARITY(h) - 1);
		bucket += (uint64_t)index << (range - 1);
	} else {
		bucket = index;
	}

	return bucket;
}

typedef void (*spdk_histogram_data_fn)(void *ctx, uint64_t start, uint64_t end, uint64_t count,
				       uint64_t total, uint64_t so_far);

static inline void
spdk_histogram_data_iterate(const struct spdk_histogram_data *histogram,
			    spdk_histogram_data_fn fn, void *ctx)
{
	uint64_t i, j, count, so_far, total;
	uint64_t bucket, last_bucket;

	total = 0;

	for (i = histogram->min_range; i <= histogram->max_range; i++) {
		for (j = 0; j < SPDK_HISTOGRAM_NUM_BUCKETS_PER_RANGE(histogram); j++) {
			total += __spdk_histogram_get_count(histogram, i, j);
		}
	}

	so_far = 0;
	bucket = 0;

	for (i = histogram->min_range; i <= histogram->max_range; i++) {
		for (j = 0; j < SPDK_HISTOGRAM_NUM_BUCKETS_PER_RANGE(histogram); j++) {
			count = __spdk_histogram_get_count(histogram, i, j);
			so_far += count;
			last_bucket = bucket;
			bucket = __spdk_histogram_data_get_bucket_start(histogram, i, j);
			fn(ctx, last_bucket, bucket, count, total, so_far);
		}
	}
}

static inline int
spdk_histogram_data_merge(const struct spdk_histogram_data *dst,
			  const struct spdk_histogram_data *src)
{
	uint64_t i;

	/* Histograms with different granularity values cannot be simply
	 * merged, because the buckets represent different ranges of
	 * values.
	 */
	if (dst->granularity != src->granularity) {
		return -EINVAL;
	}

	/* Histogram with different size cannot be simply merged. */
	if (dst->min_range != src->min_range || dst->max_range != src->max_range) {
		return -EINVAL;
	}

	for (i = 0; i < SPDK_HISTOGRAM_NUM_BUCKETS(dst); i++) {
		dst->bucket[i] += src->bucket[i];
	}

	return 0;
}

/**
 * Allocate a histogram data structure with specified granularity. It tracks datapoints
 * from min_val (inclusive) to max_val (exclusive).
 *
 * \param granularity Granularity of the histogram buckets. Each power-of-2 range is
 *                    split into (1 << granularity) buckets.
 * \param min_val The minimum value to be tracked, inclusive.
 * \param max_val The maximum value to be tracked, exclusive.
 *
 * \return A histogram data structure.
 */
static inline struct spdk_histogram_data *
spdk_histogram_data_alloc_sized_ext(uint32_t granularity, uint64_t min_val, uint64_t max_val)
{
	struct spdk_histogram_data *h;

	if (min_val >= max_val) {
		return NULL;
	}

	h = (struct spdk_histogram_data *)calloc(1, sizeof(*h));
	if (h == NULL) {
		return NULL;
	}

	h->granularity = granularity;
	h->min_range = __spdk_histogram_data_get_bucket_range(h, min_val);
	h->max_range = __spdk_histogram_data_get_bucket_range(h, max_val - 1);
	h->bucket = (uint64_t *)calloc(SPDK_HISTOGRAM_NUM_BUCKETS(h), sizeof(uint64_t));
	if (h->bucket == NULL) {
		free(h);
		return NULL;
	}

	return h;
}

static inline struct spdk_histogram_data *
spdk_histogram_data_alloc_sized(uint32_t granularity)
{
	return spdk_histogram_data_alloc_sized_ext(granularity, 0, UINT64_MAX);
}

static inline struct spdk_histogram_data *
spdk_histogram_data_alloc(void)
{
	return spdk_histogram_data_alloc_sized(SPDK_HISTOGRAM_GRANULARITY_DEFAULT);
}

static inline void
spdk_histogram_data_free(struct spdk_histogram_data *h)
{
	if (h == NULL) {
		return;
	}

	free(h->bucket);
	free(h);
}

#ifdef __cplusplus
}
#endif

#endif
