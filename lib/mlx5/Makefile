#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 1
SO_MINOR := 0

C_SRCS = mlx5_crypto.c
LIBNAME = mlx5

LOCAL_SYS_LIBS += -lmlx5 -libverbs -lrdmacm

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_mlx5.map)

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
