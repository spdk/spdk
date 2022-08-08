#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 10
SO_MINOR := 0
SO_SUFFIX := $(SO_VER).$(SO_MINOR)

LIBNAME = accel
C_SRCS = accel.c accel_rpc.c accel_sw.c

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_accel.map)

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
