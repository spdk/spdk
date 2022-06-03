#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)

UNIT_TEST_LINK_ENV = 1
CFLAGS += $(ENV_CFLAGS)
TEST_FILE = pci_ut.c

include $(SPDK_ROOT_DIR)/mk/spdk.unittest.mk
