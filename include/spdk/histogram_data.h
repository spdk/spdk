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

#define SPDK_HISTOGRAM_BUCKET_SHIFT		7
#define SPDK_HISTOGRAM_BUCKET_LSB		(64 - SPDK_HISTOGRAM_BUCKET_SHIFT)
#define SPDK_HISTOGRAM_NUM_BUCKETS_PER_RANGE	(1ULL << SPDK_HISTOGRAM_BUCKET_SHIFT)
#define SPDK_HISTOGRAM_BUCKET_MASK		(SPDK_HISTOGRAM_NUM_BUCKETS_PER_RANGE - 1)
#define SPDK_HISTOGRAM_NUM_BUCKET_RANGES	(SPDK_HISTOGRAM_BUCKET_LSB + 1)

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
 * Buckets can be made more granular by increasing SPDK_HISTOGRAM_BUCKET_SHIFT.  This
 * comes at the cost of additional storage per namespace context to store the bucket data.
 */

struct spdk_histogram_data {

	uint64_t	bucket[SPDK_HISTOGRAM_NUM_BUCKET_RANGES][SPDK_HISTOGRAM_NUM_BUCKETS_PER_RANGE];

};

static inline void
spdk_histogram_data_reset(struct spdk_histogram_data *histogram)
{
	memset(histogram, 0, sizeof(*histogram));
}

static inline uint32_t
__spdk_histogram_data_get_bucket_range(uint64_t datapoint)
{
	uint32_t clz, range;

	assert(datapoint != 0);

	clz = __builtin_clzll(datapoint);

	if (clz <= SPDK_HISTOGRAM_BUCKET_LSB) {
		range = SPDK_HISTOGRAM_BUCKET_LSB - clz;
	} else {
		range = 0;
	}

	return range;
}

static inline uint32_t
__spdk_histogram_data_get_bucket_index(uint64_t datapoint, uint32_t range)
{
	uint32_t shift;

	if (range == 0) {
		shift = 0;
	} else {
		shift = range - 1;
	}

	return (datapoint >> shift) & SPDK_HISTOGRAM_BUCKET_MASK;
}

static inline void
spdk_histogram_data_tally(struct spdk_histogram_data *histogram, uint64_t datapoint)
{
	uint32_t range = __spdk_histogram_data_get_bucket_range(datapoint);
	uint32_t index = __spdk_histogram_data_get_bucket_index(datapoint, range);

	histogram->bucket[range][index]++;
}

static inline uint64_t
__spdk_histogram_data_get_bucket_start(uint32_t range, uint32_t index)
{
	uint64_t bucket;

	index += 1;
	if (range > 0) {
		bucket = 1ULL << (range + SPDK_HISTOGRAM_BUCKET_SHIFT - 1);
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

	for (i = 0; i < SPDK_HISTOGRAM_NUM_BUCKET_RANGES; i++) {
		for (j = 0; j < SPDK_HISTOGRAM_NUM_BUCKETS_PER_RANGE; j++) {
			total += histogram->bucket[i][j];
		}
	}

	so_far = 0;
	bucket = 0;

	for (i = 0; i < SPDK_HISTOGRAM_NUM_BUCKET_RANGES; i++) {
		for (j = 0; j < SPDK_HISTOGRAM_NUM_BUCKETS_PER_RANGE; j++) {
			count = histogram->bucket[i][j];
			so_far += count;
			last_bucket = bucket;
			bucket = __spdk_histogram_data_get_bucket_start(i, j);
			fn(ctx, last_bucket, bucket, count, total, so_far);
		}
	}
}

#ifdef __cplusplus
}
#endif

#endif
