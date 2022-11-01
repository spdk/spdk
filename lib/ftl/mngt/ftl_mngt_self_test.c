/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "ftl_mngt.h"
#include "ftl_mngt_steps.h"
#include "ftl_internal.h"
#include "ftl_core.h"
#include "ftl_band.h"

struct ftl_validate_ctx {
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

static void
ftl_mngt_test_prepare(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_validate_ctx *cntx = ftl_mngt_get_process_ctx(mngt);

	cntx->valid_map.bit_count = dev->layout.base.total_blocks +
				    dev->layout.nvc.total_blocks;
	cntx->valid_map.buffer_size = spdk_divide_round_up(cntx->valid_map.bit_count, 8);
	cntx->valid_map.buffer_size = SPDK_ALIGN_CEIL(cntx->valid_map.buffer_size,
				      ftl_bitmap_buffer_alignment);

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

static void
ftl_mngt_test_cleanup(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_validate_ctx *cntx = ftl_mngt_get_process_ctx(mngt);

	ftl_bitmap_destroy(cntx->valid_map.bitmap);
	cntx->valid_map.bitmap = NULL;

	free(cntx->valid_map.buffer);
	cntx->valid_map.buffer = NULL;

	ftl_mngt_next_step(mngt);
}

static void
test_valid_map_pin_cb(struct spdk_ftl_dev *dev, int status,
		      struct ftl_l2p_pin_ctx *pin_ctx)
{
	struct ftl_mngt_process *mngt = pin_ctx->cb_ctx;
	struct ftl_validate_ctx *ctx = ftl_mngt_get_process_ctx(mngt);
	uint64_t lba, end;

	if (status) {
		FTL_ERRLOG(dev, "L2P pin ERROR when testing valid map\n");
		ftl_mngt_fail_step(mngt);
		return;
	}

	lba = pin_ctx->lba;
	end = pin_ctx->lba + pin_ctx->count;

	for (; lba < end; ++lba) {
		ftl_addr addr = ftl_l2p_get(dev, lba);
		bool valid;

		if (FTL_ADDR_INVALID == addr) {
			continue;
		}

		if (ftl_bitmap_get(ctx->valid_map.bitmap, addr)) {
			status = -EINVAL;
			FTL_ERRLOG(dev, "L2P mapping ERROR, double reference, "
				   "address 0x%.16"PRIX64"\n", addr);
			break;
		} else {
			ftl_bitmap_set(ctx->valid_map.bitmap, addr);
		}

		if (ftl_addr_in_nvc(dev, addr)) {
			ctx->valid_map.cache_valid_count++;
		} else {
			ctx->valid_map.base_valid_count++;
		}

		valid = ftl_bitmap_get(dev->valid_map, addr);
		if (!valid) {
			status = -EINVAL;
			FTL_ERRLOG(dev, "L2P and valid map mismatch"
				   ", LBA 0x%.16"PRIX64
				   ", address 0x%.16"PRIX64" unset\n",
				   lba, addr);
			break;
		}
	}

	ftl_l2p_unpin(dev, pin_ctx->lba, pin_ctx->count);
	pin_ctx->lba += pin_ctx->count;

	if (!status) {
		ftl_mngt_continue_step(mngt);
	} else {
		ftl_mngt_fail_step(mngt);
	}
}

static void
ftl_mngt_test_valid_map(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_l2p_pin_ctx *pin_ctx;
	struct ftl_validate_ctx *ctx = ftl_mngt_get_process_ctx(mngt);
	uint64_t left;

	pin_ctx = ftl_mngt_get_step_ctx(mngt);
	if (!pin_ctx) {
		if (ftl_mngt_alloc_step_ctx(mngt, sizeof(*pin_ctx))) {
			ftl_mngt_fail_step(mngt);
			return;
		}
		pin_ctx = ftl_mngt_get_step_ctx(mngt);
		assert(pin_ctx);

		pin_ctx->lba = 0;
		memset(ctx->valid_map.buffer, 0, ctx->valid_map.buffer_size);
	}

	left = dev->num_lbas - pin_ctx->lba;
	pin_ctx->count = spdk_min(left, 4096);

	if (pin_ctx->count) {
		ftl_l2p_pin(dev, pin_ctx->lba, pin_ctx->count,
			    test_valid_map_pin_cb, mngt, pin_ctx);
	} else {
		if (!ctx->status) {
			uint64_t valid = ctx->valid_map.base_valid_count +
					 ctx->valid_map.cache_valid_count;

			if (ftl_bitmap_count_set(dev->valid_map) != valid) {
				ctx->status = -EINVAL;
			}
		}

		/* All done */
		if (ctx->status) {
			ftl_mngt_fail_step(mngt);
		} else {
			ftl_mngt_next_step(mngt);
		}
	}
}

/*
 * Verifies the contents of L2P versus valid map. Makes sure any physical addresses in the L2P
 * have their corresponding valid bits set and that two different logical addresses don't point
 * to the same physical address.
 *
 * For debugging purposes only, directed via environment variable - whole L2P needs to be loaded in
 * and checked.
 */
static const struct ftl_mngt_process_desc desc_self_test = {
	.name = "[Test] Startup Test",
	.ctx_size = sizeof(struct ftl_validate_ctx),
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

void
ftl_mngt_self_test(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (getenv("FTL_SELF_TEST")) {
		ftl_mngt_call_process(mngt, &desc_self_test);
	} else {
		FTL_NOTICELOG(dev, "Self test skipped\n");
		ftl_mngt_next_step(mngt);
	}
}
