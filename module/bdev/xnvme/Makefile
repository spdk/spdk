#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) Samsung Electronics Co., Ltd.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 1
SO_MINOR := 0

C_SRCS = bdev_xnvme.c bdev_xnvme_rpc.c
LIBNAME = bdev_xnvme

CFLAGS += -I$(SPDK_ROOT_DIR)/xnvme/include

SPDK_MAP_FILE = $(SPDK_ROOT_DIR)/mk/spdk_blank.map

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
