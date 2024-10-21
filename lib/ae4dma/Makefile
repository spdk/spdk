#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2024 Advanced Micro Devices, Inc.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 1
SO_MINOR := 0

C_SRCS = ae4dma.c
LIBNAME = ae4dma

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_ae4dma.map)

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
