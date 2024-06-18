#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2017 Intel Corporation.
#  Copyright (c) 2015-2016, Micron Technology, Inc.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

APP := nvmf

C_SRCS := nvmf.c
SPDK_LIB_LIST = $(ALL_MODULES_LIST) event event_bdev nvmf

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
