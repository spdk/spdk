#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#  Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

# These directories contain tests.
TESTDIRS = app bdev blobfs cpp_headers dma env event lvol nvme rpc_client thread

DIRS-$(CONFIG_TESTS) += $(TESTDIRS)
DIRS-$(CONFIG_UNIT_TESTS) += unit

.PHONY: all clean $(DIRS-y)

all: $(DIRS-y)
clean: $(DIRS-y)

include $(SPDK_ROOT_DIR)/mk/spdk.subdirs.mk
