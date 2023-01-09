#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#  Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

#  Make targets are based on the current value of $(CUNIT_VERSION),
#  rather than on configuration values. All sub-directories are
#  added to $(DIRS-y) so that they are included in 'make clean'.
#  $(ALL_DIRS) contains the list of sub-directories to compile.
DIRS-y = blob.c blob_bdev.c
ALL_DIRS = blob_bdev.c

HASH = \#
CUNIT_VERSION = $(shell echo "$(HASH)include <CUnit/CUnit.h>" | $(CC) $(CFLAGS) -E -dM - | sed -n -e 's/\#define CU_VERSION "\([0-9\.\-]*\).*/\1/p')
ifeq ($(CUNIT_VERSION),2.1-3)
ALL_DIRS += blob.c
else
$(warning "blob_ut.c compilation skipped, only CUnit version 2.1-3 is supported")
endif

.PHONY: all clean $(DIRS-y)

all: $(ALL_DIRS)
clean: $(DIRS-y)

include $(SPDK_ROOT_DIR)/mk/spdk.subdirs.mk
