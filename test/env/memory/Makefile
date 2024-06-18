#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)

TEST_FILE = memory_ut.c

include $(SPDK_ROOT_DIR)/mk/spdk.unittest.mk

CFLAGS += $(ENV_CFLAGS)
CFLAGS += -I$(SPDK_ROOT_DIR)/test/lib
