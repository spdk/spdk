#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2018 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

APP = hello_sock

C_SRCS := hello_sock.c

SPDK_LIB_LIST = $(SOCK_MODULES_LIST)
SPDK_LIB_LIST += event sock

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
