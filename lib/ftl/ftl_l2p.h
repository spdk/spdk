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

void ftl_l2p_update_cached(struct spdk_ftl_dev *dev, uint64_t lba, ftl_addr new_addr,
			   ftl_addr prev_addr);
void ftl_l2p_update(struct spdk_ftl_dev *dev, uint64_t lba, ftl_addr new_addr, ftl_addr weak_addr);

void ftl_l2p_pin_complete(struct spdk_ftl_dev *dev, int status, struct ftl_l2p_pin_ctx *pin_ctx);

#endif /* FTL_L2P_H */
