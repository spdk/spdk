#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 1
SO_MINOR := 0

C_SRCS += tgt_endpoint.c tgt_rpc.c
CFLAGS += -I$(VFIO_USER_INCLUDE_DIR)
LDFLAGS += -L$(VFIO_USER_LIBRARY_DIR)
LOCAL_SYS_LIBS += -lvfio-user -ljson-c

LIBNAME = vfu_tgt

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_vfu_tgt.map)

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
