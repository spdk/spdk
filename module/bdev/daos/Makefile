# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) croit GmbH.
# All rights reserved.

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 1
SO_MINOR := 0

C_SRCS = bdev_daos.c bdev_daos_rpc.c
LIBNAME = bdev_daos

SPDK_MAP_FILE = $(SPDK_ROOT_DIR)/mk/spdk_blank.map

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
