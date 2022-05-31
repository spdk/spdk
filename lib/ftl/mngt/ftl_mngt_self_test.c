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
#include "spdk/bdev_zone.h"

#include "ftl_mngt_zone.h"
#include "ftl_mngt.h"
#include "ftl_mngt_steps.h"
#include "ftl_internal.h"
#include "ftl_core.h"
#include "ftl_band.h"

struct ftl_validate_context {
	struct {
		struct ftl_bitmap *bitmap;
		void *buffer;
		uint64_t buffer_size;
		uint64_t bit_count;
		uint64_t base_valid_count;
		uint64_t cache_valid_count;
	} valid_map;

	int status;
};

static void ftl_mngt_test_prepare(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	struct ftl_validate_context *cntx = ftl_mngt_get_process_cntx(mngt);

	cntx->valid_map.bit_count = dev->layout.btm.total_blocks +
				    dev->layout.nvc.total_blocks;
	cntx->valid_map.buffer_size = spdk_divide_round_up(
					      cntx->valid_map.bit_count, 8);
	cntx->valid_map.buffer_size /= ftl_bitmap_buffer_alignment;
	cntx->valid_map.buffer_size *= ftl_bitmap_buffer_alignment;

	cntx->valid_map.buffer = calloc(cntx->valid_map.buffer_size, 1);
	if (!cntx->valid_map.buffer) {
		ftl_mngt_fail_step(mngt);
		return;
	}

	cntx->valid_map.bitmap = ftl_bitmap_create(cntx->valid_map.buffer,
				 cntx->valid_map.buffer_size);
	if (!cntx->valid_map.bitmap) {
		ftl_mngt_fail_step(mngt);
		return;
	}

	ftl_mngt_next_step(mngt);
}

static void ftl_mngt_test_cleanup(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	struct ftl_validate_context *cntx = ftl_mngt_get_process_cntx(mngt);

	ftl_bitmap_destroy(cntx->valid_map.bitmap);
	cntx->valid_map.bitmap = NULL;

	free(cntx->valid_map.buffer);
	cntx->valid_map.buffer = NULL;

	ftl_mngt_next_step(mngt);
}

static void test_valid_map_pin_cb(struct spdk_ftl_dev *dev, int status,
				  struct ftl_l2p_pin_ctx *pin_cntx)
{
	struct ftl_mngt *mngt = pin_cntx->cb_ctx;
	struct ftl_validate_context *cntx = ftl_mngt_get_process_cntx(mngt);

	if (status) {
		FTL_ERRLOG(dev, "L2P pin ERROR when testing valid map\n");
		ftl_mngt_fail_step(mngt);
		return;
	}

	uint64_t lba = pin_cntx->lba;
	uint64_t end = pin_cntx->lba + pin_cntx->count;

	for (; lba < end; ++lba) {
		ftl_addr addr = ftl_l2p_get(dev, lba);

		if (FTL_ADDR_INVALID == addr) {
			continue;
		}

		if (ftl_bitmap_get(cntx->valid_map.bitmap, addr)) {
			status = -EINVAL;
			FTL_ERRLOG(dev, "L2P mapping ERROR, double reference, "
				   "address 0x%.16"PRIX64"\n", addr);
			break;
		} else {
			ftl_bitmap_set(cntx->valid_map.bitmap, addr);
		}

		if (ftl_addr_cached(dev, addr)) {
			cntx->valid_map.cache_valid_count++;
		} else {
			cntx->valid_map.base_valid_count++;
		}

		bool valid = ftl_bitmap_get(dev->valid_map, addr);
		if (!valid) {
			status = -EINVAL;
			FTL_ERRLOG(dev, "L2P and valid map mismatch"
				   ", LAB 0x%.16"PRIX64
				   ", address 0x%.16"PRIX64" unset\n",
				   lba, addr);
			break;
		}
	}

	ftl_l2p_unpin(dev, pin_cntx->lba, pin_cntx->count);
	pin_cntx->lba += pin_cntx->count;

	if (!status) {
		ftl_mngt_continue_step(mngt);
	} else {
		ftl_mngt_fail_step(mngt);
	}
}

static void ftl_mngt_test_valid_map(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	struct ftl_l2p_pin_ctx *pin_cntx;
	struct ftl_validate_context *cntx = ftl_mngt_get_process_cntx(mngt);

	pin_cntx = ftl_mngt_get_step_cntx(mngt);
	if (!pin_cntx) {
		if (ftl_mngt_alloc_step_cntx(mngt, sizeof(*pin_cntx))) {
			ftl_mngt_fail_step(mngt);
			return;
		}
		pin_cntx = ftl_mngt_get_step_cntx(mngt);
		assert(pin_cntx);

		pin_cntx->lba = 0;
		memset(cntx->valid_map.buffer, 0, cntx->valid_map.buffer_size);
	}

	uint64_t left = dev->num_lbas - pin_cntx->lba;
	pin_cntx->count = spdk_min(left, 4096);

	if (pin_cntx->count) {
		ftl_l2p_pin(dev, pin_cntx->lba, pin_cntx->count,
			    test_valid_map_pin_cb, mngt, pin_cntx);
	} else {
		if (!cntx->status) {
			uint64_t valid = cntx->valid_map.base_valid_count +
					 cntx->valid_map.cache_valid_count;

			if (ftl_bitmap_count_set(dev->valid_map) != valid) {
				cntx->status = -EINVAL;
			}
		}

		/* All done */
		if (cntx->status) {
			ftl_mngt_fail_step(mngt);
		} else {
			ftl_mngt_next_step(mngt);
		}
	}
}

static const struct ftl_mngt_process_desc desc_self_test = {
	.name = "[Test] Startup Test",
	.arg_size = sizeof(struct ftl_validate_context),
	.steps = {
		{
			.name = "[TEST] Initialize selftest",

			.action = ftl_mngt_test_prepare,
			.cleanup = ftl_mngt_test_cleanup
		},
		{
			.name = "[TEST] Validate map and L2P consistency",
			.action = ftl_mngt_test_valid_map
		},
		{
			.name = "[TEST] Deinitialize cleanup",
			.action = ftl_mngt_test_cleanup
		},
		{}
	}
};

void ftl_mngt_self_test(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	if (getenv("FTL_SELF_TEST")) {
		ftl_mngt_call(mngt, &desc_self_test);
	} else {
		FTL_NOTICELOG(dev, "Self test skipped\n");
		ftl_mngt_next_step(mngt);
	}
}
