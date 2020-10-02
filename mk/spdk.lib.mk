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
include $(SPDK_ROOT_DIR)/mk/spdk.lib_deps.mk


ifeq ($(SPDK_MAP_FILE),)
$(error SPDK_MAP_FILE is not set for lib $(LIBNAME))
endif

ifeq ($(SO_VER),)
$(error SO major version is not set for lib $(LIBNAME))
endif

ifeq ($(SO_MINOR),)
$(error SO minor version is not set for lib $(LIBNAME))
endif


SO_SUFFIX := $(SO_VER).$(SO_MINOR)
LIB := $(call spdk_lib_list_to_static_libs,$(LIBNAME))
SHARED_LINKED_LIB := $(LIB:.a=.so)
SHARED_REALNAME_LIB := $(SHARED_LINKED_LIB:.so=.so.$(SO_SUFFIX))

ifeq ($(CONFIG_SHARED),y)
DEP := $(SHARED_LINKED_LIB)
else
DEP := $(LIB)
endif

ifeq ($(OS),FreeBSD)
LOCAL_SYS_LIBS += -L/usr/local/lib
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

ifeq ($(SPDK_NO_LIB_DEPS),)
SPDK_DEP_LIBS = $(call spdk_lib_list_to_shared_libs,$(DEPDIRS-$(LIBNAME)))
endif

.PHONY: all clean $(DIRS-y)

all: $(BUILD_DEP)
	@:

clean: $(DIRS-y)
	$(CLEAN_C) $(LIB) $(SHARED_LINKED_LIB) $(SHARED_REALNAME_LIB)

$(SHARED_LINKED_LIB): $(SHARED_REALNAME_LIB)
	$(Q)echo "  SYMLINK $(notdir $@)"; $(BUILD_LINKERNAME_LIB)

$(SHARED_REALNAME_LIB): $(LIB)
	$(Q)echo "  SO $(notdir $@)"; \
	$(call spdk_build_realname_shared_lib,$^,$(SPDK_MAP_FILE),$(LOCAL_SYS_LIBS),$(SPDK_DEP_LIBS))

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

include $(SPDK_ROOT_DIR)/mk/spdk.deps.mk
