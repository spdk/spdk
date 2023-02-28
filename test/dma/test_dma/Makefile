#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

APP = test_dma
C_SRCS := test_dma.c

SPDK_LIB_LIST = $(ALL_MODULES_LIST) dma event event_bdev

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
