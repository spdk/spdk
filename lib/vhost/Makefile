#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 6
SO_MINOR := 0

CFLAGS += -I.
CFLAGS += $(ENV_CFLAGS)

C_SRCS = vhost.c vhost_rpc.c vhost_scsi.c vhost_blk.c rte_vhost_user.c

LIBNAME = vhost

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_vhost.map)

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
