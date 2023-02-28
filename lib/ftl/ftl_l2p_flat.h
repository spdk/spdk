/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#ifndef FTL_L2P_FLAT_H
#define FTL_L2P_FLAT_H

int ftl_l2p_flat_init(struct spdk_ftl_dev *dev);
void ftl_l2p_flat_deinit(struct spdk_ftl_dev *dev);
void ftl_l2p_flat_pin(struct spdk_ftl_dev *dev, struct ftl_l2p_pin_ctx *pin_ctx);
void ftl_l2p_flat_unpin(struct spdk_ftl_dev *dev, uint64_t lba, uint64_t count);
ftl_addr ftl_l2p_flat_get(struct spdk_ftl_dev *dev, uint64_t lba);
void ftl_l2p_flat_set(struct spdk_ftl_dev *dev, uint64_t lba, ftl_addr addr);
void ftl_l2p_flat_unmap(struct spdk_ftl_dev *dev, ftl_l2p_cb cb, void *cb_ctx);
void ftl_l2p_flat_clear(struct spdk_ftl_dev *dev, ftl_l2p_cb cb, void *cb_ctx);
void ftl_l2p_flat_restore(struct spdk_ftl_dev *dev, ftl_l2p_cb cb, void *cb_ctx);
void ftl_l2p_flat_persist(struct spdk_ftl_dev *dev, ftl_l2p_cb cb, void *cb_ctx);
void ftl_l2p_flat_process(struct spdk_ftl_dev *dev);
bool ftl_l2p_flat_is_halted(struct spdk_ftl_dev *dev);
void ftl_l2p_flat_halt(struct spdk_ftl_dev *dev);
void ftl_l2p_flat_resume(struct spdk_ftl_dev *dev);

#endif /* FTL_L2P_FLAT_H */
