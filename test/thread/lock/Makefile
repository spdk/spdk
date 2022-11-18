#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

APP = spdk_lock
C_SRCS := spdk_lock.c
CFLAGS += -I$(SPDK_ROOT_DIR)/lib

SPDK_LIB_LIST = event

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk

# To test error paths and some state related to spinlocks spdk_lock.c includes thread/thread.c.
# To prevent redefined symbol errors, this test program needs to not link with libspdk_thread.
DEPDIRS-event := $(filter-out thread,$(DEPDIRS-event))
DEPDIRS-init := $(filter-out thread,$(DEPDIRS-init))
