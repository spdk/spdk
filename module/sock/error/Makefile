#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2026 Nutanix Inc. All rights reserved.

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 1
SO_MINOR := 0

C_SRCS = sock_error.c sock_error_rpc.c
LIBNAME = sock_error

SPDK_MAP_FILE = $(SPDK_ROOT_DIR)/mk/spdk_blank.map

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
