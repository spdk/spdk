#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 Advanced Micro Devices, Inc.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

APP = ntb_perf

C_SRCS := perf.c

# List every SPDK library this app actually pulls symbols from. Some of
# these would be picked up transitively today (env_dpdk via ntb_amd,
# log via util/event), but spelling them out is required by SPDK style
# and makes link failures easy to diagnose:
#   ntb_amd  - the NTB driver itself (libspdk_ntb_amd)
#   ae4dma   - DMA-offload helpers used by perf
#   env_dpdk - spdk_pci_ntb_amd_get_driver(), BAR/DMA helpers
#   log      - SPDK_ERRLOG / SPDK_NOTICELOG used throughout perf.c
#   util     - byte-order, memory and other small helpers
#   event    - reactor / app shell
#   thread   - poller registration and thread plumbing
SPDK_LIB_LIST = ntb_amd ae4dma env_dpdk log util event thread

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
