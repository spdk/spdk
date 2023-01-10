#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

DIRS-y += trace
DIRS-y += trace_record
DIRS-y += nvmf_tgt
DIRS-y += iscsi_tgt
DIRS-y += spdk_tgt
DIRS-y += spdk_lspci
ifneq ($(OS),Windows)
# TODO - currently disabled on Windows due to lack of support for curses
DIRS-y += spdk_top
endif
ifeq ($(OS),Linux)
DIRS-$(CONFIG_VHOST) += vhost
DIRS-y += spdk_dd
endif

.PHONY: all clean $(DIRS-y)

all: $(DIRS-y)
clean: $(DIRS-y)

include $(SPDK_ROOT_DIR)/mk/spdk.subdirs.mk
