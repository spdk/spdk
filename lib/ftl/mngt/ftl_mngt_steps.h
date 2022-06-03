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

#ifndef FTL_MNGT_STEPS_H
#define FTL_MNGT_STEPS_H

#include "ftl_mngt.h"

void ftl_mngt_check_conf(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

void ftl_mngt_open_base_bdev(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

void ftl_mngt_close_base_bdev(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

#ifdef SPDK_FTL_VSS_EMU
void ftl_mngt_md_init_vss_emu(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

void ftl_mngt_md_deinit_vss_emu(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);
#endif

void ftl_mngt_superblock_init(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

void ftl_mngt_superblock_deinit(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

void ftl_mngt_open_cache_bdev(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

void ftl_mngt_close_cache_bdev(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

void ftl_mngt_init_mem_pools(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

void ftl_mngt_deinit_mem_pools(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

void ftl_mngt_init_bands(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

void ftl_mngt_init_bands_md(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

void ftl_mngt_deinit_bands(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

void ftl_mngt_init_io_channel(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

void ftl_mngt_deinit_io_channel(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

void ftl_mngt_init_zone(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

void ftl_mngt_decorate_bands(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

void ftl_mngt_init_reloc(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

void ftl_mngt_deinit_reloc(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

void ftl_mngt_init_nv_cache(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

void ftl_mngt_deinit_nv_cache(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

void ftl_mngt_init_l2p(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

void ftl_mngt_deinit_l2p(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

void ftl_mngt_clear_l2p(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

void ftl_mngt_scrub_nv_cache(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

void ftl_mngt_finalize_init_bands(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

void ftl_mngt_finalize_init(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

void ftl_mngt_start_task_core(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

void ftl_mngt_stop_task_core(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

void ftl_mngt_init_layout(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

void ftl_mngt_init_md(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

void ftl_mngt_deinit_md(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

void ftl_mngt_rollback_device(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

void ftl_mngt_dump_stats(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

void ftl_mngt_init_default_sb(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

void ftl_mngt_set_dirty(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

void ftl_mngt_load_sb(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

void ftl_mngt_validate_sb(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

void ftl_mngt_persist_band_info_metadata(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

void ftl_mngt_persist_nv_cache_metadata(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

void ftl_mngt_update_supeblock(struct spdk_ftl_dev *dev, ftl_mngt_fn cb, void *cb_cntx);

void ftl_mngt_persist_superblock(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt);

#endif /* FTL_MNGT_STEPS_H */
