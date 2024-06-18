#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../../../..)

TEST_FILE = ftl_l2p_ut.c

include $(SPDK_ROOT_DIR)/mk/spdk.unittest.mk

CFLAGS += -I$(SPDK_ROOT_DIR)/lib/ftl/utils

CFLAGS += -I$(SPDK_ROOT_DIR)/lib/ftl
