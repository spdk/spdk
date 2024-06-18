#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

APP = bdev_svc

C_SRCS := bdev_svc.c

SPDK_LIB_LIST = $(ALL_MODULES_LIST) event event_bdev

ifeq ($(OS),Linux)
SPDK_LIB_LIST += event event_nbd
endif

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
