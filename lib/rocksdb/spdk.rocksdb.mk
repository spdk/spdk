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
