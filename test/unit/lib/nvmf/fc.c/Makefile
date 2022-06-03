#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2018 Broadcom.  All Rights Reserved.
#  The term "Broadcom" refers to Broadcom Limited and/or its subsidiaries.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../../../../)
include $(SPDK_ROOT_DIR)/mk/config.mk

CFLAGS += -I$(SPDK_ROOT_DIR)/test/common/lib -I$(SPDK_ROOT_DIR)/lib \
-I$(SPDK_ROOT_DIR)/lib/nvmf $(ENV_CFLAGS)

ifneq ($(strip $(CONFIG_FC_PATH)),)
CFLAGS += -I$(CONFIG_FC_PATH)
endif

TEST_FILE = fc_ut.c

include $(SPDK_ROOT_DIR)/mk/spdk.unittest.mk

# Disable clang warning: taking address of packed member of class or structure may result in an unaligned pointer value [-Werror,-Waddress-of-packed-member]
ifeq ($(CC),clang)
	CLANG_VERSION := $(shell $(CC) -v 2>&1 | \
		        sed -n "s/.*version \([0-9]*\.[0-9]*\).*/\1/p")

CLANG_MAJOR_VERSION := $(shell echo $(CLANG_VERSION) | cut -f1 -d.)

ifeq ($(shell test $(CLANG_MAJOR_VERSION) -ge 4 && echo 1), 1)
	CFLAGS += -Wno-address-of-packed-member
endif
endif
