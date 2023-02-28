#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.lib_deps.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

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

PKGCONFIG = $(call pkgconfig_filename,spdk_$(LIBNAME))
PKGCONFIG_INST = $(call pkgconfig_filename,tmp/spdk_$(LIBNAME))

ifeq ($(CONFIG_SHARED),y)
DEP := $(SHARED_LINKED_LIB)
else
DEP := $(LIB)
endif

DEP += $(PKGCONFIG) ${PKGCONFIG_INST}

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

ifeq ($(CXX_SRCS),)
COMPILER=$(CC)
else
COMPILER=$(CXX)
endif

MODULES-bdev = spdk_bdev_modules
MODULES-sock = spdk_sock_modules
MODULES-accel = spdk_accel_modules
MODULES-scheduler = spdk_scheduler_modules
ifeq ($(SPDK_ROOT_DIR)/lib/env_dpdk,$(CONFIG_ENV))
MODULES-event = spdk_env_dpdk_rpc
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
	$(call spdk_build_realname_shared_lib,$(COMPILER),$^,$(SPDK_MAP_FILE),$(LOCAL_SYS_LIBS),$(SPDK_DEP_LIBS))

define pkgconfig_create
	$(Q)$(SPDK_ROOT_DIR)/scripts/pc.sh $(1) $(2) $(LIBNAME) $(SO_SUFFIX) \
		"$(DEPDIRS-$(LIBNAME):%=spdk_%) $(MODULES-$(LIBNAME))" \
		"" > $@
endef

$(PKGCONFIG): $(LIB)
	$(call pkgconfig_create,$(SPDK_ROOT_DIR)/build "")

$(PKGCONFIG_INST): $(LIB)
	$(call pkgconfig_create,$(CONFIG_PREFIX),$(libdir))

$(LIB): $(OBJS)
	$(LIB_C)

install: all
	$(INSTALL_LIB)
	@$(call pkgconfig_install,$(PKGCONFIG_INST))
ifeq ($(CONFIG_SHARED),y)
	$(INSTALL_SHARED_LIB)
endif

uninstall: $(DIRS-y)
	$(UNINSTALL_LIB)
	@$(call pkgconfig_uninstall,$(PKGCONFIG_INST))
ifeq ($(CONFIG_SHARED),y)
	$(UNINSTALL_SHARED_LIB)
endif

include $(SPDK_ROOT_DIR)/mk/spdk.deps.mk
