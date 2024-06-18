#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2017 Intel Corporation
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

APP = blobcli

C_SRCS := blobcli.c

# Don't link bdev_lvol in blobcli - otherwise this utility cannot operate on an lvolstore
SPDK_LIB_LIST = $(filter-out bdev_lvol,$(ALL_MODULES_LIST)) event event_bdev

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
