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

/** \file
 * This file contains services for creating and managing histograms
 */

/*
 * usage info
 * use below api to create a histogram and tally to populate
 *     spdk_histogram_register - create an histogram with attributes
 *     spdk_histogram_tally    - populate histogram stats
 *
 * below apis from rpc client to manage histograms
 *  spdk_hist_list_ids       - list histogram list based on internal ids
 *  spdk_histogram_clear     - clear contents of histogram
 *  spdk_histogram_clear_all - clear contents of all histogram
 *  spdk_histogram_show      - display information about histogram
 *  spdk_histogram_show_all  - display information about all histograms
 *  spdk_histogram_enable    - enable  data collection for histogram
 *  spdk_histogram_disable   - disable data collection for histogram
 */

#ifndef SPDK_HISTOGRAM_H
#define SPDK_HISTOGRAM_H

#include "spdk/stdinc.h"
#include <spdk/json.h>

/* scale types */
#define HIST_SCALE_LINEAR  0
#define HIST_SCALE_LOG2    1
#define HIST_SCALE_LOG10   2

typedef struct histogram_value_stats {
	uint64_t values;
	uint64_t value_min;
	uint64_t value_max;
	uint64_t value_total;
} value_stats;

/* Histogram Statistics */
typedef struct histogram_stats {
	uint32_t  buckets;          /* number of buckets in histogram. */
	uint32_t  scale;            /* scale of the histogram stats(ex: Log2). */

	uint64_t  bucket_min;       /* minimum size of the bucket. */
	uint64_t  bucket_size;      /* standard bucket size. */
	uint64_t  bucket_max;       /* maximum size of bucket. */

	value_stats  low;           /* holds the values of lowest underflow bucket. */
	value_stats  mid;           /* holds the values which are not low or high. */
	value_stats  hi;            /* holds the values of highest overflow bucket. */
	value_stats  total;         /* holds the cumulative values of stats. */

	uint64_t  underflow_bucket; /* count of low value buckets. */
	uint64_t  overflow_bucket;  /* count of high value buckets. */

	uint64_t  bucket[1];        /* variable length array. */
} hist_stats;

/* Histogram Structure */
typedef struct histogram {
	struct          histogram *next;    /* link to next registered histogram. */
	uint32_t        hist_id;            /* histogram id for parsing from user scripts. */
	bool            enabled;            /* if true starts collecting stats */
	hist_stats      *hstats;            /* histogram stats */
	unsigned char   name[30];           /* name for each histogram structure. */
	unsigned char   unit_name[30];      /* metric of tally value. */
	unsigned char   class_name[5];      /* class identifier. */
} histogram;

/**
 * api to manage histogram stats to be used by clients
 */

#define spdk_histogram_enabled(hg)    ((hg) && (hg)->enabled)

#define spdk_histogram_tally(hg,value) \
        do {if ( spdk_histogram_enabled(hg) ) spdk_hstats_tally(((hg)->hstats),(value));} \
        while (0)

#define spdk_histogram_clear(hg) \
        do {if ((hg)) spdk_hstats_clear((hg)->hstats);} \
        while (0)

#define spdk_histogram_clear_all() \
        do {spdk_hstats_clear_all();} \
        while (0)

#define spdk_histogram_show(hg,level,w) \
        do {if ((hg)) spdk_histogram_show_info((hg),(true),(level),(w));} \
        while (0)

#define spdk_histogram_show_all(level,w) \
        do {spdk_histogram_show_info_all((level),(w));} \
        while (0)

#define spdk_histogram_enable(hg)                 \
        do {if ((hg)) { (hg)->enabled = true;} \
        } while(0)

#define spdk_histogram_disable(hg)                \
        do {if ((hg)) { (hg)->enabled = false;} \
        } while(0)

#define spdk_histogram_cleared(hg) \
            (!hg->hstats->low.value_total && \
             !hg->hstats->hi.value_total && \
             !hg->hstats->mid.value_total && \
             !hg->hstats->total.value_total )

/**
 * Function create a histogram with attributes to be used by clients
 */
histogram *spdk_histogram_register(bool enable, const char *name, const char *class_name,
				   const char *unit_name, uint32_t buckets, uint64_t bucket_min, uint64_t bucket_size,
				   uint32_t scale);

/**
 * Add histogram stats(tally data) to a given histogram object.
 */
void  spdk_hstats_tally(hist_stats *hs, uint64_t value);

/**
 * Clears all statistics of given histogram object.
 */
void spdk_hstats_clear(hist_stats *hs);

/**
 * Clears all statistics of all histogram objects.
 */
void spdk_hstats_clear_all(void);

/**
 *Function returns the histogram object using its id number.
 */
histogram *spdk_histogram_find(uint32_t hist_id);

/**
 * Function prints information for the given histogram object, depending on the level.
 */
void spdk_histogram_show_info(histogram *hg, bool show_header, uint32_t level,
			      struct spdk_json_write_ctx *w);

/**
 * Function prints information for all the histogram objects, depending on level
 */
void spdk_histogram_show_info_all(uint32_t level, struct spdk_json_write_ctx *w);

/**
 *Function lists ids of all histograms to be used for other rpc commands
 */
void spdk_hist_list_ids(struct spdk_json_write_ctx *w);

#endif
