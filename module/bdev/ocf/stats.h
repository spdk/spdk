/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#ifndef VBDEV_OCF_STATS_H
#define VBDEV_OCF_STATS_H

#include "spdk/json.h"
#include <ocf/ocf.h>

struct vbdev_ocf_stats {
	struct ocf_stats_usage usage;
	struct ocf_stats_requests reqs;
	struct ocf_stats_blocks blocks;
	struct ocf_stats_errors errors;
};

int vbdev_ocf_stats_get(ocf_cache_t cache, char *core_name, struct vbdev_ocf_stats *stats);

void vbdev_ocf_stats_write_json(struct spdk_json_write_ctx *w, struct vbdev_ocf_stats *stats);

#endif
