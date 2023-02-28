/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#ifndef NVMF_TGT_H
#define NVMF_TGT_H

#include "spdk/stdinc.h"

#include "spdk/nvmf.h"
#include "spdk/queue.h"

#include "spdk_internal/init.h"
#include "spdk/log.h"

struct spdk_nvmf_admin_passthru_conf {
	bool identify_ctrlr;
};

struct spdk_nvmf_tgt_conf {
	struct spdk_nvmf_admin_passthru_conf admin_passthru;
	enum spdk_nvmf_tgt_discovery_filter discovery_filter;
};

extern struct spdk_nvmf_tgt_conf g_spdk_nvmf_tgt_conf;

extern uint32_t g_spdk_nvmf_tgt_max_subsystems;
extern uint16_t g_spdk_nvmf_tgt_crdt[3];

extern struct spdk_nvmf_tgt *g_spdk_nvmf_tgt;

extern struct spdk_cpuset *g_poll_groups_mask;

#endif
