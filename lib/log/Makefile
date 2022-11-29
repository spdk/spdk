#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 5
SO_MINOR := 1
SO_SUFFIX := $(SO_VER).$(SO_MINOR)

C_SRCS = log.c log_flags.c log_deprecated.c
LIBNAME = log

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_log.map)

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
