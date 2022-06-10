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

#include "ftl_l2p.h"
#include "ftl_band.h"
#include "ftl_nv_cache.h"
#include "ftl_l2p_flat.h"

#define FTL_L2P_OP(name)	ftl_l2p_flat_ ## name


int ftl_l2p_init(struct spdk_ftl_dev *dev)
{
	TAILQ_INIT(&dev->l2p_deferred_pins);
	return FTL_L2P_OP(init)(dev);
}

void ftl_l2p_deinit(struct spdk_ftl_dev *dev)
{
	FTL_L2P_OP(deinit)(dev);
}

static inline void
ftl_l2p_pin_ctx_init(struct ftl_l2p_pin_ctx *pin_ctx, uint64_t lba, uint64_t count,
		     ftl_l2p_pin_cb cb, void *cb_ctx)
{
	pin_ctx->lba = lba;
	pin_ctx->count = count;
	pin_ctx->cb = cb;
	pin_ctx->cb_ctx = cb_ctx;
}

void
ftl_l2p_pin(struct spdk_ftl_dev *dev, uint64_t lba, uint64_t count, ftl_l2p_pin_cb cb, void *cb_ctx,
	    struct ftl_l2p_pin_ctx *pin_ctx)
{
	ftl_l2p_pin_ctx_init(pin_ctx, lba, count, cb, cb_ctx);
	FTL_L2P_OP(pin)(dev, pin_ctx);
}

void
ftl_l2p_unpin(struct spdk_ftl_dev *dev, uint64_t lba, uint64_t count)
{
	FTL_L2P_OP(unpin)(dev, lba, count);
}

void
ftl_l2p_pin_skip(struct spdk_ftl_dev *dev, ftl_l2p_pin_cb cb, void *cb_ctx,
		 struct ftl_l2p_pin_ctx *pin_ctx)
{
	ftl_l2p_pin_ctx_init(pin_ctx, FTL_LBA_INVALID, 0, cb, cb_ctx);
	cb(dev, 0, pin_ctx);
}

void
ftl_l2p_set(struct spdk_ftl_dev *dev, uint64_t lba, ftl_addr addr)
{
	FTL_L2P_OP(set)(dev, lba, addr);
}

ftl_addr
ftl_l2p_get(struct spdk_ftl_dev *dev, uint64_t lba)
{
	return FTL_L2P_OP(get)(dev, lba);
}

void
ftl_l2p_clear(struct spdk_ftl_dev *dev, ftl_l2p_cb cb, void *cb_ctx)
{
	FTL_L2P_OP(clear)(dev, cb, cb_ctx);
}

void
ftl_l2p_process(struct spdk_ftl_dev *dev)
{
	struct ftl_l2p_pin_ctx *pin_ctx;

	pin_ctx = TAILQ_FIRST(&dev->l2p_deferred_pins);
	if (pin_ctx) {
		TAILQ_REMOVE(&dev->l2p_deferred_pins, pin_ctx, link);
		FTL_L2P_OP(pin)(dev, pin_ctx);
	}

	FTL_L2P_OP(process)(dev);
}

bool
ftl_l2p_is_halted(struct spdk_ftl_dev *dev)
{
	if (!TAILQ_EMPTY(&dev->l2p_deferred_pins)) {
		return false;
	}

	return FTL_L2P_OP(is_halted)(dev);
}

void
ftl_l2p_halt(struct spdk_ftl_dev *dev)
{
	return FTL_L2P_OP(halt)(dev);
}

void
ftl_l2p_update_cached(struct spdk_ftl_dev *dev, uint64_t lba, ftl_addr new_addr, ftl_addr prev_addr)
{
	struct ftl_nv_cache_chunk *current_chunk, *new_chunk;
	ftl_addr current_addr;

	assert(ftl_check_core_thread(dev));
	assert(new_addr != FTL_ADDR_INVALID);
	assert(ftl_addr_cached(dev, new_addr));

	current_addr = ftl_l2p_get(dev, lba);

	if (current_addr != FTL_ADDR_INVALID) {

		/* Write-after-write happend */
		if (spdk_unlikely(current_addr != prev_addr
				  && ftl_addr_cached(dev, current_addr))) {

			current_chunk = ftl_nv_cache_get_chunk_from_addr(dev, current_addr);
			new_chunk = ftl_nv_cache_get_chunk_from_addr(dev, new_addr);

			/* To keep data consistency after recovery skip oldest block */
			if (current_chunk == new_chunk) {
				if (new_addr < current_addr) {
					return;
				}
			}
		}

		/* For recovery from SHM case valid maps need to be set before l2p set and
		 * invalidated after it */

		/* DO NOT CHANGE ORDER - START */
		ftl_nv_cache_set_addr(dev, lba, new_addr);
		ftl_l2p_set(dev, lba, new_addr);
		ftl_invalidate_addr(dev, current_addr);
		/* DO NOT CHANGE ORDER - END */
		return;
	}

	/* DO NOT CHANGE ORDER - START */
	ftl_nv_cache_set_addr(dev, lba, new_addr);
	ftl_l2p_set(dev, lba, new_addr);
	/* DO NOT CHANGE ORDER - END */
}

void
ftl_l2p_update(struct spdk_ftl_dev *dev, uint64_t lba, ftl_addr new_addr, ftl_addr weak_addr)
{
	ftl_addr current_addr;

	assert(ftl_check_core_thread(dev));
	assert(new_addr != FTL_ADDR_INVALID);
	assert(weak_addr != FTL_ADDR_INVALID);
	assert(!ftl_addr_cached(dev, new_addr));

	current_addr = ftl_l2p_get(dev, lba);

	if (current_addr == weak_addr) {
		/* DO NOT CHANGE ORDER - START */
		ftl_band_set_addr(ftl_band_from_addr(dev, new_addr), lba, new_addr);
		ftl_l2p_set(dev, lba, new_addr);
		/* In case update is from gc invalidate bit in lba map */
		ftl_invalidate_addr(dev, current_addr);
		/* DO NOT CHANGE ORDER - END */
	} else {
		ftl_invalidate_addr(dev, weak_addr);

		/* new addr could be set by running p2l checkpoint but in the time window between
		 * p2l checkpoint completion and l2p set operation new data could be written on
		 * open chunk so this address need to be invalidated */
		ftl_invalidate_addr(dev, new_addr);
	}
}

void
ftl_l2p_pin_complete(struct spdk_ftl_dev *dev, int status, struct ftl_l2p_pin_ctx *pin_ctx)
{
	if (spdk_unlikely(status == -EAGAIN)) {
		TAILQ_INSERT_TAIL(&dev->l2p_deferred_pins, pin_ctx, link);
	} else {
		pin_ctx->cb(dev, status, pin_ctx);
	}
}
