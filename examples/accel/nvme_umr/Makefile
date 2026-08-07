#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 Dell Inc, or its subsidiaries.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

APP = nvme_umr
C_SRCS := nvme_umr.c

SPDK_LIB_LIST = $(SOCK_MODULES_LIST) nvme thread accel_mlx5
CFLAGS += -DNVME_ACCEL_MLX5

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
