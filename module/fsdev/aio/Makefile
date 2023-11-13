#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 1
SO_MINOR := 0

C_SRCS = fsdev_aio.c fsdev_aio_rpc.c

ifeq ($(OS),Linux)
C_SRCS += linux_aio_mgr.c
LOCAL_SYS_LIBS = -laio
else
$(info $(UNAME): using the POSIX aio)
C_SRCS += aio_mgr.c
endif

CFLAGS += $(shell pkg-config --cflags glib-2.0)
CXXFLAGS += $(shell pkg-config --cflags glib-2.0)

LIBNAME = fsdev_aio

SPDK_MAP_FILE = $(SPDK_ROOT_DIR)/mk/spdk_blank.map

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
