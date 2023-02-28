#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#  Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

DIRS-y = poller_perf

# spdk_lock.c includes thread.c, which causes problems when registering the same
# tracepoint for "thread" in the program and shared library. It is sufficient
# to test this only on static builds.
ifneq ($(CONFIG_SHARED),y)
DIRS-y += lock
endif

.PHONY: all clean $(DIRS-y)

all: $(DIRS-y)
clean: $(DIRS-y)

include $(SPDK_ROOT_DIR)/mk/spdk.subdirs.mk
