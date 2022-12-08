#
#  BSD LICENSE
#
#  Copyright © 2022 NVIDIA CORPORATION & AFFILIATES. ALL RIGHTS RESERVED.
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

SNAP_LIB_DIR :=/usr/local/snap/lib

SNAP_INC_DIR := $(SPDK_ROOT_DIR)/snap-rdma/src
SNAP_CTL_INC_DIR :=$(SPDK_ROOT_DIR)/snap-rdma/ctl
SNAP_RPC_INC_DIR :=$(SPDK_ROOT_DIR)/snap-rdma/rpc
SNAP_VRDMA_INC_DIR :=$(SPDK_ROOT_DIR)/snap-rdma/vrdma
USR_INCLUDE := /usr/include

CFLAGS += $(ENV_CFLAGS)
CFLAGS += -I$(SNAP_INC_DIR) -I$(SNAP_CTL_INC_DIR) -I$(SNAP_RPC_INC_DIR) -I$(SNAP_VRDMA_INC_DIR) -I$(USR_INCLUDE)
C_SRCS = vrdma_dpa.c vrdma_dpa_mm.c vrdma_dpa_vq.c

LIBNAME = dpa
LOCAL_SYS_LIBS = -libverbs -lmlx5 -L$(SNAP_LIB_DIR) -lsnap-dma -lsnap -lsnap-mr -lsnap-env -lsnap-vrdma -lsnap-virtio-blk-ctrl

#include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
#---------below expand 
# include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
# include $(SPDK_ROOT_DIR)/mk/spdk.lib_deps.mk

SPDK_MAP_FILE = $(SPDK_ROOT_DIR)/shared_lib/spdk.map
LIB := $(call spdk_lib_list_to_static_libs,$(LIBNAME))


SHARED_LINKED_LIB := $(subst .a,.so,$(LIB))
SHARED_REALNAME_LIB := $(subst .so,.so.$(SO_SUFFIX_ALL),$(SHARED_LINKED_LIB))

ifeq ($(CONFIG_SHARED),y)
DEP := $(SHARED_LINKED_LIB)
else
DEP := $(LIB)
endif


ifeq ($(OS),FreeBSD)
LOCAL_SYS_LIBS += -L/usr/local/lib -lrt
else
LOCAL_SYS_LIBS += -lrt
endif

define subdirs_rule
$(1): $(2)
	@+$(Q)$(MAKE) -C $(1) S=$S$(S:%=/)$@ $(MAKECMDGOALS)
endef
$(foreach dir,$(DIRS-y),$(eval $(call subdirs_rule,$(dir),$(DEP))))

ifneq ($(DIRS-y),)
BUILD_DEP := $(DIRS-y)
else
BUILD_DEP := $(DEP)
endif
SPDK_DEP_LIBS = $(call spdk_lib_list_to_shared_libs,$(DEPDIRS-$(LIBNAME)))
.PHONY: all clean $(DIRS-y)

all: $(BUILD_DEP)
	@:
clean: $(DIRS-y)
	$(CLEAN_C) $(LIB) $(SHARED_LINKED_LIB) $(SHARED_REALNAME_LIB)

$(SHARED_LINKED_LIB): $(SHARED_REALNAME_LIB)
	$(Q)echo "  SYMLINK $(notdir $@)"; $(BUILD_LINKERNAME_LIB)

$(SHARED_REALNAME_LIB): $(LIB)
	$(Q)echo "  SO $(notdir $@)"; \
	$(call spdk_build_realname_shared_lib,$^,$(SPDK_MAP_FILE),$(LOCAL_SYS_LIBS) $(SPDK_DEP_LIBS))

$(LIB): $(OBJS)
	$(LIB_C)
install: all
	$(INSTALL_LIB)
ifeq ($(CONFIG_SHARED),y)
	$(INSTALL_SHARED_LIB)
endif
uninstall: $(DIRS-y)
	$(UNINSTALL_LIB)
ifeq ($(CONFIG_SHARED),y)
	$(UNINSTALL_SHARED_LIB)
endif

# include $(SPDK_ROOT_DIR)/mk/spdk.deps.mk
