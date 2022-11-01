#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2020 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

APP = app_repeat
C_SRCS := app_repeat.c

# Some of the modules and libraries are not repeatable yet, only organize
# the repeatable ones.
SPDK_LIB_LIST = event_bdev event_accel event_vmd event_sock
SPDK_LIB_LIST += event_nbd

BLOCKDEV_LIST = bdev_malloc bdev_null
BLOCKDEV_LIST += bdev_aio
SYS_LIBS += -laio

SPDK_LIB_LIST += $(BLOCKDEV_LIST) event

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
