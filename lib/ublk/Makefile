#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 1
SO_MINOR := 0

C_SRCS = ublk.c ublk_rpc.c
LIBNAME = ublk
LOCAL_SYS_LIBS = -luring

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_ublk.map)

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
