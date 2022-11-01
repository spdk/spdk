#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2018 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)

include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 4
SO_MINOR := 0

CFLAGS += $(ENV_CFLAGS) -I$(SPDK_ROOT_DIR)/lib/env_ocf -I$(SPDK_ROOT_DIR)/lib/env_ocf/include
C_SRCS = $(shell ls *.c)

LIBNAME := bdev_ocf

SPDK_MAP_FILE = $(SPDK_ROOT_DIR)/mk/spdk_blank.map

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk

OCF_ENV := $(call spdk_lib_list_to_static_libs,ocfenv)

$(LIB) : $(OCF_ENV)
