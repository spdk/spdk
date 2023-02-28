#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

UNIT_TEST_LINK_ENV = 1
TEST_FILE = mem_callbacks.c
CFLAGS += $(ENV_CFLAGS)

include $(SPDK_ROOT_DIR)/mk/spdk.unittest.mk
