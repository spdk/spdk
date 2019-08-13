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

DIRS-y += bdev blob blobfs conf copy event json jsonrpc \
          log log_rpc lvol net rpc sock thread trace util nvme vmd nvmf scsi \
          ioat ut_mock iscsi notify
ifeq ($(OS),Linux)
DIRS-y += nbd ftl
endif

DIRS-$(CONFIG_VHOST) += vhost
DIRS-$(CONFIG_VIRTIO) += virtio
DIRS-$(CONFIG_REDUCE) += reduce

# If CONFIG_ENV is pointing at a directory in lib, build it.
# Out-of-tree env implementations must be built separately by the user.
ENV_NAME := $(notdir $(CONFIG_ENV))
ifeq ($(abspath $(CONFIG_ENV)),$(SPDK_ROOT_DIR)/lib/$(ENV_NAME))
DIRS-y += $(ENV_NAME)
endif

# Phase 1 dependencies.
# Most other modules depend on some combination of these 7 base modules.
DEPDIRS-log :=
DEPDIRS-util := log
DEPDIRS-conf := log util
DEPDIRS-thread := log util
DEPDIRS-json := log util
DEPDIRS-jsonrpc := log util json
DEPDIRS-rpc := log util json jsonrpc

# Phase 2 dependencies.
# Functional level libraries.
DEPDIRS-bdev := conf json log notify thread trace util
DEPDIRS-blob := log util
DEPDIRS-copy := thread
DEPDIRS-event := conf json jsonrpc log rpc thread trace util
DEPDIRS-ioat := log
DEPDIRS-log_rpc := log json jsonrpc rpc
DEPDIRS-net := log json jsonrpc rpc util
DEPDIRS-notify := log util
DEPDIRS-reduce := log util
DEPDIRS-sock := log
DEPDIRS-trace := log json jsonrpc rpc util
DEPDIRS-virtio := log json thread util
DEPDIRS-vmd := log

# Phase 3 dependencies.
DEPDIRS-blobfs := blob conf log thread trace
DEPDIRS-ftl := bdev log nvme thread trace util
DEPDIRS-iscsi := conf event json jsonrpc log rpc sock thread trace util
DEPDIRS-lvol := blob log util
DEPDIRS-nbd := bdev json jsonrpc log rpc thread util
DEPDIRS-nvme := log sock util
DEPDIRS-nvmf := bdev json log nvme sock thread trace util
DEPDIRS-scsi := bdev json jsonrpc log rpc thread trace util
DEPDIRS-vhost := bdev conf event jsonrpc json log rpc scsi thread util

.PHONY: all clean $(DIRS-y)

all: $(DIRS-y)
clean: $(DIRS-y)

include $(SPDK_ROOT_DIR)/mk/spdk.subdirs.mk
