#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../../../..)
include $(SPDK_ROOT_DIR)/mk/config.mk

TEST_FILE = init_ut.c

include $(SPDK_ROOT_DIR)/mk/spdk.unittest.mk

CFLAGS += $(ENV_CFLAGS)
