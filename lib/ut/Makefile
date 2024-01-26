#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2023 Intel Corporation. All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 2
SO_MINOR := 0

C_SRCS = ut.c
LIBNAME = ut

LOCAL_SYS_LIBS = -lcunit

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_ut.map)

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
