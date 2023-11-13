#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

APP = hello_fsdev

C_SRCS := hello_fsdev.c

SPDK_LIB_LIST = $(ALL_MODULES_LIST) $(FSDEV_MODULES_LIST) event event_fsdev

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
