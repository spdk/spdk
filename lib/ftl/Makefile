#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 5
SO_MINOR := 0

CFLAGS += -I.

C_SRCS = ftl_core.c

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_ftl.map)

LIBNAME = ftl

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
