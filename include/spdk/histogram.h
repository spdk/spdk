/*-
 *   BSD LICENSE
 *
 *   Copyright (c) NetApp, Inc.
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

/** \file
 * This file contains services for creating and managing histograms.
 */

#ifndef SPDK_HISTOGRAM_H
#define SPDK_HISTOGRAM_H

#include "spdk/stdinc.h"
#include "spdk/histogram_data.h"
#include "spdk/queue.h"
#include "spdk/json.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SPDK_HIST_LARGEST_VALUE  ~0ull
#define SPDK_HIST_SMALLEST_VALUE 0ull

/*
 * SPDK_histogram contains additional information on top of spdk_histogram_data
 */

struct spdk_histogram {
	uint32_t        hist_id;            /* histogram id for parsing from user scripts. */

	bool            enabled;            /* if true starts collecting stats */
	unsigned char   name[32];           /* name for each histogram structure. */
	unsigned char   unit_name[32];      /* metric of tally value. */
	unsigned char   class_name[32];     /* class name */

	uint64_t        values;             /* track number of tally entry */
	uint64_t        value_min;          /* track min value of tally entry */
	uint64_t        value_max;          /* track max value of tally entry */
	uint64_t        value_total;        /* track total of tally entries */

	struct spdk_histogram_data hd;

	TAILQ_ENTRY(spdk_histogram) link;
};

static inline bool
spdk_histogram_is_enabled(struct spdk_histogram *hg)
{
	return hg->enabled;
}

static inline void
spdk_histogram_enable(struct spdk_histogram *hg)
{
	if (hg) {
		hg->enabled = true;
	}
}

static inline void
spdk_histogram_disable(struct spdk_histogram *hg)
{
	if (hg) {
		hg->enabled = false;
	}
}

static inline bool
spdk_histogram_cleared(struct spdk_histogram *hg)
{
	return !hg->value_total;
}

/**
 * Clears all statistics of given histogram object.
 */
static inline void
spdk_histogram_reset(struct spdk_histogram *hg)
{
	hg->values      = 0;
	hg->value_min   = SPDK_HIST_LARGEST_VALUE;
	hg->value_max   = SPDK_HIST_SMALLEST_VALUE;
	hg->value_total = 0;

	spdk_histogram_data_reset(&hg->hd);
}

static inline void
spdk_histogram_tally(struct spdk_histogram *hg, uint64_t value)
{
	if (!hg || !hg->enabled) {
		return;
	}

	spdk_histogram_data_tally(&hg->hd, value);

	if (value < hg->value_min) {
		hg->value_min = value;
	}

	if (value > hg->value_max) {
		hg->value_max = value;
	}

	hg->values++;
	hg->value_total += value;
}

/**
 * Function create a histogram with attributes to be used by clients
 */
struct spdk_histogram *spdk_histogram_alloc(bool enable, const char *name,
		const char *class_name, const char *unit_name);

/**
 * Clears all statistics of all histogram objects.
 */
void spdk_histogram_reset_all(void);

/**
 *Function returns the histogram object using its id number.
 */
struct spdk_histogram *spdk_histogram_find(uint32_t hist_id);

/**
 * Function prints information for all the histogram objects in json format
 */
void spdk_histogram_dump_json(struct spdk_json_write_ctx *w, struct spdk_histogram *hg);

/**
 *Function lists ids of all histograms
 */
void spdk_hist_list_ids(struct spdk_json_write_ctx *w);

/**
 *Function frees memory of histogram allocated during register
 */
void spdk_histogram_free(struct spdk_histogram *hg);

#ifdef __cplusplus
}
#endif

#endif
