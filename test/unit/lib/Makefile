#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

DIRS-y =  accel bdev blob blobfs dma event ioat iscsi json jsonrpc log lvol
DIRS-y += notify nvme nvmf scsi sock thread util env_dpdk init rpc
DIRS-$(CONFIG_IDXD) += idxd
DIRS-$(CONFIG_REDUCE) += reduce
ifeq ($(OS),Linux)
DIRS-$(CONFIG_VHOST) += vhost
endif

.PHONY: all clean $(DIRS-y)

all: $(DIRS-y)
clean: $(DIRS-y)

include $(SPDK_ROOT_DIR)/mk/spdk.subdirs.mk
