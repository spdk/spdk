#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

NVME_DIR := $(SPDK_ROOT_DIR)/lib/nvme

include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

C_SRCS := $(APP:%=%.c)

SPDK_LIB_LIST += $(SOCK_MODULES_LIST) nvme
ifeq ($(CONFIG_VMD),y)
SPDK_LIB_LIST += vmd
endif

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
