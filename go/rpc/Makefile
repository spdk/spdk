#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2023 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
CLIENT_BUILD_DIR = $(SPDK_ROOT_DIR)/build/go/rpc
CLIENT_SRC := $(wildcard $(CURDIR)/*.go $(CURDIR)/client/*.go)

.PHONY: all clean mock-generate

CGOFLAGS=-trimpath -mod=readonly -gcflags="all=-spectre=all -N -l" -asmflags="all=-spectre=all" -buildmode=c-shared -ldflags="all=-s -w"

all: $(CLIENT_SRC)
	$(Q)go build $(CGOFLAGS) -o $(CLIENT_BUILD_DIR)/libspdk_gorpc.so clientIntegration.go

clean:
	$(Q)rm -rf $(CLIENT_BUILD_DIR)

mock-generate:
	$(Q)mockery --name=IClient --dir client --output mocks --with-expecter --boilerplate-file mocks/boilerplate.txt
