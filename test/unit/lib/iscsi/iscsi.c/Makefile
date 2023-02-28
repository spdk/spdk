#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../../../..)

SPDK_LIB_LIST = conf util

SCSI_OBJS = port
ISCSI_OBJS = md5 param
LIBS += $(SCSI_OBJS:%=$(SPDK_ROOT_DIR)/lib/scsi/%.o)
LIBS += $(ISCSI_OBJS:%=$(SPDK_ROOT_DIR)/lib/iscsi/%.o)
LIBS += -lcunit

TEST_FILE = iscsi_ut.c

include $(SPDK_ROOT_DIR)/mk/spdk.unittest.mk
