#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 11
SO_MINOR := 0

CFLAGS += $(ENV_CFLAGS) -Wno-address-of-packed-member

LIBNAME = event
C_SRCS = app.c reactor.c log_rpc.c \
	 app_rpc.c scheduler_static.c

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_event.map)

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
