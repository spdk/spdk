#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

APP = jsoncat

C_SRCS = jsoncat.c

SPDK_LIB_LIST = json thread util log

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
