#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

define generate_protobuf
$(1)/%_pb2.py $(1)/%_pb2_grpc.py: $(1)/%.proto
	$(Q)python3 -m grpc_tools.protoc -I $(1) --python_out=$(1) \
		--grpc_python_out=$(1) --proto_path=$(1) $$^
endef

ifeq ($(CONFIG_SMA),y)
protodirs = spdk/sma/proto
protodefs = $(foreach protodir,$(protodirs),$(wildcard $(protodir)/*.proto))
protopy = $(foreach proto,$(basename $(protodefs)),$(addprefix $(proto),_pb2.py _pb2_grpc.py))
endif

all: $(protopy)

clean:
	$(Q)$(RM) $(protopy)

# TODO: we should probably write a proper install rule here instead of just blindly copying all
# python scripts when building the RPMs
install:
uninstall:

$(foreach protodir,$(protodirs),$(eval $(call generate_protobuf,$(protodir))))

.PHONY: all clean install uninstall
