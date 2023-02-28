/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#ifndef FTL_MNGT_STEPS_H
#define FTL_MNGT_STEPS_H

#include "ftl_mngt.h"

void ftl_mngt_check_conf(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_open_base_bdev(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_close_base_bdev(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

#ifdef SPDK_FTL_VSS_EMU
void ftl_mngt_md_init_vss_emu(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_md_deinit_vss_emu(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);
#endif

void ftl_mngt_superblock_init(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_superblock_deinit(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_open_cache_bdev(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_close_cache_bdev(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_register_io_device(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_unregister_io_device(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_init_mem_pools(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_deinit_mem_pools(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_init_bands(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_init_bands_md(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_deinit_bands(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_deinit_bands_md(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_init_io_channel(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_deinit_io_channel(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_decorate_bands(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_initialize_band_address(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_init_reloc(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_deinit_reloc(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_init_nv_cache(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_deinit_nv_cache(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_init_l2p(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_deinit_l2p(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_clear_l2p(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_unmap_l2p(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_restore_l2p(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_scrub_nv_cache(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_finalize_init_bands(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_finalize_startup(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_start_core_poller(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_stop_core_poller(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_persist_l2p(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_init_layout(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_layout_verify(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_layout_upgrade(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_init_md(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_deinit_md(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_persist_md(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_fast_persist_md(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_rollback_device(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_dump_stats(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_init_default_sb(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_set_dirty(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_set_clean(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_set_shm_clean(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_load_sb(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_validate_sb(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_restore_md(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_recover(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_init_vld_map(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_deinit_vld_map(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_init_unmap_map(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_deinit_unmap_map(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_unmap_clear(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_p2l_init_ckpt(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_p2l_deinit_ckpt(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_p2l_wipe(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_p2l_free_bufs(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_p2l_restore_ckpt(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_self_test(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_persist_band_info_metadata(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_persist_nv_cache_metadata(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_persist_superblock(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

#endif /* FTL_MNGT_STEPS_H */
