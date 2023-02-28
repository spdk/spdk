#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

APP = idxd_perf

C_SRCS := perf.c

SPDK_LIB_LIST = idxd util

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
