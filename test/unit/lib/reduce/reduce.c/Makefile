#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../../../..)

TEST_FILE = reduce_ut.c
LDFLAGS += -Wl,--wrap,unlink

include $(SPDK_ROOT_DIR)/mk/spdk.unittest.mk
