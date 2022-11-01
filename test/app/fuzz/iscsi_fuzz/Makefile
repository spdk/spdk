#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2019 Intel Corporation
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

APP = iscsi_fuzz

CFLAGS += -I$(SPDK_ROOT_DIR)/lib
CFLAGS += -I$(SPDK_ROOT_DIR)/include

C_SRCS := iscsi_fuzz.c

ISCSI_OBJS = md5 param conn tgt_node init_grp portal_grp

LIBS += $(SCSI_OBJS:%=$(SPDK_ROOT_DIR)/lib/scsi/%.o)
LIBS += $(ISCSI_OBJS:%=$(SPDK_ROOT_DIR)/lib/iscsi/%.o)

SPDK_LIB_LIST += $(SOCK_MODULES_LIST) event scsi

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
