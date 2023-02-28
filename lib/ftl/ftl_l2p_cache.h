/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#ifndef FTL_L2P_CACHE_H
#define FTL_L2P_CACHE_H

#define FTL_L2P_CACHE_MD_NAME_L1		"l2p_l1"
#define FTL_L2P_CACHE_MD_NAME_L2		"l2p_l2"
#define FTL_L2P_CACHE_MD_NAME_L2_CTX		"l2p_l2_ctx"

int ftl_l2p_cache_init(struct spdk_ftl_dev *dev);
void ftl_l2p_cache_deinit(struct spdk_ftl_dev *dev);
void ftl_l2p_cache_pin(struct spdk_ftl_dev *dev, struct ftl_l2p_pin_ctx *pin_ctx);
void ftl_l2p_cache_unpin(struct spdk_ftl_dev *dev, uint64_t lba, uint64_t count);
ftl_addr ftl_l2p_cache_get(struct spdk_ftl_dev *dev, uint64_t lba);
void ftl_l2p_cache_set(struct spdk_ftl_dev *dev, uint64_t lba, ftl_addr addr);
void ftl_l2p_cache_unmap(struct spdk_ftl_dev *dev, ftl_l2p_cb cb, void *cb_ctx);
void ftl_l2p_cache_clear(struct spdk_ftl_dev *dev, ftl_l2p_cb cb, void *cb_ctx);
void ftl_l2p_cache_restore(struct spdk_ftl_dev *dev, ftl_l2p_cb cb, void *cb_ctx);
void ftl_l2p_cache_persist(struct spdk_ftl_dev *dev, ftl_l2p_cb cb, void *cb_ctx);
void ftl_l2p_cache_process(struct spdk_ftl_dev *dev);
bool ftl_l2p_cache_is_halted(struct spdk_ftl_dev *dev);
void ftl_l2p_cache_halt(struct spdk_ftl_dev *dev);
void ftl_l2p_cache_resume(struct spdk_ftl_dev *dev);

#endif /* FTL_L2P_CACHE_H */
