#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

APP = llvm_vfio_fuzz

C_SRCS := llvm_vfio_fuzz.c

LDFLAGS+= -lstdc++

SPDK_LIB_LIST = $(ALL_MODULES_LIST) event event_nvmf

ifeq ($(SPDK_ROOT_DIR)/lib/env_dpdk,$(CONFIG_ENV))
SPDK_LIB_LIST += env_dpdk_rpc
endif

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
