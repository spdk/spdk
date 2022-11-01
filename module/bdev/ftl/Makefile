#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 4
SO_MINOR := 0

CFLAGS += -I$(SPDK_ROOT_DIR)/lib/ftl/

ifdef SPDK_FTL_VSS_EMU
CFLAGS += -DSPDK_FTL_VSS_EMU
endif

C_SRCS += bdev_ftl.c bdev_ftl_rpc.c
LIBNAME = bdev_ftl

SPDK_MAP_FILE = $(SPDK_ROOT_DIR)/mk/spdk_blank.map

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
