#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 6
SO_MINOR := 0

CFLAGS += -I$(SPDK_ROOT_DIR)/lib
C_SRCS = conn.c \
	 init_grp.c iscsi.c md5.c param.c portal_grp.c \
	 tgt_node.c iscsi_subsystem.c \
	 iscsi_rpc.c task.c
LIBNAME = iscsi
LOCAL_SYS_LIBS = -lcrypto

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_iscsi.map)

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
