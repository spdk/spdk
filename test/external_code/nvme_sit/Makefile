#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2026 SanDisk. All rights reserved.

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

APP = sit_runner
CFLAGS += -I$(SPDK_ROOT_DIR)/lib/nvme
C_SRCS = sit_runner.c sit_common.c \
	sit_test_tracker_capacity.c
SPDK_LIB_LIST = nvme vmd sock trace log util

include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk
include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
