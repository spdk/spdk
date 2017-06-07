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

#include <spdk/histogram.h>
#include <spdk/cmn_utils.h>
#include <spdk/log.h>
#include "spdk_internal/log.h"

#define HIST_MAX_BUCKETS    1000
#define HIST_MAX_HIST_ID    999
#define HIST_LARGEST_VALUE  ~0ull
#define HIST_SMALLEST_VALUE 0ull

/* Display levels */
#define HIST_SHOW_NAMES             1
#define HIST_SHOW_TALLY_SUMMARY     2
#define HIST_SHOW_BUCKET_PCT        3
#define HIST_SHOW_TALLY_NO_ZERO     4
#define HIST_SHOW_TALLY_ALL         5
#define HIST_SHOW_MAX_PCT_BUCKETS   10

#define SPRINT            sprintf

histogram  *histograms;
uint32_t g_hist_id = 1;

void hist_stat_show(hist_stats *hstats, uint32_t level, bool bucket_pct_header,
		    struct spdk_json_write_ctx *w);

void hist_stat_show_header(uint32_t column_width, struct spdk_json_write_ctx *w);

void spdk_histogram_show_summary(histogram *hg, bool show_header,
				 struct spdk_json_write_ctx *w);
uint64_t    hist_stat_max_value(hist_stats *hstats);
uint64_t    hstat_scaled_bucket_start(hist_stats *hstats, uint64_t bucket);
void       value_stat_init(value_stats *vstats);
void       value_stat_tally(value_stats *vstats, uint64_t value);
void       value_stat_show(value_stats *vstats, uint32_t column_width,
			   struct spdk_json_write_ctx *w);
uint64_t   value_stat_max_value(value_stats *vstats);
uint32_t   column_width(uint64_t value);

/**
 * This routine clears all tally data for a histogram.
 */
void
spdk_hstats_clear(hist_stats *hstats)
{
	if (!hstats)
		return;

	value_stat_init(&hstats->low);
	value_stat_init(&hstats->mid);
	value_stat_init(&hstats->hi);
	value_stat_init(&hstats->total);
	hstats->underflow_bucket = 0;
	hstats->overflow_bucket  = 0;
	memset(hstats->bucket, 0, hstats->buckets * sizeof(hstats->bucket[0]));
}

void
spdk_hstats_clear_all(void)
{
	histogram *hg;
	for (hg = histograms; hg; hg = hg->next) {
		spdk_hstats_clear(hg->hstats);
	}
}

/**
 * create a histogram with properties listed in argument
 */
histogram *
spdk_histogram_register(bool enable, const char *name, const char *class_name,
			const char *unit_name, uint32_t buckets,
			uint64_t bucket_min, uint64_t bucket_size, uint32_t scale)
{
	hist_stats *hstats;
	histogram *hg, *hg1, *prev;

	if (!class_name || !name || !unit_name || buckets < 1 ||
	    buckets > HIST_MAX_BUCKETS || !bucket_size) {
		SPDK_ERRLOG("Invalid histogram parameters\n");
		return NULL;
	}

	if (g_hist_id > HIST_MAX_HIST_ID) {
		SPDK_ERRLOG("Max hist id limits reached\n");
		return NULL;
	}


	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "class_name %s, name %s unit_name %s \n",
		      class_name, name, unit_name);
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Id %"PRIu32", enable %d buckets %"PRIx32", bucket_min %"PRIu64", "
		      "bucket_size %"PRIu64", scale %"PRIu32" \n", g_hist_id, enable, buckets,
		      bucket_min, bucket_size, scale);

	/* Allocate memory for a histogram. */
	hg = (histogram *)calloc(1, sizeof(histogram));
	if (!hg)
		return NULL;

	hstats = (hist_stats *)(malloc(sizeof(hist_stats) + (buckets - 1) * sizeof(hstats->bucket[0])));
	if (!hstats) {
		free(hg);
		return NULL;
	}

	hg->hist_id = g_hist_id++;
	hg->enabled = enable;
	hstats->buckets    = buckets;
	hstats->bucket_min  = bucket_min;
	hstats->bucket_max  = bucket_min + buckets * bucket_size;
	hstats->bucket_size = bucket_size;
	hstats->scale  = scale;
	spdk_hstats_clear(hstats);
	hg->hstats = hstats;

	strncpy((char *)hg->class_name, class_name, sizeof(hg->class_name) - 1);
	strncpy((char *)hg->name, name, sizeof(hg->name) - 1);
	strncpy((char *)hg->unit_name, unit_name, sizeof(hg->unit_name) - 1);

	/* Add to the set of all histograms in ascending hist_id order. */
	for (prev = 0, hg1 = histograms; hg1; prev = hg1, hg1 = hg1->next)
		if (g_hist_id < hg1->hist_id) break;

	hg->next = hg1;
	if (prev)
		prev->next = hg;
	else
		histograms = hg;

	return hg;
}

/**
 * Find a histogram object using its hist_id number.
 */
histogram *
spdk_histogram_find(uint32_t  histogram_id)
{
	histogram *hg;

	for (hg = histograms; hg; hg = hg->next) {
		if (histogram_id == hg->hist_id)
			return hg;
	}
	return NULL;
}

/**
 * This routine shows summary information about one histogram.
 */
void
spdk_histogram_show_summary(histogram *hg, bool show_header,
			    struct spdk_json_write_ctx *w)
{
	char str[200];
	hist_stats  *hstats;
	value_stats  *vstats;

	if (show_header) {
		spdk_json_write_string_asis(w,
					    " ----- ------ --------------------------- -------- ----------- ----------- ----------- ----------- \n");
		SPRINT(str, "| %3s | %4s |   %21s   | %6s |    %3s    |    %3s    |    %3s    | %8s  |\n",
		       "id", "Clas", "Histogram name", "Units", "Min", "Max", "Avg", "Values");
		spdk_json_write_string_asis(w, str);
		spdk_json_write_string_asis(w,
					    " ----- ------ --------------------------- -------- ----------- ----------- ----------- ----------- \n");
	}

	if (!hg || !hg->hstats)
		return;

	hstats = hg->hstats;
	if (hstats) {
		vstats = &hstats->total;
		SPRINT(str, "| %3u | %4s | %25s | %6s | "
		       "%9"PRIu64" | %9"PRIu64" | %9"PRIu64" | %9"PRIu64" |\n",
		       hg->hist_id, hg->class_name, hg->name, hg->unit_name,
		       (vstats->values ? vstats->value_min : 0),
		       (vstats->values ? vstats->value_max : 0),
		       (vstats->values ? (vstats->value_total / vstats->values) : 0),
		       vstats->values);
		spdk_json_write_string_asis(w, str);
	}
}

/**
 * This routine displays basic information about one histogram.
 */
void
spdk_histogram_show_info(histogram *hg, bool show_header, uint32_t level,
			 struct spdk_json_write_ctx *w)
{
	char str[256];

	hist_stats  *hstats;

	if (level == HIST_SHOW_TALLY_SUMMARY) {
		spdk_histogram_show_summary(hg, show_header, w);
		return;
	}

	if (show_header || level >= HIST_SHOW_NAMES) {
		SPRINT(str,
		       "\n\t\t -----------\n\t\t| Histogram |\n\t\t -----------\n ----- ------- --------------------------- ------- --------- \n| %3s | %5s | %25s | %5s | %7s |\n ----- ------- --------------------------- ------- --------- \n",

		       "id", "Class", "Histogram name", "Units", "Enabled");
		spdk_json_write_string_asis(w, str);
	}

	SPRINT(str, "| %3u | %5s | %25s | %5s | %7s |\n",
	       hg->hist_id, hg->class_name, hg->name, hg->unit_name,
	       hg->enabled ? "yes" : "no");
	spdk_json_write_string_asis(w, str);

	if (level < HIST_SHOW_NAMES) return;

	/* Show all statistics. */
	if (level != HIST_SHOW_BUCKET_PCT) {
		SPRINT(str, "\n%5s %s %7s %8s %10s %10s %6s\n",
		       "Tally", "E", "Buckets", "Min", "Max", "Size", "Scale");
		spdk_json_write_string_asis(w, str);
	}

	hstats = hg->hstats;
	if (hstats) {
		if (level != HIST_SHOW_BUCKET_PCT) {
			SPRINT(str, "%15u %8"PRIu64" %10"PRIu64" %10"PRIu64" %6s\n",
			       hstats->buckets,
			       hstats->bucket_min,
			       hstats->bucket_max,
			       hstats->bucket_size,
			       hstats->scale == HIST_SCALE_LOG2  ? "Log 2" : hstats->scale == HIST_SCALE_LOG10 ? "Log 10" :
			       "Linear");
			spdk_json_write_string_asis(w, str);
		}
		if (level >= HIST_SHOW_BUCKET_PCT) {
			hist_stat_show(hstats, level, true, w);
		}
	}
	spdk_json_write_string_asis(w, "\n");
}

void
spdk_histogram_show_info_all(uint32_t level, struct spdk_json_write_ctx *w)
{
	histogram *hg;
	bool x = true;
	for (hg = histograms; hg; hg = hg->next) {
		spdk_histogram_show_info(hg, x, level, w);
		x = false;
	}
	if (level == 2)
		spdk_json_write_string_asis(w,
					    " ----- ------ --------------------------- -------- ----------- ----------- ----------- ----------- \n");
}

/***
 * Add tally to given histogram object
 */
void
spdk_hstats_tally(hist_stats  *hstats, uint64_t  value)
{
	if (!hstats)
		return;

	uint32_t  bucketNumber;
	uint64_t  sValue = value;  /* scaled value */

	if (hstats->scale == HIST_SCALE_LINEAR)
		; /* no-op */
	else if (hstats->scale == HIST_SCALE_LOG2)
		sValue = spdk_floor_log2(value);
	else if (hstats->scale == HIST_SCALE_LOG10)
		sValue = spdk_floor_log10(value);
	else
		SPDK_ERRLOG("invalid scale = %u\n", hstats->scale);

	/* Determine which bucket to increment. */
	if (sValue < hstats->bucket_min) {
		/* Increment the underflow bucket. */
		value_stat_tally(&hstats->low, value);
		hstats->underflow_bucket++;
	} else if (sValue >= hstats->bucket_max) {
		/* Increment the overflow bucket. */
		value_stat_tally(&hstats->hi, value);
		hstats->overflow_bucket++;
	} else {
		/* Increment one of the mid range buckets. */
		value_stat_tally(&hstats->mid, value);
		bucketNumber = (uint32_t)((sValue - hstats->bucket_min) / hstats->bucket_size);
		hstats->bucket[bucketNumber]++;
	}

	value_stat_tally(&hstats->total, value);
}

/*******************************************************************************
 * compute the scaled bucket value
 */
uint64_t
hstat_scaled_bucket_start(hist_stats *hstats, uint64_t bucket)
{
	if (hstats->scale == HIST_SCALE_LOG2)
		return spdk_power_fn(2, (hstats->bucket_min + bucket * hstats->bucket_size));

	if (hstats->scale == HIST_SCALE_LOG10)
		return spdk_power_fn(10, (hstats->bucket_min + bucket * hstats->bucket_size));
	return hstats->bucket_min + (bucket * hstats->bucket_size);
}

static  unsigned char    txt2[] = "%*"PRIu64" - %*"PRIu64"";
static  unsigned char    txt3[] = "  %*"PRIu64" + %*s";
static  unsigned char    txt4[] = " %*"PRIu64"";

/*******************************************************************************
 * Show statistics of histogram
 */
void
hist_stat_show(hist_stats *hstats, uint32_t level, bool bucket_pct_header,
	       struct spdk_json_write_ctx *w)
{
	uint32_t  i;
	uint32_t  percent;
	uint32_t  percent_low;
	uint32_t  percent_hi;
	uint32_t  val_stat_cw;
	uint32_t  hstat_cw;
	uint32_t  range_cw;
	uint64_t  min_range;
	uint64_t  max_range;
	uint64_t  trunc_sum;
	char      str[200];

	hstat_cw = column_width(hist_stat_max_value(hstats));
	if (hstat_cw < 6)
		hstat_cw = 6;

	val_stat_cw = column_width(value_stat_max_value(&hstats->total));
	if (val_stat_cw < 6)
		val_stat_cw = 6;

	range_cw = column_width(hstat_scaled_bucket_start(hstats, hstats->buckets));
	if (range_cw < 6)
		range_cw = 6;

	if (level == HIST_SHOW_BUCKET_PCT) {  /* use compressed horizontal version */
		if (bucket_pct_header) {    /* print header at start of each series */
			/* up to HIST_SHOW_MAX_PCT_BUCKETS buckets */
			spdk_json_write_string_asis(w, "\n  Values ::  Low  <: ");
			for (i = 0; i < HIST_SHOW_MAX_PCT_BUCKETS && i < hstats->buckets; i++) {
				SPRINT(str, "%5"PRIu64" ", hstat_scaled_bucket_start(hstats, i));
				spdk_json_write_string_asis(w, str);
			}
			if (i < hstats->buckets)
				SPRINT(str, "[ %"PRIu64" - %"PRIu64" ] :>", /* upper range */
				       hstat_scaled_bucket_start(hstats, i),
				       hstat_scaled_bucket_start(hstats, hstats->buckets));
			else
				SPRINT(str, "  %"PRIu64" ", hstat_scaled_bucket_start(hstats, i));
			spdk_json_write_string_asis(w, str);
			spdk_json_write_string_asis(w, " High \n");
		}
		if (!hstats->total.values) {
			return;
		}

		percent_low = spdk_percent_fn(hstats->low.values, hstats->total.values);
		percent_hi  = spdk_percent_fn(hstats->hi.values,  hstats->total.values);
		SPRINT(str, "%8"PRIu64" :: %u.%02u%% <: ", hstats->total.values,
		       percent_low / 100, percent_low % 100);
		spdk_json_write_string_asis(w, str);

		for (i = 0; i < HIST_SHOW_MAX_PCT_BUCKETS && i < hstats->buckets; i++) {
			percent = spdk_percent_fn(hstats->bucket[i], hstats->total.values);
			SPRINT(str, "%u.%02u%% ", percent / 100, percent % 100);
			spdk_json_write_string_asis(w, str);
		}

		if (i < hstats->buckets) {
			/* overflow print */
			trunc_sum = 0;
			/* pick up any truncated values */
			for (; i < hstats->buckets; i++)
				trunc_sum += hstats->bucket[i];

			percent = spdk_percent_fn(trunc_sum, hstats->total.values);
			SPRINT(str, "[%*s %3u.%02u%% %*s] :>",
			       range_cw - 2, " ", percent / 100, percent % 100,
			       range_cw - 3, " ");
		} else {
			SPRINT(str, "%*s", range_cw - 5, " ");
		}
		spdk_json_write_string_asis(w, str);

		SPRINT(str, " %u.%02u%%\n", percent_hi / 100, percent_hi % 100);
		spdk_json_write_string_asis(w, str);
	} else {
		/* full detail version */
		if (level < HIST_SHOW_TALLY_ALL && !hstats->total.values) {
			spdk_json_write_string_asis(w, "\t\tAll tallies are zero!\n");
			return;
		}
		spdk_json_write_string_asis(w, "\n");
		if (hstats->low.values) {
			uint64_t scaledMin = hstats->bucket_min ?
					     hstat_scaled_bucket_start(hstats, hstats->bucket_min - 1) : 0;
			SPRINT(str, "\nTallies below specified range (<%"PRIu64")\n", scaledMin);
			spdk_json_write_string_asis(w, str);
			spdk_json_write_string_asis(w, "    min       max   freq      percent\n");
			SPRINT(str, (char *)txt2, range_cw, 0LL, range_cw, scaledMin - 1);
			spdk_json_write_string_asis(w, str);
			SPRINT(str, (char *)txt4, hstat_cw, hstats->underflow_bucket);
			spdk_json_write_string_asis(w, str);
			spdk_json_write_string_asis(w, "\n");
			value_stat_show(&hstats->low, val_stat_cw, w);
			spdk_json_write_string_asis(w, "\n");
		}

		if (hstats->mid.values) {
			if (hstats->low.values)
				spdk_json_write_string_asis(w, "---------------------------------------------------------------\n");

			SPRINT(str, "Tallies within range (%"PRIu64" to %"PRIu64" %s step %"PRIu64")\n",
			       hstat_scaled_bucket_start(hstats, 0),
			       hstat_scaled_bucket_start(hstats, hstats->buckets),
			       hstats->scale == HIST_SCALE_LOG2  ? "Log 2" : hstats->scale == HIST_SCALE_LOG10 ? "Log 10" :
			       "Linear",
			       hstats->bucket_size);
			spdk_json_write_string_asis(w, str);

			spdk_json_write_string_asis(w, "    min       max   freq      percent\n");
			for (i = 0; i < hstats->buckets; i++) {
				/* Suppress individual lines with an empty bucket. */
				if (level < HIST_SHOW_TALLY_ALL && hstats->bucket[i] == 0)
					continue;

				min_range = hstat_scaled_bucket_start(hstats, i),
				max_range = hstat_scaled_bucket_start(hstats, (uint64_t)(i) + 1) - 1;

				SPRINT(str, (char *)txt2, range_cw, min_range, range_cw, max_range);
				spdk_json_write_string_asis(w, str);
				SPRINT(str, (char *)txt4, hstat_cw, hstats->bucket[i]);
				spdk_json_write_string_asis(w, str);
				percent = spdk_percent_fn(hstats->bucket[i], hstats->mid.values);
				SPRINT(str, "    %3u.%02u%%\n", percent / 100, percent % 100);
				spdk_json_write_string_asis(w, str);
			}
			value_stat_show(&hstats->mid, val_stat_cw, w);
			spdk_json_write_string_asis(w, "\n");
		}

		if (hstats->hi.values) {
			if (hstats->low.values ||
			    hstats->mid.values)
				spdk_json_write_string_asis(w, "---------------------------------------------------------------\n");

			SPRINT(str, "Tallies above specified range (>=%"PRIu64")\n",
			       hstat_scaled_bucket_start(hstats, hstats->buckets));
			spdk_json_write_string_asis(w, str);
			SPRINT(str, (char *)txt3, range_cw, hstats->bucket_max, range_cw, "");
			spdk_json_write_string_asis(w, str);
			SPRINT(str, (char *)txt4, hstat_cw, hstats->overflow_bucket);
			spdk_json_write_string_asis(w, str);
			spdk_json_write_string_asis(w, "\n");
			value_stat_show(&hstats->hi, val_stat_cw, w);
			spdk_json_write_string_asis(w, "\n");
		}

		if ((hstats->low.values && hstats->mid.values) ||
		    (hstats->low.values && hstats->hi.values) ||
		    (hstats->mid.values && hstats->hi.values)) {
			spdk_json_write_string_asis(w,
						    "---------------------------------------------------------------\n\tTotal tallies\n");
			value_stat_show(&hstats->total, val_stat_cw, w);
		}
	}
}

/**
 * Return the maximum value from any of the buckets.
 */
uint64_t
hist_stat_max_value(hist_stats *hstats)
{
	uint64_t  value;
	uint32_t  i;

	value = hstats->underflow_bucket;

	if (hstats->overflow_bucket > value)
		value = hstats->overflow_bucket;

	for (i = 0; i < hstats->buckets; i++) {
		if (hstats->bucket[i] > value)
			value = hstats->bucket[i];
	}
	return value;
}

/**
 * Initialize a value statistic object of histogram.
 */
void
value_stat_init(value_stats *vstats)
{
	vstats->values      = 0;
	vstats->value_min   = HIST_LARGEST_VALUE;
	vstats->value_max   = HIST_SMALLEST_VALUE;
	vstats->value_total = 0;
}

/**
 * Add the tally value statistics.
 */
void
value_stat_tally(value_stats  *vstats, uint64_t  value)
{
	if (value < vstats->value_min)
		vstats->value_min = value;

	if (value > vstats->value_max)
		vstats->value_max = value;

	vstats->values++;
	vstats->value_total += value;
}

/**
 * Show statistics of histogram data collected
 */
void
value_stat_show(value_stats *vstats, uint32_t  column_width, struct spdk_json_write_ctx *w)
{
	static  unsigned char    txt1[] = "  %-15s %*s\n";
	static  unsigned char    txt2[] = "  %-15s %*"PRIu64"\n";
	char str[200];
	if (vstats->value_min == HIST_LARGEST_VALUE)
		SPRINT(str, (char *)txt1, "Min value", column_width, "-");
	else
		SPRINT(str, (char *)txt2, "Min value", column_width, vstats->value_min);
	spdk_json_write_string_asis(w, str);

	if (vstats->value_max == HIST_SMALLEST_VALUE)
		SPRINT(str, (char *)txt1, "Max value", column_width, "-");
	else
		SPRINT(str, (char *)txt2, "Max value", column_width, vstats->value_max);
	spdk_json_write_string_asis(w, str);

	SPRINT(str, (char *)txt2, "Total",   column_width, vstats->value_total);
	spdk_json_write_string_asis(w, str);

	SPRINT(str, (char *)txt2, "Values",  column_width, vstats->values);
	spdk_json_write_string_asis(w, str);

	SPRINT(str, (char *)txt2, "Average", column_width, vstats->values == 0 ?
	       0LL : vstats->value_total / vstats->values);
	spdk_json_write_string_asis(w, str);
}

/**
 * Return the maximum value_stats value.
 */
uint64_t
value_stat_max_value(value_stats *vstats)
{
	uint64_t value;

	value = vstats->values;
	if (vstats->value_min != HIST_LARGEST_VALUE && vstats->value_min > value)
		value = vstats->value_min;
	if (vstats->value_max > value)
		value = vstats->value_max;
	if (vstats->value_total > value)
		value = vstats->value_total;
	return value;
}

void
spdk_hist_list_ids(struct spdk_json_write_ctx *w)
{
	histogram *hg;
	char str[250];
	bool showHeader = true;
	spdk_json_write_string_asis(w,
				    " --------- --------- --------------------------- \n| Hist_id | Enabled |       Histogram name      |\n");
	spdk_json_write_string_asis(w, " --------- --------- --------------------------- \n");
	for (hg = histograms; hg; hg = hg->next) {
		SPRINT(str, "| %7"PRIu32" | %7s | %25s |\n", hg->hist_id, (hg->enabled) ? "true" : "false",
		       hg->name);
		spdk_json_write_string_asis(w, str);
		showHeader = false;
	}
	if (showHeader) {
		spdk_json_write_string_asis(w, "| ---------- No Histogram Found ----------------|\n");
	}
	spdk_json_write_string_asis(w, " --------- --------- --------------------------- \n");
}

uint32_t
column_width(uint64_t   value)
{
	uint32_t count;

	count = 0;
	while (value) {
		count++;
		value /= 10;
	}
	return count;
}
