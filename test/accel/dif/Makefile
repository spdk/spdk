#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2023 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

APP = dif

C_SRCS := dif.c
CFLAGS += -I$(SPDK_ROOT_DIR)/lib/bdev

SPDK_LIB_LIST = $(ALL_MODULES_LIST) event event_bdev

LIBS += -lcunit

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
