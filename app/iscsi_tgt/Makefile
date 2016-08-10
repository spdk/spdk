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

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

APP = iscsi_tgt

CFLAGS += $(DPDK_INC)

# Add iSCSI library directory to include path
# TODO: remove this once iSCSI has a public API header
CFLAGS += -I$(SPDK_ROOT_DIR)/lib

C_SRCS := iscsi_tgt.c

SPDK_LIBS = \
	$(SPDK_ROOT_DIR)/lib/json/libspdk_json.a \
	$(SPDK_ROOT_DIR)/lib/jsonrpc/libspdk_jsonrpc.a \
	$(SPDK_ROOT_DIR)/lib/rpc/libspdk_rpc.a \
	$(SPDK_ROOT_DIR)/lib/bdev/libspdk_bdev.a \
	$(SPDK_ROOT_DIR)/lib/iscsi/libspdk_iscsi.a \
	$(SPDK_ROOT_DIR)/lib/scsi/libspdk_scsi.a \
	$(SPDK_ROOT_DIR)/lib/net/libspdk_net.a \
	$(SPDK_ROOT_DIR)/lib/copy/libspdk_copy.a \
	$(SPDK_ROOT_DIR)/lib/trace/libspdk_trace.a \
	$(SPDK_ROOT_DIR)/lib/conf/libspdk_conf.a \
	$(SPDK_ROOT_DIR)/lib/util/libspdk_util.a \
	$(SPDK_ROOT_DIR)/lib/env/libspdk_env.a \
	$(SPDK_ROOT_DIR)/lib/log/libspdk_log.a \
	$(SPDK_ROOT_DIR)/lib/log/rpc/libspdk_log_rpc.a \
	$(SPDK_ROOT_DIR)/lib/event/libspdk_event.a \
	$(SPDK_ROOT_DIR)/lib/event/rpc/libspdk_app_rpc.a \

LIBS += -Wl,--whole-archive $(SPDK_LIBS) -Wl,--no-whole-archive
LIBS += -lcrypto $(DPDK_LIB)
LIBS += $(BLOCKDEV_MODULES_LINKER_ARGS) \
	$(COPY_MODULES_LINKER_ARGS)

all : $(APP)

$(APP) : $(OBJS) $(SPDK_LIBS)
	$(LINK_C)

clean :
	$(CLEAN_C) $(APP)

include $(SPDK_ROOT_DIR)/mk/spdk.deps.mk
