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

#include "spdk/histogram_data.h"
#include "spdk/util.h"
#include "spdk/log.h"
#include "spdk_internal/log.h"
#include "spdk/env.h"

#define HIST_MAX_HIST_ID    999
static TAILQ_HEAD(, spdk_histogram_data) g_histograms = TAILQ_HEAD_INITIALIZER(g_histograms);
static uint32_t g_hist_id = 1;

/**
 * create a histogram with properties listed in argument, returns NULL if there is not enough memory
 * user to handle such cases
 */
struct spdk_histogram_data *
spdk_histogram_alloc(bool enable, const char *name, const char *class_name,
		     const char *unit_name)
{
	struct spdk_histogram_data *hg;

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
	hg = malloc(sizeof(struct spdk_histogram_data));
	if (!hg) {
		return NULL;
	}

	spdk_histogram_data_reset(hg);
	hg->hist_id = g_hist_id++;
	hg->enabled = enable;

	snprintf(hg->name, sizeof(hg->name), "%s", name);
	snprintf(hg->class_name, sizeof(hg->class_name), "%s", class_name);
	snprintf(hg->unit_name, sizeof(hg->unit_name), "%s", unit_name);

	/* Append the histogram to list */
	TAILQ_INSERT_TAIL(&g_histograms, hg, link);
	return hg;
}

void
spdk_histogram_data_reset_all(void)
{
	struct spdk_histogram_data *hg, *tmp;

	TAILQ_FOREACH_SAFE(hg, &g_histograms, link, tmp) {
		spdk_histogram_data_reset(hg);
	}
}

void
spdk_histogram_free(struct spdk_histogram_data *hg)
{
	if (hg) {
		TAILQ_REMOVE(&g_histograms, hg, link);
		free(hg);
	}
}

/**
 * Find a histogram object using its hist_id number.
 */
struct spdk_histogram_data *
spdk_histogram_find(uint32_t  hist_id)
{
	struct spdk_histogram_data *hg, *tmp;

	TAILQ_FOREACH_SAFE(hg, &g_histograms, link, tmp) {
		if (hist_id == hg->hist_id)
			return hg;
	}
	return NULL;
}

static void
spdk_histogram_dump_header(struct spdk_json_write_ctx *w, struct spdk_histogram_data *hg)
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
spdk_histogram_dump_json(struct spdk_json_write_ctx *w, struct spdk_histogram_data *hg)
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

	spdk_json_write_name(w, "timestamp_rate");
	spdk_json_write_uint64(w, spdk_get_ticks_hz());

	spdk_json_write_name(w, "histogram_data");
	spdk_json_write_array_begin(w);
	for (i = 0; i < SPDK_HISTOGRAM_NUM_BUCKET_RANGES; i++) {
		for (j = 0; j < SPDK_HISTOGRAM_NUM_BUCKETS_PER_RANGE; j++) {
			last_bucket = cur_bucket;
			cur_bucket = __spdk_histogram_data_get_bucket_start(i, j);
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
	struct spdk_histogram_data *hg, *tmp;
	TAILQ_FOREACH_SAFE(hg, &g_histograms, link, tmp) {
		spdk_json_write_object_begin(w);
		spdk_histogram_dump_header(w, hg);
		spdk_json_write_object_end(w);
	}
}
