/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
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

#ifndef SPDK_BDEV_MERGE_INTERNAL_H
#define SPDK_BDEV_MERGE_INTERNAL_H

#include "spdk/bdev_module.h"


struct taus258_state {
	uint64_t s1, s2, s3, s4, s5;
};

struct merge_bdev_io {
	/* WaitQ entry, used only in waitq logic */
	struct spdk_bdev_io_wait_entry	waitq_entry;

	/* Original channel for this IO, used in queuing logic */
	struct spdk_io_channel		*ch;
};


struct merge_bdev_io_channel {
	/* Array of IO channels of master base bdevs, for now, there is only one */
	struct spdk_io_channel	**master_channel;
	/* Array of IO channels of slave base bdevs, for now, there is only one */
	struct spdk_io_channel	**slave_channel;
	/* Number of IO channels of master_channel, for now these is only one. */
	uint8_t			num_master_channels;
	/* Number of IO channels of slave_channel, for now these is only one. */
	uint8_t			num_slave_channels;
};



/*
 * Merge state describes the state of the merge. This merge bdev can be either in
 * configured list or configuring list
 */
enum merge_bdev_state {
	/* merge bdev is ready and is seen by upper layers */
	MERGE_BDEV_STATE_ONLINE,

	/*
	 * merge bdev is configuring, not all underlying bdevs are present.
	 * And can't be seen by upper layers.
	 */
	MERGE_BDEV_STATE_CONFIGURING,

	/*
	 * In offline state, merge bdev layer will complete all incoming commands without
	 * submitting to underlying base nvme bdevs
	 */
	MERGE_BDEV_STATE_OFFLINE,

	/* merge bdev error
	 */
	MERGE_BDEV_STATE_ERROR
};



enum merge_bdev_type {
	/* master merge bdev , all of small io request will be store in master */
	MERGE_BDEV_TYPE_MASTER,

	/* slave merge bdev , store the merged io request which come from master node */
	MERGE_BDEV_TYPE_SLAVE
};

struct merge_base_bdev_info {
	/* pointer to base spdk bdev */
	struct spdk_bdev	*bdev;

	/* pointer to base bdev descriptor opened by merge bdev */
	struct spdk_bdev_desc	*desc;

};

struct merge_bdev {
	/* merge bdev device, this will get registered in bdev layer */
	struct spdk_bdev		bdev;

	enum merge_bdev_state		state;

	struct merge_config *config;

	/* Set to true if destruct is called for this merge bdev */
	bool				destruct_called;

	/* Set to true if destroy of this merge bdev is started. */
	bool				destroy_started;

	/* cache buff */
	void *big_buff;

	struct iovec big_buff_iov;

	uint32_t big_buff_size;

	uint64_t slave_offset;

	uint64_t max_blockcnt;

	struct taus258_state max_io_rand_state;
};


struct merge_base_bdev_config {
	/* base bdev config per underlying bdev */
	char *name;

	enum merge_bdev_type type;

	struct merge_base_bdev_info base_bdev_info;

	/* strip size */
	uint32_t			strip_size;

	/* Points to already created raid bdev  */
	struct merge_bdev *merge_bdev;

	TAILQ_ENTRY(merge_base_bdev_config) link;
};


struct merge_config {
	char *name;

	uint32_t			master_strip_size;

	uint32_t			slave_strip_size;

	struct merge_bdev *merge_bdev;

	TAILQ_HEAD(, merge_base_bdev_config) merge_base_bdev_config_head;

	/* total merge bdev  from config file */
	uint8_t total_merge_slave_bdev;
};


typedef void (*merge_bdev_destruct_cb)(void *cb_ctx, int rc);

static inline uint64_t __seed(uint64_t x, uint64_t m)
{
	return (x < m) ? x + m : x;
}

static inline uint64_t __rand64(struct taus258_state *state)
{
	uint64_t xval;

	xval = ((state->s1 <<  1) ^ state->s1) >> 53;
	state->s1 = ((state->s1 & 18446744073709551614ULL) << 10) ^ xval;

	xval = ((state->s2 << 24) ^ state->s2) >> 50;
	state->s2 = ((state->s2 & 18446744073709551104ULL) <<  5) ^ xval;

	xval = ((state->s3 <<  3) ^ state->s3) >> 23;
	state->s3 = ((state->s3 & 18446744073709547520ULL) << 29) ^ xval;

	xval = ((state->s4 <<  5) ^ state->s4) >> 24;
	state->s4 = ((state->s4 & 18446744073709420544ULL) << 23) ^ xval;

	xval = ((state->s5 <<  3) ^ state->s5) >> 33;
	state->s5 = ((state->s5 & 18446744073701163008ULL) <<  8) ^ xval;

	return (state->s1 ^ state->s2 ^ state->s3 ^ state->s4 ^ state->s5);
}


static void __init_rand64(struct taus258_state *state, uint64_t seed)
{
	int cranks = 6;

#define LCG64(x, seed)  ((x) * 6906969069ULL ^ (seed))

	state->s1 = __seed(LCG64((2 ^ 31) + (2 ^ 17) + (2 ^ 7), seed), 1);
	state->s2 = __seed(LCG64(state->s1, seed), 7);
	state->s3 = __seed(LCG64(state->s2, seed), 15);
	state->s4 = __seed(LCG64(state->s3, seed), 33);
	state->s5 = __seed(LCG64(state->s4, seed), 49);

	while (cranks--) {
		__rand64(state);
	}
}

#endif
