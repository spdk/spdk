#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)

include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 3
SO_MINOR := 0

CXX_SRCS = trace.cpp
LIBNAME = trace_parser
LOCAL_SYS_LIBS = -lrt

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_trace_parser.map)

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
