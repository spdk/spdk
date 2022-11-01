#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 4
SO_MINOR := 0

C_SRCS = notify.c notify_rpc.c
LIBNAME = notify

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_notify.map)

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
