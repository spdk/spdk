#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

DIRS-y += nvme_fuzz
DIRS-y += iscsi_fuzz

ifeq ($(CONFIG_FUZZER),y)
DIRS-y += llvm_nvme_fuzz
ifeq ($(CONFIG_VFIO_USER),y)
DIRS-y += llvm_vfio_fuzz
endif
endif

ifeq ($(OS),Linux)
DIRS-$(CONFIG_VIRTIO) += vhost_fuzz
endif
.PHONY: all clean $(DIRS-y)

all: $(DIRS-y)
clean: $(DIRS-y)

include $(SPDK_ROOT_DIR)/mk/spdk.subdirs.mk
