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
 *     spdk_histogram_alloc - create an histogram with attributes
 *     spdk_histogram_tally    - populate histogram stats
 *
 * below apis from rpc client to manage histograms
 *  spdk_hist_list_ids       - list histogram list based on internal ids
 *  spdk_histogram_clear     - clear contents of histogram
 *  spdk_histogram_clear_all - clear contents of all histogram
 *  spdk_histogram_dump_json - get histogram stats in json format
 *  spdk_histogram_enable    - enable  data collection for histogram
 *  spdk_histogram_disable   - disable data collection for histogram
 */

#ifndef SPDK_HISTOGRAM_H
#define SPDK_HISTOGRAM_H

#include "spdk/stdinc.h"
#include "spdk/queue.h"
#include "spdk/json.h"


/* Histogram Structure */
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

	TAILQ_ENTRY(spdk_histogram) link;

	uint64_t       *bucket[];           /* bucket array to track counts of tally */
};

/**
 * api to manage histogram stats to be used by clients
 */

#define spdk_histogram_enabled(hg)    ((hg) && (hg)->enabled)

#define spdk_histogram_tally(hg,value) \
        do {if ( spdk_histogram_enabled(hg) ) spdk_hstats_tally((hg),(value));} \
        while (0)

#define spdk_histogram_enable(hg)                 \
        do {if ((hg)) { (hg)->enabled = true;} \
        } while(0)

#define spdk_histogram_disable(hg)                \
        do {if ((hg)) { (hg)->enabled = false;} \
        } while(0)

#define spdk_histogram_cleared(hg) \
            (!hg->value_total)

/**
 * Function create a histogram with attributes to be used by clients
 */
struct spdk_histogram *spdk_histogram_alloc(bool enable, const char *name,
		const char *class_name, const char *unit_name);

/**
 * Add histogram stats(tally data) to a given histogram object.
 */
void  spdk_hstats_tally(struct spdk_histogram *hg, uint64_t value);

/**
 * Clears all statistics of given histogram object.
 */
void spdk_histogram_clear(struct spdk_histogram *hg);

/**
 * Clears all statistics of all histogram objects.
 */
void spdk_histogram_clear_all(void);

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
#endif
