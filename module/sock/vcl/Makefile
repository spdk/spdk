#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 6
SO_MINOR := 0

LIBNAME = sock_vcl
C_SRCS = vcl.c

SPDK_MAP_FILE = $(SPDK_ROOT_DIR)/mk/spdk_blank.map

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
