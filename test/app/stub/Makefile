#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2017 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

APP = stub

C_SRCS := stub.c

SPDK_LIB_LIST = $(SOCK_MODULES_LIST) event nvme

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
