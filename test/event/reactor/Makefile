#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

APP = reactor
C_SRCS := reactor.c

SPDK_LIB_LIST = event

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
