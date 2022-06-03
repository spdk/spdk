#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2018 Broadcom.  All Rights Reserved.
#  The term "Broadcom" refers to Broadcom Limited and/or its subsidiaries.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../../../../)
include $(SPDK_ROOT_DIR)/mk/config.mk

CFLAGS += -I$(SPDK_ROOT_DIR)/test/common/lib -I$(SPDK_ROOT_DIR)/lib/nvmf $(ENV_CFLAGS)

ifneq ($(strip $(CONFIG_FC_PATH)),)
CFLAGS += -I$(CONFIG_FC_PATH)
endif

TEST_FILE = fc_ls_ut.c

include $(SPDK_ROOT_DIR)/mk/spdk.unittest.mk
