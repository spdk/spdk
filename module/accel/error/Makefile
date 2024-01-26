# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2023 Intel Corporation. All rights reserved.

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 2
SO_MINOR := 0

LIBNAME = accel_error
C_SRCS = accel_error.c accel_error_rpc.c

SPDK_MAP_FILE = $(SPDK_ROOT_DIR)/mk/spdk_blank.map

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
