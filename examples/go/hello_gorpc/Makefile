#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2023 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

.PHONY: all clean

BUILDFLAGS=-trimpath -mod=readonly -gcflags="all=-spectre=all -N -l" -asmflags="all=-spectre=all" -ldflags="all=-s -w"

all:
	$(Q)go build $(BUILDFLAGS) -o $(SPDK_ROOT_DIR)/build/examples/hello_gorpc

clean:
	$(Q)rm -rf $(SPDK_ROOT_DIR)/build/examples/hello_gorpc
