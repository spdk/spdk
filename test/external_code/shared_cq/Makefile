#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2026 SanDisk. All rights reserved.

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

APP = shared_cq_sit
C_SRCS = shared_cq_sit.c
SPDK_LIB_LIST = nvme

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
