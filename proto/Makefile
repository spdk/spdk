#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

# Location where the python generated code is placed
pydir=$(SPDK_ROOT_DIR)/python/spdk/sma/proto
protodefs = $(wildcard *.proto)
protopy = $(foreach proto,$(basename $(protodefs)),$(addprefix $(pydir)/$(proto),_pb2.py _pb2_grpc.py))

all: $(protopy)

clean:
	$(Q)$(RM) $(protopy)

# TODO: we should probably write a proper install rule here instead of just blindly copying all
# python scripts when building the RPMs
install:
uninstall:

$(pydir)/%_pb2.py $(pydir)/%_pb2_grpc.py: %.proto
	$(Q)python3 -m grpc_tools.protoc --proto_path=. --python_out=$(pydir) \
		--grpc_python_out=$(pydir)  $^

.PHONY: all clean install uninstall
