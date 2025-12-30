/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019, 2020 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */
#define __SPDK_NVMF_TCP_STATS_C__
#include "spdk/stdinc.h"
#include "spdk/endian.h"
#include "spdk/assert.h"
#include "spdk/thread.h"
//#include "spdk/nvmf_transport.h"
#include "spdk/string.h"
#include "spdk/trace.h"
#include "spdk/util.h"
#include "spdk/log.h"
#include "spdk/sock.h"

#include "spdk_internal/assert.h"
#include "nvmf_tcp_stats.h"


uint32_t bucket_2_size[IO_SIZE_BUCKETS] = {2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};

#ifdef SPDK_CONFIG_NVMF_TCP_IO_STATS

void latency_update(struct latency_stats * stats, uint64_t latency);


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

void nvmf_tcp_stats_reset_qp(struct qp_io_stats *qp_stats)
{
	memset(qp_stats, 0, sizeof(*qp_stats));
}

void latency_update(struct latency_stats * stats, uint64_t latency) {
	if (spdk_unlikely(!stats->min_set)) {
		stats->min = latency;
		stats->min_set = true;
	}
	else {
		if (stats->min > latency) stats->min = latency;
	}
	if (stats->max < latency) stats->max = latency;
	stats->mean += latency;
}

void
nvmf_tcp_req_stats_finalize(struct qp_io_stats *qp_stats,
		struct tcp_req_stats *stats, bool status) {
	if (stats->ts_cmd_recv != 0) {
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
		// just for debug - would be removed
		SPDK_WARNLOG("io completed for %p , write %d, size %d, total-lat %llu, bdev-lat %llu , net-lat %llu, qos-lat %llu\n",
				stats, stats->is_write_io, stats->size,
				total * 1000000ULL / ticks_hz,
				bdev * 1000000ULL / ticks_hz,
				net * 1000000ULL / ticks_hz,
				stats->qos_latency * 1000000ULL /ticks_hz
		);
		//store qp_stats
		int bucket = io_size_to_bucket(stats->size);
		qp_stats->total_num_ios ++;
		struct io_latency_group *group = &qp_stats->buckets[bucket].dir[stats->is_write_io];
		group->io_count ++;
		latency_update(&group->total, total);
		latency_update(&group->bdev, bdev);
		latency_update(&group->net, net);
		latency_update(&group->qos, stats->qos_latency );
	}
}
// RPC  handlers for IO stats

static void accumulate_latency(struct latency_stats * accum_latency_stats,
		const struct latency_stats *qp_latency_stats) {
	accum_latency_stats->mean += qp_latency_stats->mean;
	if (accum_latency_stats->max < qp_latency_stats->max) {
		accum_latency_stats->max = qp_latency_stats->max;
	}
	if (accum_latency_stats->min_set == false) {
		accum_latency_stats->min = qp_latency_stats->min;
		accum_latency_stats->min_set = true;
	} else {
		if (accum_latency_stats->min > qp_latency_stats->min) {
			accum_latency_stats->min = qp_latency_stats->min;
		}
	}
}

void accumulate_stats(struct qp_io_stats *accum_stats,
		const struct qp_io_stats *qp_stats) {
	int i,j;
	if (qp_stats->total_num_ios < 10) { // dont  calculate qp with no IOs
		return;
	}
	accum_stats->total_num_ios += qp_stats->total_num_ios;
	for (i = 0; i < IO_SIZE_BUCKETS; i++) {
		for (j = 0; j<IO_DIR_MAX; j++) {
			// Do not take into account buckets with no IOs - it could ruin statistics
			if (qp_stats->buckets[i].dir[j].io_count) {
				accum_stats->buckets[i].dir[j].io_count +=  qp_stats->buckets[i].dir[j].io_count;
				accumulate_latency(&accum_stats->buckets[i].dir[j].total, &qp_stats->buckets[i].dir[j].total);
				accumulate_latency(&accum_stats->buckets[i].dir[j].bdev,  &qp_stats->buckets[i].dir[j].bdev);
				accumulate_latency(&accum_stats->buckets[i].dir[j].net,   &qp_stats->buckets[i].dir[j].net);
				accumulate_latency(&accum_stats->buckets[i].dir[j].qos,   &qp_stats->buckets[i].dir[j].qos);
			}
		}
	}
}

static void
emit_latency_stats(struct spdk_json_write_ctx *w,
		   const struct latency_stats *s, uint64_t ticks_hz, uint64_t io_cnt)
{
	spdk_json_write_named_uint64(w, "min", s->min * 1000000ULL/ticks_hz);
	spdk_json_write_named_uint64(w, "max", s->max * 1000000ULL/ticks_hz);
	spdk_json_write_named_uint64(w, "mean",
			(uint64_t)((s->mean/io_cnt) * 1000000ULL/ticks_hz));
}

static void
emit_latency_group(struct spdk_json_write_ctx *w,
		   const struct io_latency_group *g, uint64_t ticks_hz)
{
	spdk_json_write_named_uint64(w, "io_count", g->io_count);

	spdk_json_write_named_object_begin(w, "latency");

	spdk_json_write_named_object_begin(w, "total");
	emit_latency_stats(w, &g->total, ticks_hz, g->io_count );
	spdk_json_write_object_end(w);

	spdk_json_write_named_object_begin(w, "bdev");
	emit_latency_stats(w, &g->bdev, ticks_hz, g->io_count);
	spdk_json_write_object_end(w);

	spdk_json_write_named_object_begin(w, "net");
	emit_latency_stats(w, &g->net, ticks_hz, g->io_count);
	spdk_json_write_object_end(w);

	spdk_json_write_named_object_begin(w, "qos");
	emit_latency_stats(w, &g->qos, ticks_hz, g->io_count);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w); /* latency */
}

void
emit_qp_stats(struct spdk_json_write_ctx *w,
		const struct qp_io_stats *stats)
{
	uint32_t i;
	uint64_t ticks_hz = spdk_get_ticks_hz();
	spdk_json_write_object_begin(w); //1
	spdk_json_write_named_uint64(w, "total_num_ios",
				stats->total_num_ios);
	SPDK_NOTICELOG(" Dumping qp stats: total ios  %lu\n", stats->total_num_ios);
	spdk_json_write_named_array_begin(w, "buckets"); //2

	for (i = 0; i < IO_SIZE_BUCKETS; i++) {
		const struct io_size_bucket *b = &stats->buckets[i];
		/* Skip empty buckets */
		if (b->dir[IO_READ].io_count == 0 &&
			b->dir[IO_WRITE].io_count == 0) {
			continue;
		}
		spdk_json_write_object_begin(w); //3
		spdk_json_write_named_uint32(w, "bucket-size (KB)", bucket_2_size[i]);//TODO
		SPDK_NOTICELOG(" Dumping qp stats: bucket  %d r ios %lu  w ios %lu \n", i, b->dir[IO_READ].io_count, b->dir[IO_WRITE].io_count );
		if (b->dir[IO_READ].io_count) {
			spdk_json_write_named_object_begin(w, "read");//4
			emit_latency_group(w, &b->dir[IO_READ], ticks_hz);
			spdk_json_write_object_end(w);  //4
		}
		if (b->dir[IO_WRITE].io_count) {
			spdk_json_write_named_object_begin(w, "write");//4
			emit_latency_group(w, &b->dir[IO_WRITE], ticks_hz);
			spdk_json_write_object_end(w);//4
		}

		spdk_json_write_object_end(w); //3
		SPDK_NOTICELOG(" close bucket %d\n", i);
	}
	spdk_json_write_array_end(w);//2
	spdk_json_write_object_end(w);// 1
}

#endif
