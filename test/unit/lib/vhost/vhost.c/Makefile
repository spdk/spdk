#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../../../..)
include $(SPDK_ROOT_DIR)/mk/config.mk

TEST_FILE = vhost_ut.c

include $(SPDK_ROOT_DIR)/mk/spdk.unittest.mk

CFLAGS += $(ENV_CFLAGS)
