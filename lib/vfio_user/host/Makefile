#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 3
SO_MINOR := 1

C_SRCS = vfio_user_pci.c vfio_user.c
LIBNAME = vfio_user

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_vfio_user.map)

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
