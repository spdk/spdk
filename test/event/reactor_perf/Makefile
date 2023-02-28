#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

APP = reactor_perf
C_SRCS := reactor_perf.c

SPDK_LIB_LIST = event

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
