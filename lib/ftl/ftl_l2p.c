/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "ftl_l2p.h"
#include "ftl_band.h"
#include "ftl_nv_cache.h"
#include "ftl_l2p_cache.h"
#include "ftl_l2p_flat.h"


/* TODO: Verify why function pointers had worse performance than compile time constants */
#ifdef SPDK_FTL_L2P_FLAT
#define FTL_L2P_OP(name)	ftl_l2p_flat_ ## name
#else
#define FTL_L2P_OP(name)	ftl_l2p_cache_ ## name
#endif


int
ftl_l2p_init(struct spdk_ftl_dev *dev)
{
	TAILQ_INIT(&dev->l2p_deferred_pins);
	return FTL_L2P_OP(init)(dev);
}

void
ftl_l2p_deinit(struct spdk_ftl_dev *dev)
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
ftl_l2p_restore(struct spdk_ftl_dev *dev, ftl_l2p_cb cb, void *cb_ctx)
{
	FTL_L2P_OP(restore)(dev, cb, cb_ctx);
}

void
ftl_l2p_persist(struct spdk_ftl_dev *dev, ftl_l2p_cb cb, void *cb_ctx)
{
	FTL_L2P_OP(persist)(dev, cb, cb_ctx);
}

void
ftl_l2p_unmap(struct spdk_ftl_dev *dev, ftl_l2p_cb cb, void *cb_ctx)
{
	FTL_L2P_OP(unmap)(dev, cb, cb_ctx);
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
ftl_l2p_resume(struct spdk_ftl_dev *dev)
{
	return FTL_L2P_OP(resume)(dev);
}

void
ftl_l2p_halt(struct spdk_ftl_dev *dev)
{
	return FTL_L2P_OP(halt)(dev);
}

static uint64_t
get_trim_seq_id(struct spdk_ftl_dev *dev, uint64_t lba)
{
	struct ftl_md *md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_TRIM_MD];
	uint64_t *page = ftl_md_get_buffer(md);
	uint64_t page_no = lba / dev->layout.l2p.lbas_in_page;

	return page[page_no];
}

void
ftl_l2p_update_cache(struct spdk_ftl_dev *dev, uint64_t lba, ftl_addr new_addr, ftl_addr old_addr)
{
	struct ftl_nv_cache_chunk *current_chunk, *new_chunk;
	ftl_addr current_addr;
	/* Updating L2P for data in cache device - used by user writes.
	 * Split off from updating L2P in base due to extra edge cases for handling dirty shutdown in the cache case,
	 * namely keeping two simultaneous writes to same LBA consistent before/after shutdown - on base device we
	 * can simply ignore the L2P update, here we need to keep the address with more advanced write pointer
	 */
	assert(ftl_check_core_thread(dev));
	assert(new_addr != FTL_ADDR_INVALID);
	assert(ftl_addr_in_nvc(dev, new_addr));

	current_addr = ftl_l2p_get(dev, lba);

	if (current_addr != FTL_ADDR_INVALID) {

		/* Check if write-after-write happened (two simultaneous user writes to the same LBA) */
		if (spdk_unlikely(current_addr != old_addr
				  && ftl_addr_in_nvc(dev, current_addr))) {

			current_chunk = ftl_nv_cache_get_chunk_from_addr(dev, current_addr);
			new_chunk = ftl_nv_cache_get_chunk_from_addr(dev, new_addr);

			/* To keep data consistency after recovery skip oldest block */
			/* If both user writes are to the same chunk, the highest address should 'win', to keep data after
			 * dirty shutdown recovery consistent. If they're on different chunks, then higher seq_id chunk 'wins' */
			if (current_chunk == new_chunk) {
				if (new_addr < current_addr) {
					return;
				}
			} else {
				if (new_chunk->md->seq_id < current_chunk->md->seq_id) {
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
	} else {
		uint64_t trim_seq_id = get_trim_seq_id(dev, lba);
		uint64_t new_seq_id = ftl_nv_cache_get_chunk_from_addr(dev, new_addr)->md->seq_id;

		/* Check if region hasn't been unmapped during IO */
		if (new_seq_id < trim_seq_id) {
			return;
		}
	}

	/* If current address doesn't have any value (ie. it was never set, or it was trimmed), then we can just set L2P */
	/* DO NOT CHANGE ORDER - START (need to set P2L maps/valid map first) */
	ftl_nv_cache_set_addr(dev, lba, new_addr);
	ftl_l2p_set(dev, lba, new_addr);
	/* DO NOT CHANGE ORDER - END */
}

void
ftl_l2p_update_base(struct spdk_ftl_dev *dev, uint64_t lba, ftl_addr new_addr, ftl_addr old_addr)
{
	ftl_addr current_addr;

	/* Updating L2P for data in base device - used by compaction and GC, may be invalidated by user write.
	 * Split off from updating L2P in cache due to extra edge cases for handling dirty shutdown in the cache case.
	 * Also some assumptions are not the same (can't assign INVALID address for base device - trim cases are done on cache)
	 */
	assert(ftl_check_core_thread(dev));
	assert(new_addr != FTL_ADDR_INVALID);
	assert(old_addr != FTL_ADDR_INVALID);
	assert(!ftl_addr_in_nvc(dev, new_addr));

	current_addr = ftl_l2p_get(dev, lba);

	if (current_addr == old_addr) {
		/* DO NOT CHANGE ORDER - START (need to set L2P (and valid bits), before invalidating old ones,
		 * due to dirty shutdown from shm recovery - it's ok to have too many bits set, but not ok to
		 * have too many cleared) */
		ftl_band_set_addr(ftl_band_from_addr(dev, new_addr), lba, new_addr);
		ftl_l2p_set(dev, lba, new_addr);
		/* DO NOT CHANGE ORDER - END */
	} else {
		/* new addr could be set by running p2l checkpoint but in the time window between
		 * p2l checkpoint completion and l2p set operation new data could be written on
		 * open chunk so this address need to be invalidated */
		ftl_invalidate_addr(dev, new_addr);
	}

	ftl_invalidate_addr(dev, old_addr);
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
