#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

TEST_FILE = rpc_ut.c

SPDK_MOCK_SYSCALLS = \
	open \
	close \
	flock \
	unlink

include $(SPDK_ROOT_DIR)/mk/spdk.unittest.mk
