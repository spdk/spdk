/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#include "ctx.h"
#include "stats.h"

int
vbdev_ocf_stats_get(ocf_cache_t cache, char *core_name, struct vbdev_ocf_stats *stats)
{
	int status;
	ocf_core_t core;

	status = ocf_core_get_by_name(cache, core_name, strlen(core_name), &core);
	if (status) {
		return status;
	}

	return ocf_stats_collect_core(core, &stats->usage, &stats->reqs, &stats->blocks, &stats->errors);
}

#define WJSON_STAT(w, stats, group, field, units) \
	spdk_json_write_named_object_begin(w, #field); \
	spdk_json_write_named_uint64(w, "count", stats->group.field.value); \
	spdk_json_write_named_string_fmt(w, "percentage", "%lu.%lu", \
		stats->group.field.fraction / 100, stats->group.field.fraction % 100); \
	spdk_json_write_named_string(w, "units", units); \
	spdk_json_write_object_end(w);

void
vbdev_ocf_stats_write_json(struct spdk_json_write_ctx *w, struct vbdev_ocf_stats *stats)
{
	spdk_json_write_object_begin(w);

	spdk_json_write_named_object_begin(w, "usage");
	WJSON_STAT(w, stats, usage, occupancy, "4KiB blocks");
	WJSON_STAT(w, stats, usage, free, "4KiB blocks");
	WJSON_STAT(w, stats, usage, clean, "4KiB blocks");
	WJSON_STAT(w, stats, usage, dirty, "4KiB blocks");
	spdk_json_write_object_end(w);

	spdk_json_write_named_object_begin(w, "requests");
	WJSON_STAT(w, stats, reqs, rd_hits, "Requests");
	WJSON_STAT(w, stats, reqs, rd_partial_misses, "Requests");
	WJSON_STAT(w, stats, reqs, rd_full_misses, "Requests");
	WJSON_STAT(w, stats, reqs, rd_total, "Requests");
	WJSON_STAT(w, stats, reqs, wr_hits, "Requests");
	WJSON_STAT(w, stats, reqs, wr_partial_misses, "Requests");
	WJSON_STAT(w, stats, reqs, wr_full_misses, "Requests");
	WJSON_STAT(w, stats, reqs, wr_total, "Requests");
	WJSON_STAT(w, stats, reqs, rd_pt, "Requests");
	WJSON_STAT(w, stats, reqs, wr_pt, "Requests");
	WJSON_STAT(w, stats, reqs, serviced, "Requests");
	WJSON_STAT(w, stats, reqs, total, "Requests");
	spdk_json_write_object_end(w);

	spdk_json_write_named_object_begin(w, "blocks");
	WJSON_STAT(w, stats, blocks, core_volume_rd, "4KiB blocks");
	WJSON_STAT(w, stats, blocks, core_volume_wr, "4KiB blocks");
	WJSON_STAT(w, stats, blocks, core_volume_total, "4KiB blocks");
	WJSON_STAT(w, stats, blocks, cache_volume_rd, "4KiB blocks");
	WJSON_STAT(w, stats, blocks, cache_volume_wr, "4KiB blocks");
	WJSON_STAT(w, stats, blocks, cache_volume_total, "4KiB blocks");
	WJSON_STAT(w, stats, blocks, volume_rd, "4KiB blocks");
	WJSON_STAT(w, stats, blocks, volume_wr, "4KiB blocks");
	WJSON_STAT(w, stats, blocks, volume_total, "4KiB blocks");
	spdk_json_write_object_end(w);

	spdk_json_write_named_object_begin(w, "errors");
	WJSON_STAT(w, stats, errors, core_volume_rd, "Requests");
	WJSON_STAT(w, stats, errors, core_volume_wr, "Requests");
	WJSON_STAT(w, stats, errors, core_volume_total, "Requests");
	WJSON_STAT(w, stats, errors, cache_volume_rd, "Requests");
	WJSON_STAT(w, stats, errors, cache_volume_wr, "Requests");
	WJSON_STAT(w, stats, errors, cache_volume_total, "Requests");
	WJSON_STAT(w, stats, errors, total, "Requests");
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}
