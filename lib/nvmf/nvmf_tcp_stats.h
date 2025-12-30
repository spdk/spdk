/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#ifndef __NVMF_TCP_STATS_H__
#define __NVMF_TCP_STATS_H__

/* #define SPDK_CONFIG_NVMF_TCP_IO_STATS */

#include "nvmf_controller_stats.h"

struct tcp_req_stats {
	/* timestamps */
	uint64_t ts_cmd_recv;	/* CMD capsule received */

	uint64_t ts_bdev_start;	/* bdev submit */
	uint64_t ts_bdev_end;	/* bdev completion */

	/* WRITE path (per XFER scratch) */
	uint64_t ts_r2t_sent ;

	uint64_t ts_qos_start;
	/* READ path */
	uint64_t ts_net_start;

	/* accumulators */
	uint64_t write_net_latency;
	uint64_t qos_latency;

	uint32_t size;
	bool waiting_for_data;
	bool is_write_io;
};

#ifdef __SPDK_NVMF_TCP_C__
/* below inline functions definitions */

static inline void
nvmf_tcp_stats_write_io_qos_start(struct tcp_req_stats *stats)
{
	if (stats && stats->ts_cmd_recv != 0) {
		stats->ts_qos_start = spdk_get_ticks();
	}
}

static inline void
nvmf_tcp_stats_read_io_bdev_complete(struct tcp_req_stats *stats)
{
	if (stats && stats->ts_cmd_recv != 0 && stats->is_write_io == false) {
		stats->ts_bdev_end = spdk_get_ticks();
	}
}

static inline void
nvmf_tcp_req_stats_cmd_start(struct tcp_req_stats *stats, uint8_t xfer)
{
	if (stats && stats->ts_cmd_recv == 0) {
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
nvmf_tcp_stats_r2t_sent(struct tcp_req_stats *stats)
{
	/* for write IO transport sends XFER_RDY to the initiator */
	if (stats && stats->ts_cmd_recv != 0) {
		stats->ts_r2t_sent = spdk_get_ticks();
		stats->waiting_for_data = true;
	}
}

static inline void
nvmf_tcp_stats_host_write_data_rcvd(struct tcp_req_stats *stats)
{
	if (stats && stats->waiting_for_data) {
		stats->write_net_latency +=
			(spdk_get_ticks() - stats->ts_r2t_sent);
		stats->waiting_for_data = false;
     }
}

static inline void
nvmf_tcp_stats_start_read_io_netw_latency(struct tcp_req_stats *stats)
{
	if (stats && (stats->ts_cmd_recv != 0) && (stats->ts_net_start == 0)) {
		stats->ts_net_start = spdk_get_ticks();
	}
}

static inline void
nvmf_tcp_stats_complete_qos_latency(struct tcp_req_stats *stats, uint32_t size)
{
	if (stats && (stats->ts_cmd_recv != 0) && (stats->ts_bdev_start  == 0)) {
		stats->ts_bdev_start = spdk_get_ticks(); /* the same point for write and read IOs */
		stats->size = size;
		if (!stats->is_write_io) {
			stats->qos_latency =  spdk_get_ticks() - stats->ts_cmd_recv;
		} else { /* write IO */
			if (stats->ts_qos_start == 0)
				/* write_net_latency = 0 -> all data came in capsula */
			{
				stats->qos_latency =  spdk_get_ticks() - stats->ts_cmd_recv;
			} else {
				stats->qos_latency =  spdk_get_ticks() - stats->ts_qos_start;
			}
		}
	}
}

#endif /*  __SPDK_NVMF_TCP_C__ */
/* Below defined other non-inline functions */
void nvmf_tcp_req_stats_finalize(struct qp_io_stats *qp_stats,
				 struct tcp_req_stats *stats, bool status);

void nvmf_stats_reset_qp(struct qp_io_stats *qp_stats);

#endif
