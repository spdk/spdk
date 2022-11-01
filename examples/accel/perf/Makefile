#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2017 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

APP = accel_perf

C_SRCS := accel_perf.c

SPDK_LIB_LIST = $(ACCEL_MODULES_LIST) event event_accel

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
