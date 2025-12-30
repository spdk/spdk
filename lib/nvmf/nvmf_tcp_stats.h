/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#ifndef __NVMF_TCP_STATS_H__
#define __NVMF_TCP_STATS_H__

#define SPDK_CONFIG_NVMF_TCP_IO_STATS

struct tcp_req_stats {
	/* timestamps */
	uint64_t ts_cmd_recv;	/* CMD capsule received */

	uint64_t ts_bdev_start;	/* bdev submit */
	uint64_t ts_bdev_end;	/* bdev completion */

	/* WRITE path (per XFER scratch) */
	uint64_t ts_r2t_sent ;

	uint64_t ts_qos_start;
	/*READ path*/
	uint64_t ts_net_start;

	/* accumulators */
	uint64_t write_net_latency;
	uint64_t qos_latency;

	uint32_t size;
	bool waiting_for_data;
	bool is_write_io;
};

// statistics are collected per QP:
#define IO_SIZE_BUCKETS 12 //2KB, 4, 8, 16, 32, 64, 128, 256, 512, 1M, 2, 4
enum io_dir {
	IO_READ = 0,
	IO_WRITE = 1,
	IO_DIR_MAX
};

struct latency_stats {
	uint64_t min;
	uint64_t max;
	double   mean;
	double   m2;
	bool min_set;
};

struct io_latency_group {
	uint64_t io_count;  // num ios in bucket
	struct latency_stats total;
	struct latency_stats bdev;
	struct latency_stats net;
	struct latency_stats qos;
};

struct io_size_bucket {
	struct io_latency_group dir[IO_DIR_MAX];
};

struct qp_io_stats {
	uint64_t total_num_ios;  // number IOS in QP for all buckets
	struct io_size_bucket buckets[IO_SIZE_BUCKETS];
};

#ifdef SPDK_CONFIG_NVMF_TCP_IO_STATS
#ifdef __SPDK_NVMF_TCP_C__
// below inline functions definitions

static inline void
nvmf_tcp_req_stats_init(struct tcp_req_stats *stats) {
	//req->stats = spdk_mempool_get(g_tcp_req_stats_pool);
	memset(stats, 0, sizeof(*stats));
}

static inline void
nvmf_tcp_stats_write_io_qos_start(struct tcp_req_stats *stats) {
	if (stats->ts_cmd_recv != 0) {
		stats->ts_qos_start = spdk_get_ticks();
	}
}

static inline void
nvmf_tcp_stats_read_io_bdev_complete(struct tcp_req_stats *stats) {
	if (stats->ts_cmd_recv != 0 && stats->is_write_io == false) {
		stats->ts_bdev_end = spdk_get_ticks();
	}
}

static inline void
nvmf_tcp_req_stats_cmd_start(struct tcp_req_stats *stats, uint8_t xfer) {
	if (stats->ts_cmd_recv == 0) {
		if (xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
			stats->is_write_io = true;
			stats->ts_cmd_recv = spdk_get_ticks();
		} else if (xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
			stats->is_write_io = false;
			stats->ts_cmd_recv = spdk_get_ticks();
		}
	}
}

static inline void
nvmf_tcp_stats_r2t_sent(struct tcp_req_stats *stats) {
	//for write IO transport sends XFER_RDY to the initiator
	if (stats->ts_cmd_recv != 0) {
		stats->ts_r2t_sent = spdk_get_ticks();
		stats->waiting_for_data = true;
	}
}

static inline void
nvmf_tcp_stats_host_write_data_rcvd(struct tcp_req_stats *stats) {
	if (stats->waiting_for_data) {
		stats->write_net_latency +=
			(spdk_get_ticks() - stats->ts_r2t_sent);
		 	 stats->waiting_for_data = false;
	}
	else {
		//TODO some log and assert
		//SPDK_WARNLOG("wrong xfer for %p \n",tcp_req);
	}
}

static inline void
nvmf_tcp_stats_start_read_io_netw_latency(struct tcp_req_stats *stats){
	if ((stats->ts_cmd_recv != 0) &&(stats->ts_net_start == 0)) {
		stats->ts_net_start = spdk_get_ticks();
	}
}

static inline void
nvmf_tcp_stats_complete_qos_latency(struct tcp_req_stats *stats, uint32_t size){
	if ((stats->ts_cmd_recv != 0) && (stats->ts_bdev_start  == 0)) {
		stats->ts_bdev_start = spdk_get_ticks(); // same point for write and read IOs
		stats->size = size;
		if (!stats->is_write_io ) {
			stats->qos_latency =  spdk_get_ticks() - stats->ts_cmd_recv;
		} else { // write IO
			if (stats->ts_qos_start == 0) // in-capsula data
				//write_net_latency = 0 -> all data came in capsula
				stats->qos_latency =  spdk_get_ticks() - stats->ts_cmd_recv;
			else
				stats->qos_latency =  spdk_get_ticks() - stats->ts_qos_start;
		}
	}
}


#endif // __SPDK_NVMF_TCP_C__
//Below defined other non-inline functions
void
nvmf_tcp_req_stats_finalize(struct qp_io_stats *qp_stats,
	struct tcp_req_stats *stats, bool status);

void
emit_qp_stats(struct spdk_json_write_ctx *w,
		const struct qp_io_stats *stats);

void accumulate_stats(struct qp_io_stats *accum_stats,
		const struct qp_io_stats *qp_stats) ;
void
nvmf_tcp_stats_reset_qp(struct qp_io_stats *qp_stats);

#else
	#define nvmf_tcp_req_stats_init(stats)
	#define nvmf_tcp_req_stats_cmd_start(stats, xfer)	do {} while (0)
	#define nvmf_tcp_stats_r2t_sent(stats)			do {} while (0)
	#define nvmf_tcp_stats_host_write_data_rcvd(stats)		do {} while (0)
	#define nvmf_tcp_stats_write_io_qos_start(stats)	do {} while (0)
	#define nvmf_tcp_stats_read_io_bdev_complete(stats)	do {} while (0)
	#define nvmf_tcp_stats_start_read_io_netw_latency(stats)	 do {} while (0)
	#define nvmf_tcp_stats_complete_qos_latency(stats, size)	 do {} while (0)
	#define nvmf_tcp_req_stats_finalize(qp_stats, req_stats, status) do {} while (0)
	#define nvmf_tcp_stats_reset_qp(stats)	 do {} while (0)
	#define emit_qp_stats(json_ctx, stats)   do {} while (0)
	#define	accumulate_stats( astats, qstats)   do {} while (0)
#endif

#endif
