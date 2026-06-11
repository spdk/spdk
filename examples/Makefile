#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

# See lib/Makefile for the CONFIG_AMD_NTB default (y); the example uses the
# same knob so 'make CONFIG_AMD_NTB=n' disables both the library and its app
# in lockstep.
CONFIG_AMD_NTB ?= y

DIRS-y += accel bdev blob ioat nvme sock nvmf util thread
DIRS-$(CONFIG_IDXD) += idxd
DIRS-$(CONFIG_GOLANG) += go
DIRS-$(CONFIG_FSDEV) += fsdev
DIRS-$(CONFIG_VMD) += vmd

ifeq ($(OS),Linux)
DIRS-$(CONFIG_VHOST) += interrupt_tgt
# The NTB perf example links against lib/ntb_amd, which is gated to Linux.
# Keep them in lockstep so 'make' on non-Linux hosts doesn't try to build it.
DIRS-$(CONFIG_AMD_NTB) += ntb
endif

.PHONY: all clean $(DIRS-y)

all: $(DIRS-y)
clean: $(DIRS-y)

include $(SPDK_ROOT_DIR)/mk/spdk.subdirs.mk
