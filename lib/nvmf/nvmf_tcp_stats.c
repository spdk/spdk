/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019, 2020 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */
#define __SPDK_NVMF_TCP_STATS_C__
#include "spdk/config.h"
#include "spdk/stdinc.h"
#include "spdk/endian.h"
#include "spdk/assert.h"
#include "spdk/thread.h"
#include "spdk/string.h"
#include "spdk/trace.h"
#include "spdk/util.h"
#include "spdk/log.h"

#include "spdk_internal/assert.h"
#include "nvmf_tcp_stats.h"


void latency_update(struct latency_stats *stats, uint64_t latency);

void
nvmf_stats_reset_qp(struct qp_io_stats *qp_stats)
{
	memset(qp_stats, 0, sizeof(*qp_stats));
}

static inline int
io_size_to_bucket(uint32_t size)
{
	int bucket;
	/* clamp to 2K minimum */
	if (size < 2048) {
		size = 2048;
	}
	bucket = spdk_u32log2(size) - 11; /* log2(2048)=11 */
	if (bucket < 0) {
		bucket = 0;
	} else if (bucket >= IO_SIZE_BUCKETS) {
		bucket = IO_SIZE_BUCKETS - 1;
	}
	return bucket;
}

void
latency_update(struct latency_stats *stats, uint64_t latency)
{
	if (spdk_unlikely(!stats->min_set)) {
		stats->min = latency;
		stats->min_set = true;
	} else {
		if (stats->min > latency) { stats->min = latency; }
	}
	if (stats->max < latency) { stats->max = latency; }
	stats->mean += latency;
}

void
nvmf_tcp_req_stats_finalize(struct qp_io_stats *qp_stats,
			    struct tcp_req_stats *stats, bool status)
{
	if (stats && qp_stats && stats->ts_cmd_recv != 0) {
		if (!status) {
			return;
		}
		uint64_t rsp_sent;
		rsp_sent		 = spdk_get_ticks();
		if (stats->is_write_io) {
			stats->ts_bdev_end = rsp_sent;
		} else if (stats->ts_net_start == 0) {
			return;// read IO without network
		}
		uint64_t ticks_hz = spdk_get_ticks_hz();
		uint64_t total = rsp_sent - stats->ts_cmd_recv;
		uint64_t bdev  = stats->ts_bdev_end - stats->ts_bdev_start;
		uint64_t net   = stats->is_write_io ? stats->write_net_latency
				 : (rsp_sent - stats->ts_net_start);
		/* SPDK_WARNLOG("io completed for %p , write %d, size %d, total-lat %llu, bdev-lat %llu , net-lat %llu, qos-lat %llu\n",
			     stats, stats->is_write_io, stats->size,
			     total * 1000000ULL / ticks_hz,
			     bdev * 1000000ULL / ticks_hz,
			     net * 1000000ULL / ticks_hz,
			     stats->qos_latency * 1000000ULL / ticks_hz
			    );
        */
		/* store qp_stats */
		int bucket = io_size_to_bucket(stats->size);
		qp_stats->total_num_ios ++;
		struct io_latency_group *group = &qp_stats->buckets[bucket].dir[stats->is_write_io];
		group->io_count ++;
		latency_update(&group->total, total);
		latency_update(&group->bdev, bdev);
		latency_update(&group->net, net);
		latency_update(&group->qos, stats->qos_latency);
	}
}
