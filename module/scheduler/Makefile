#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

DIRS-y = dynamic

# When DPDK rte_power is missing, do not compile schedulers
# and governors based on it.
ifeq (y,$(DPDK_POWER))
DIRS-y += dpdk_governor gscheduler
else
$(warning Skipping building dpdk_governor and gscheduler, due to missing rte_power)
endif

.PHONY: all clean $(DIRS-y)

all: $(DIRS-y)
clean: $(DIRS-y)

include $(SPDK_ROOT_DIR)/mk/spdk.subdirs.mk
