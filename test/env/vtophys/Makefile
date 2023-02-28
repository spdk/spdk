#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

APP = vtophys
C_SRCS := vtophys.c
SPDK_LIB_LIST += thread util log

SYS_LIBS += -lcunit

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
