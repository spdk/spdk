#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2017 Intel Corporation.
#  All rights reserved.
#

# This snippet will be included into the RocksDB Makefile

include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

CXXFLAGS +=  -I$(SPDK_DIR)/include -Iinclude/

# The SPDK makefiles turn this on, but RocksDB won't compile with it.  So
#  turn it off after including the SPDK makefiles.
CXXFLAGS += -Wno-missing-declarations -Wno-maybe-uninitialized

# The SPDK Makefiles may turn these options on but we do not want to enable
#  them for the RocksDB source files.
CXXFLAGS += -fno-profile-arcs -fno-test-coverage
ifeq ($(CONFIG_UBSAN),y)
CXXFLAGS += -fno-sanitize=undefined
endif
ifeq ($(CONFIG_ASAN),y)
CXXFLAGS += -fno-sanitize=address
endif

SPDK_LIB_LIST = $(ALL_MODULES_LIST) event_bdev event

AM_LINK += $(SPDK_LIB_LINKER_ARGS) $(ENV_LINKER_ARGS)
AM_LINK += $(SYS_LIBS)

ifeq ($(CONFIG_UBSAN),y)
AM_LINK += -fsanitize=undefined
endif

ifeq ($(CONFIG_COVERAGE),y)
AM_LINK += -fprofile-arcs -ftest-coverage
endif
