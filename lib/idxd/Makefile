#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 9
SO_MINOR := 0

C_SRCS = idxd.c idxd_user.c
ifeq ($(CONFIG_IDXD_KERNEL),y)
C_SRCS += idxd_kernel.c
endif

LIBNAME = idxd

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_idxd.map)

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
