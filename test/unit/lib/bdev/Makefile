#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

DIRS-y = bdev.c part.c scsi_nvme.c gpt vbdev_lvol.c mt raid bdev_zone.c vbdev_zone_block.c nvme

DIRS-$(CONFIG_CRYPTO) += crypto.c

# enable once new mocks are added for compressdev
DIRS-$(CONFIG_VBDEV_COMPRESS) += compress.c

DIRS-$(CONFIG_PMDK) += pmem

.PHONY: all clean $(DIRS-y)

all: $(DIRS-y)
clean: $(DIRS-y)

include $(SPDK_ROOT_DIR)/mk/spdk.subdirs.mk
