#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 4
SO_MINOR := 0

CFLAGS += $(ENV_CFLAGS)
C_SRCS = virtio.c virtio_user.c virtio_pci.c vhost_user.c
LIBNAME = virtio

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_virtio.map)

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
