#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

DIRS-y += accel bdev blob ioat nvme sock vmd nvmf util thread
DIRS-$(CONFIG_IDXD) += idxd
DIRS-$(CONFIG_GOLANG) += go
DIRS-$(CONFIG_FSDEV) += fsdev

ifeq ($(OS),Linux)
DIRS-$(CONFIG_VHOST) += interrupt_tgt
endif

.PHONY: all clean $(DIRS-y)

all: $(DIRS-y)
clean: $(DIRS-y)

include $(SPDK_ROOT_DIR)/mk/spdk.subdirs.mk
