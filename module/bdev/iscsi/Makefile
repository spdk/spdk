#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2017 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 4
SO_MINOR := 0

CFLAGS += -I$(SPDK_ROOT_DIR)/lib/bdev/
# CentOS 7 libiscsi package has functions declared inline but not
# defined in the header file.  Not aware of any way to disable
# this warning so just make sure the warning isn't treated as
# an error.
CFLAGS += -Wno-error
C_SRCS = bdev_iscsi.c bdev_iscsi_rpc.c
LIBNAME = bdev_iscsi

SPDK_MAP_FILE = $(SPDK_ROOT_DIR)/mk/spdk_blank.map

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
