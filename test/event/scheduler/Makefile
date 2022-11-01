#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2017 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

APP = scheduler
C_SRCS := scheduler.c

SPDK_LIB_LIST = $(ALL_MODULES_LIST) event event_bdev conf event_scheduler

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
