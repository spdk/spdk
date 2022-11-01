#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2018 Intel Corporation
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

APP = nvme_fuzz

CFLAGS += -I$(SPDK_ROOT_DIR)/test/app/fuzz/common

C_SRCS := nvme_fuzz.c

SPDK_LIB_LIST += $(SOCK_MODULES_LIST) event nvme conf

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
