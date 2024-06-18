#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

DIRS-y += hello_world bdevperf

.PHONY: all clean $(DIRS-y)

all: $(DIRS-y)
	@:

clean: $(DIRS-y)
	@:

include $(SPDK_ROOT_DIR)/mk/spdk.subdirs.mk
