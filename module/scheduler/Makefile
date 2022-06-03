#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

DIRS-y = dynamic

# Do not compile schedulers and governors based on DPDK env
# if non-DPDK env is used.
ENV_NAME := $(notdir $(CONFIG_ENV))
ifeq ($(ENV_NAME),env_dpdk)
ifeq ($(OS),Linux)
DIRS-y += dpdk_governor gscheduler
endif
endif

.PHONY: all clean $(DIRS-y)

all: $(DIRS-y)
clean: $(DIRS-y)

include $(SPDK_ROOT_DIR)/mk/spdk.subdirs.mk
