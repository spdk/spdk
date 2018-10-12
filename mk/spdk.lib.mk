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

include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

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


.PHONY: all clean $(DIRS-y)

all: $(DEP) $(DIRS-y)
	@:

clean: $(DIRS-y)
	$(CLEAN_C) $(LIB) $(SHARED_LINKED_LIB) $(SHARED_REALNAME_LIB)

$(SHARED_LINKED_LIB): $(SHARED_REALNAME_LIB)
	$(Q)echo "  SYMLINK $(notdir $@)"; $(BUILD_LINKERNAME_LIB)

$(SHARED_REALNAME_LIB): $(LIB)
	$(Q)echo "  SO $(notdir $@)"; \
	$(call spdk_build_realname_shared_lib,$^,$(SPDK_MAP_FILE),$(LOCAL_SYS_LIBS))

$(LIB): $(OBJS)
	$(LIB_C)

install: all
	$(INSTALL_LIB)
ifeq ($(CONFIG_SHARED),y)
	$(INSTALL_SHARED_LIB)
endif

include $(SPDK_ROOT_DIR)/mk/spdk.deps.mk

include $(SPDK_ROOT_DIR)/mk/spdk.subdirs.mk
