/*-
 *   BSD LICENSE
 *
 *   Copyright (c) NetApp, Inc.
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

#include "spdk/histogram.h"
#include "spdk/util.h"
#include "spdk/log.h"
#include "spdk_internal/log.h"
#include "spdk/env.h"

#define HIST_MAX_HIST_ID    999
#define HIST_LARGEST_VALUE  ~0ull
#define HIST_SMALLEST_VALUE 0ull

/*
 * Leveraging histogram bucket concept from perf.c, The bucket
 * for any given I/O is determined solely by the tally delta
 *
 * Each range has a number of buckets determined by NUM_BUCKETS_PER_RANGE
 * which is 128.  The buckets in ranges 0 and 1 each map to one specific
 * tally delta.  The buckets in subsequent ranges each map to twice as many
 * tally deltas as buckets in the range before it:
 *
 * Range 0:  1 tally each - 128 buckets cover 0 to 127 (2^7-1)
 * Range 1:  1 tally each - 128 buckets cover 128 to 255 (2^8-1)
 * Range 2:  2 tally each - 128 buckets cover 256 to 511 (2^9-1)
 * Range 3:  4 tally each - 128 buckets cover 512 to 1023 (2^10-1)
 * Range 4:  8 tally each - 128 buckets cover 1024 to 2047 (2^11-1)
 * Range 5: 16 tally each - 128 buckets cover 2048 to 4095 (2^12-1)
 * ...
 * Range 55: 2^54 tally each - 128 buckets cover 2^61 to 2^62-1
 * Range 56: 2^55 tally each - 128 buckets cover 2^62 to 2^63-1
 * Range 57: 2^56 tally each - 128 buckets cover 2^63 to 2^64-1
 *
 * Buckets can be made more granular by increasing SPDK_BUCKET_SHIFT.
 */
#define SPDK_BUCKET_SHIFT 7
#define SPDK_NUM_BUCKETS_PER_RANGE (1ULL << SPDK_BUCKET_SHIFT)
#define SPDK_BUCKET_MASK (SPDK_NUM_BUCKETS_PER_RANGE - 1)
#define SPDK_NUM_BUCKET_RANGES (64 - SPDK_BUCKET_SHIFT + 1)

static TAILQ_HEAD(, spdk_histogram) g_histograms = TAILQ_HEAD_INITIALIZER(g_histograms);
uint32_t g_hist_id = 1;

static uint32_t
spdk_get_bucket_range(uint64_t val)
{
	uint32_t clz, range;

	assert(val != 0);

	clz = __builtin_clzll(val);

	if (clz < SPDK_NUM_BUCKET_RANGES) {
		range = SPDK_NUM_BUCKET_RANGES - clz - 1;
	} else {
		range = 0;
	}

	return range;
}

static uint32_t
spdk_get_bucket_index(uint64_t val, uint32_t range)
{
	uint32_t shift;

	if (range == 0) {
		shift = 0;
	} else {
		shift = range - 1;
	}

	return (val >> shift) & SPDK_BUCKET_MASK;
}

static uint64_t
spdk_get_value_from_bucket(uint32_t range, uint32_t index)
{
	uint64_t val;

	index += 1;
	if (range > 0) {
		val = 1ULL << (range + SPDK_BUCKET_SHIFT - 1);
		val += (uint64_t)index << (range - 1);
	} else {
		val = index;
	}

	return val;
}

/**
 * create a histogram with properties listed in argument, returns NULL if there is not enough memory
 * user to handle such cases
 */
struct spdk_histogram *
spdk_histogram_register(bool enable, const char *name, const char *class_name,
			const char *unit_name)
{
	struct spdk_histogram *hg;
	int i, j;

	if (!class_name || !name || !unit_name) {
		SPDK_ERRLOG("Invalid histogram parameters\n");
		return NULL;
	}

	if (g_hist_id > HIST_MAX_HIST_ID) {
		SPDK_ERRLOG("Max hist id limits reached\n");
		return NULL;
	}

	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "class_name %s, name %s unit_name %s \n",
		      class_name, name, unit_name);

	/* Allocate memory for a histogram. */
	hg = malloc(sizeof(struct spdk_histogram) + SPDK_NUM_BUCKET_RANGES * sizeof(uint64_t *));
	if (!hg) {
		return NULL;
	}

	for (i = 0; i < SPDK_NUM_BUCKET_RANGES; i++) {
		hg->bucket[i] = malloc(SPDK_NUM_BUCKETS_PER_RANGE * sizeof(uint64_t));
		if (!hg->bucket[i] && i) {
			for (j = i; j; j--) {
				free(hg->bucket[j - 1]);
			}
			free(hg);
			return NULL;
		}
	}

	hg->hist_id = g_hist_id++;
	hg->enabled = enable;
	spdk_histogram_clear(hg);

	snprintf(hg->name, sizeof(hg->name), "%s", name);
	snprintf(hg->class_name, sizeof(hg->class_name), "%s", class_name);
	snprintf(hg->unit_name, sizeof(hg->unit_name), "%s", unit_name);

	/* Append the histogram to list */
	TAILQ_INSERT_TAIL(&g_histograms, hg, link);

	return hg;
}

/***
 * Add tally to given histogram object
 */
void
spdk_hstats_tally(struct spdk_histogram  *hg, uint64_t  value)
{
	uint32_t range = spdk_get_bucket_range(value);
	uint32_t index = spdk_get_bucket_index(value, range);
	hg->bucket[range][index]++;

	if (value < hg->value_min)
		hg->value_min = value;

	if (value > hg->value_max)
		hg->value_max = value;

	hg->values++;
	hg->value_total += value;
}

/**
 * This routine clears all tally data for a histogram.
 */
void
spdk_histogram_clear(struct spdk_histogram *hg)
{
	uint32_t i, j;
	hg->values      = 0;
	hg->value_min   = HIST_LARGEST_VALUE;
	hg->value_max   = HIST_SMALLEST_VALUE;
	hg->value_total = 0;

	for (i = 0; i < SPDK_NUM_BUCKET_RANGES; i++) {
		for (j = 0; j < SPDK_NUM_BUCKETS_PER_RANGE; j++) {
			hg->bucket[i][j] = 0;
		}
	}
}

void
spdk_histogram_clear_all(void)
{
	struct spdk_histogram *hg, *tmp;

	TAILQ_FOREACH_SAFE(hg, &g_histograms, link, tmp) {
		spdk_histogram_clear(hg);
	}
}

void
spdk_histogram_free(struct spdk_histogram *hg)
{
	uint32_t i;

	for (i = 0; i < SPDK_NUM_BUCKET_RANGES; i++) {
		if (hg->bucket[i])
			free(hg->bucket[i]);
	}

	TAILQ_REMOVE(&g_histograms, hg, link);
	if (hg) {
		free(hg);
	}
}

/**
 * Find a histogram object using its hist_id number.
 */
struct spdk_histogram *
spdk_histogram_find(uint32_t  hist_id)
{
	struct spdk_histogram *hg, *tmp;

	TAILQ_FOREACH_SAFE(hg, &g_histograms, link, tmp) {
		if (hist_id == hg->hist_id)
			return hg;
	}
	return NULL;
}

static void
spdk_histogram_dump_header(struct spdk_json_write_ctx *w, struct spdk_histogram *hg)
{
	spdk_json_write_name(w, "histogram_name");
	spdk_json_write_string(w, hg->name);

	spdk_json_write_name(w, "ID");
	spdk_json_write_uint32(w, hg->hist_id);

	spdk_json_write_name(w, "class_name");
	spdk_json_write_string(w, hg->class_name);

	spdk_json_write_name(w, "metric");
	spdk_json_write_string(w, hg->unit_name);

	spdk_json_write_name(w, "enabled");
	spdk_json_write_bool(w, hg->enabled ? true : false);
}

void
spdk_histogram_show(struct spdk_json_write_ctx *w, struct spdk_histogram *hg)
{
	uint32_t i, j;
	uint64_t last_bucket, cur_bucket = 0;

	spdk_json_write_object_begin(w);
	spdk_histogram_dump_header(w, hg);

	spdk_json_write_name(w, "total_num_ios");
	spdk_json_write_uint64(w, hg->values);

	spdk_json_write_name(w, "min_value");
	spdk_json_write_uint64(w, (hg->values) ? hg->value_min : 0);

	spdk_json_write_name(w, "max_value");
	spdk_json_write_uint64(w, hg->value_max);

	spdk_json_write_name(w, "total_values");
	spdk_json_write_uint64(w, hg->value_total);

	spdk_json_write_name(w, "histogram_data");
	spdk_json_write_array_begin(w);
	for (i = 0; i < SPDK_NUM_BUCKET_RANGES; i++) {
		for (j = 0; j < SPDK_NUM_BUCKETS_PER_RANGE; j++) {
			last_bucket = cur_bucket;
			cur_bucket = spdk_get_value_from_bucket(i, j);
			if (hg->bucket[i][j] == 0) {
				continue;
			}
			spdk_json_write_object_begin(w);
			spdk_json_write_name(w, "min");
			spdk_json_write_uint64(w, last_bucket);
			spdk_json_write_name(w, "max");
			spdk_json_write_uint64(w, cur_bucket);
			spdk_json_write_name(w, "count");
			spdk_json_write_uint64(w, hg->bucket[i][j]);
			spdk_json_write_object_end(w);
		}
	}
	spdk_json_write_array_end(w);
	spdk_json_write_object_end(w);
}

void
spdk_hist_list_ids(struct spdk_json_write_ctx *w)
{
	struct spdk_histogram *hg, *tmp;
	TAILQ_FOREACH_SAFE(hg, &g_histograms, link, tmp) {
		spdk_json_write_object_begin(w);
		spdk_histogram_dump_header(w, hg);
		spdk_json_write_object_end(w);
	}
}
