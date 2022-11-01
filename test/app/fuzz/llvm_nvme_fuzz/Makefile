#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2018 Intel Corporation
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

APP = llvm_nvme_fuzz

C_SRCS := llvm_nvme_fuzz.c

LDFLAGS+= -lstdc++

SPDK_LIB_LIST = $(ALL_MODULES_LIST) event event_nvmf

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
