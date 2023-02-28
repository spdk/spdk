#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2018 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

APP = rpc_client_test

C_SRCS := rpc_client_test.c

SPDK_LIB_LIST = rpc

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
