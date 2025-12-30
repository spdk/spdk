/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#ifndef __NVMF_CNTRL_STATS_H__
#define __NVMF_CNTRL_STATS_H__

/* statistics are collected per QP: */
#define IO_SIZE_BUCKETS 12 /* 2KB, 4, 8, 16, 32, 64, 128, 256, 512, 1M, 2, 4 */
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
	uint64_t io_count;  /* num ios in bucket */
	struct latency_stats total;
	struct latency_stats bdev;
	struct latency_stats net;
	struct latency_stats qos;
};

struct io_size_bucket {
	struct io_latency_group dir[IO_DIR_MAX];
};

struct qp_io_stats {
	uint64_t total_num_ios;  /* number IOS in QP for all buckets */
	struct io_size_bucket buckets[IO_SIZE_BUCKETS];
};
#endif
