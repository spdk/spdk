#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

APP = spdk_trace
SPDK_NO_LINK_ENV = 1

SPDK_LIB_LIST += json trace_parser

CXX_SRCS := trace.cpp

include $(SPDK_ROOT_DIR)/mk/spdk.app_cxx.mk

install: $(APP)
	$(INSTALL_APP)

uninstall:
	$(UNINSTALL_APP)
