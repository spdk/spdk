#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 4
SO_MINOR := 0

C_SRCS = ftl_band.c ftl_core.c ftl_debug.c ftl_io.c ftl_reloc.c \
	 ftl_restore.c ftl_init.c ftl_trace.c

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_ftl.map)

LIBNAME = ftl

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
