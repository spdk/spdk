#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 1
SO_MINOR := 0

LIBNAME = accel_mlx5
C_SRCS = accel_mlx5.c accel_mlx5_rpc.c

SPDK_MAP_FILE = $(SPDK_ROOT_DIR)/mk/spdk_blank.map

LOCAL_SYS_LIBS += -libverbs -lmlx5

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
