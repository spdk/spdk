#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 4
SO_MINOR := 0

C_SRCS = blobfs_bdev.c blobfs_bdev_rpc.c

# libfuse3 is required by blobfs_fuse.c
ifeq ($(CONFIG_FUSE),y)
C_SRCS += blobfs_fuse.c
endif

LIBNAME = blobfs_bdev

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_blobfs_bdev.map)

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
