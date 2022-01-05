#
#  BSD LICENSE
#
#  Copyright (c) Intel Corporation.
#  All rights reserved.
#
#  Redistribution and use in source and binary forms, with or without
#  modification, are permitted provided that the following conditions
#  are met:
#
#    * Redistributions of source code must retain the above copyright
#      notice, this list of conditions and the following disclaimer.
#    * Redistributions in binary form must reproduce the above copyright
#      notice, this list of conditions and the following disclaimer in
#      the documentation and/or other materials provided with the
#      distribution.
#    * Neither the name of Intel Corporation nor the names of its
#      contributors may be used to endorse or promote products derived
#      from this software without specific prior written permission.
#
#  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
#  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
#  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
#  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
#  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
#  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
#  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
#  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
#  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
#  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
#  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
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
