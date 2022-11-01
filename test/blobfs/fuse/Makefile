#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2017 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

APP = fuse

C_SRCS := fuse.c

SPDK_LIB_LIST = $(ALL_MODULES_LIST) event_bdev

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
