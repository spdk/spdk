#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

TEST_FILE = esnap.c

SPDK_LIB_LIST = accel bdev blob blob_bdev dma init notify

ifeq ($(OS),Linux)
SPDK_LIB_LIST += bdev_aio
endif

include $(SPDK_ROOT_DIR)/mk/spdk.unittest.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk
