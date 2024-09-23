#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 1
SO_MINOR := 0

LIBNAME = xlio
C_SRCS = xlio.c

ifneq ($(strip $(CONFIG_XLIO_DIR)),)
CFLAGS += -I$(CONFIG_XLIO_DIR)/include
endif

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_xlio.map)

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
