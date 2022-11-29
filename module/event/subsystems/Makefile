#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

DIRS-y += bdev accel scheduler iscsi nvmf scsi vmd sock iobuf

ifeq ($(OS),Linux)
DIRS-y += nbd
ifeq ($(CONFIG_UBLK),y)
DIRS-y += ublk
endif
endif

DIRS-$(CONFIG_VHOST) += vhost_blk vhost_scsi
DIRS-$(CONFIG_VFIO_USER) += vfu_tgt

# These dependencies are not based specifically on symbols, but rather
# the subsystem dependency tree defined within the event subsystem C files
# themselves. Should that tree change, these dependencies should change
# accordingly.
DEPDIRS-accel := iobuf
DEPDIRS-bdev := accel vmd sock iobuf
DEPDIRS-iscsi := scsi
DEPDIRS-nbd := bdev
DEPDIRS-ublk := bdev
DEPDIRS-nvmf := bdev
DEPDIRS-scsi := bdev
DEPDIRS-vhost_scsi := scsi

.PHONY: all clean $(DIRS-y)

all: $(DIRS-y)
clean: $(DIRS-y)

include $(SPDK_ROOT_DIR)/mk/spdk.subdirs.mk
