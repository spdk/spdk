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
include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

APP = spdk_tgt

C_SRCS := spdk_tgt.c

ifeq ($(OS),Linux)
SPDK_LIB_LIST += event_nbd nbd

ifeq ($(CONFIG_VHOST),y)
SPDK_LIB_LIST += event_vhost vhost rte_vhost
endif

SPDK_LIB_LIST = event_bdev event_copy event_iscsi event_net event_scsi event_nvmf event
SPDK_LIB_LIST += nvmf trace log conf thread util bdev iscsi scsi copy rpc jsonrpc json
SPDK_LIB_LIST += app_rpc log_rpc bdev_rpc net

endif

LIBS += $(BLOCKDEV_MODULES_LINKER_ARGS) \
	$(COPY_MODULES_LINKER_ARGS) \
	$(SOCK_MODULES_LINKER_ARGS) \
	$(SPDK_LIB_LINKER_ARGS) $(ENV_LINKER_ARGS)

all: $(APP)
	@:

$(APP): $(OBJS) $(SPDK_LIB_FILES) $(SPDK_WHOLE_LIBS) $(BLOCKDEV_MODULES_FILES) $(COPY_MODULES_FILES) $(SOCK_MODULES_FILES) $(LINKER_MODULES) $(ENV_LIBS)
	$(LINK_C)

clean:
	$(CLEAN_C) $(APP)

install: $(APP)
	$(INSTALL_APP)

include $(SPDK_ROOT_DIR)/mk/spdk.deps.mk
