/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#ifndef FTL_L2P_H
#define FTL_L2P_H

#include "spdk/queue.h"

#include "ftl_internal.h"

struct spdk_ftl_dev;
struct ftl_nv_cache_chunk;
struct ftl_rq;
struct ftl_io;
struct ftl_l2p_pin_ctx;

typedef void (*ftl_l2p_cb)(struct spdk_ftl_dev *dev, int status, void *ctx);
typedef void (*ftl_l2p_pin_cb)(struct spdk_ftl_dev *dev, int status,
			       struct ftl_l2p_pin_ctx *pin_ctx);

int ftl_l2p_init(struct spdk_ftl_dev *dev);
void ftl_l2p_deinit(struct spdk_ftl_dev *dev);

struct ftl_l2p_pin_ctx {
	uint64_t lba;
	uint64_t count;
	ftl_l2p_pin_cb cb;
	void *cb_ctx;
	TAILQ_ENTRY(ftl_l2p_pin_ctx) link;
};

void ftl_l2p_pin(struct spdk_ftl_dev *dev, uint64_t lba, uint64_t count, ftl_l2p_pin_cb cb,
		 void *cb_ctx, struct ftl_l2p_pin_ctx *pin_ctx);
void ftl_l2p_unpin(struct spdk_ftl_dev *dev, uint64_t lba, uint64_t count);
void ftl_l2p_pin_skip(struct spdk_ftl_dev *dev, ftl_l2p_pin_cb cb, void *cb_ctx,
		      struct ftl_l2p_pin_ctx *pin_ctx);

void ftl_l2p_set(struct spdk_ftl_dev *dev, uint64_t lba, ftl_addr addr);
ftl_addr ftl_l2p_get(struct spdk_ftl_dev *dev, uint64_t lba);

void ftl_l2p_clear(struct spdk_ftl_dev *dev, ftl_l2p_cb cb, void *cb_ctx);
void ftl_l2p_unmap(struct spdk_ftl_dev *dev, ftl_l2p_cb cb, void *cb_ctx);
void ftl_l2p_restore(struct spdk_ftl_dev *dev, ftl_l2p_cb cb, void *cb_ctx);
void ftl_l2p_persist(struct spdk_ftl_dev *dev, ftl_l2p_cb cb, void *cb_ctx);
void ftl_l2p_process(struct spdk_ftl_dev *dev);
bool ftl_l2p_is_halted(struct spdk_ftl_dev *dev);
void ftl_l2p_halt(struct spdk_ftl_dev *dev);
void ftl_l2p_resume(struct spdk_ftl_dev *dev);

void ftl_l2p_update_cache(struct spdk_ftl_dev *dev, uint64_t lba, ftl_addr new_addr,
			  ftl_addr old_addr);
void ftl_l2p_update_base(struct spdk_ftl_dev *dev, uint64_t lba, ftl_addr new_addr,
			 ftl_addr old_addr);

void ftl_l2p_pin_complete(struct spdk_ftl_dev *dev, int status, struct ftl_l2p_pin_ctx *pin_ctx);

#endif /* FTL_L2P_H */
