#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation.
#  Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 2
SO_MINOR := 0
SO_SUFFIX := $(SO_VER).$(SO_MINOR)

LIBNAME = dma
C_SRCS = dma.c

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_dma.map)

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
