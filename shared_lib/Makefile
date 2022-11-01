#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2018 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

SO_VER := 4
SO_MINOR := 0
SO_SUFFIX := $(SO_VER).$(SO_MINOR)

# Build combined libspdk.so shared library
SHARED_LINKED_LIB := $(SPDK_ROOT_DIR)/build/lib/libspdk.so
SHARED_REALNAME_LIB := $(SHARED_LINKED_LIB).$(SO_SUFFIX)
IGNORED_LIBS := libspdk.so
IGNORED_LIBS += libspdk_ut_mock.so
IGNORED_LIBS += libspdk_env_dpdk.so

SPDK_SHARED_LIBS := $(filter-out $(IGNORED_LIBS), $(sort $(notdir $(wildcard $(SPDK_ROOT_DIR)/build/lib/*.so))))

$(SHARED_REALNAME_LIB):
	$(Q)echo "  SO $(notdir $@)"; \
	echo "GROUP ( $(SPDK_SHARED_LIBS) )" > $(SHARED_REALNAME_LIB)

$(SHARED_LINKED_LIB) : $(SHARED_REALNAME_LIB)
	$(Q)echo "  SYMLINK $(notdir $@)"; $(BUILD_LINKERNAME_LIB)

.PHONY: all clean $(DIRS-y) install uninstall

all: $(SHARED_LINKED_LIB)

CLEAN_FILES += $(SHARED_REALNAME_LIB) $(SHARED_LINKED_LIB)

install:
	$(INSTALL_SHARED_LIB)

uninstall:
	$(UNINSTALL_SHARED_LIB)

include $(SPDK_ROOT_DIR)/mk/spdk.subdirs.mk
