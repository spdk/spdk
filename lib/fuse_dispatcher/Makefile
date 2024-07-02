#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES.
#  All rights reserved.

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 1
SO_MINOR := 0

C_SRCS = fuse_dispatcher.c
LIBNAME = fuse_dispatcher

INCLUDE_ROOT=$(abspath $(CURDIR)/../../include)

CFLAGS += -I$(INCLUDE_ROOT)

SPDK_MAP_FILE = $(abspath $(CURDIR)/fuse_dispatcher.map)

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
