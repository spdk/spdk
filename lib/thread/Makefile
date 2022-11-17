#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 7
SO_MINOR := 1

C_SRCS = thread.c
LIBNAME = thread

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_thread.map)

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
