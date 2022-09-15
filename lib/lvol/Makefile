#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 8
SO_MINOR := 0

C_SRCS = lvol.c
LIBNAME = lvol

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_lvol.map)

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
