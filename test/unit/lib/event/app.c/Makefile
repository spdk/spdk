#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../../../..)

CFLAGS += -Wno-address-of-packed-member

SPDK_LIB_LIST = conf trace jsonrpc json
TEST_FILE = app_ut.c

include $(SPDK_ROOT_DIR)/mk/spdk.unittest.mk
