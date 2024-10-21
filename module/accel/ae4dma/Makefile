#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2024 Advanced Micro Devices, Inc.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 1
SO_MINOR := 0

LIBNAME = accel_ae4dma
C_SRCS = accel_ae4dma.c accel_ae4dma_rpc.c

SPDK_MAP_FILE = $(SPDK_ROOT_DIR)/mk/spdk_blank.map

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
