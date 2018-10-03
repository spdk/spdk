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
include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

# Build combined libspdk.so shared library
SHARED_LINKED_LIB := $(SPDK_ROOT_DIR)/build/lib/libspdk.so
SHARED_REALNAME_LIB := $(SHARED_LINKED_LIB).$(SO_SUFFIX_ALL)
IGNORED_LIBS := libspdk.so
IGNORED_LIBS += libspdk_spdk_mock.so
IGNORED_LIBS += libspdk_env_dpdk.so

SPDK_SHARED_LIBS := $(filter-out $(IGNORED_LIBS), $(sort $(notdir $(wildcard $(SPDK_ROOT_DIR)/build/lib/*.so))))

$(SHARED_REALNAME_LIB):
	$(Q)echo "  SO $(notdir $@)"; \
	echo "GROUP ( AS_NEEDED ( $(SPDK_SHARED_LIBS) ) )" > $(SHARED_REALNAME_LIB)

$(SHARED_LINKED_LIB) : $(SHARED_REALNAME_LIB)
	$(Q)echo "  SYMLINK $(notdir $@)"; $(BUILD_LINKERNAME_LIB)

.PHONY: all clean $(DIRS-y)

all: $(SHARED_LINKED_LIB)

clean:
	$(CLEAN_C) $(SHARED_REALNAME_LIB) $(SHARED_LINKED_LIB)

install:
	$(INSTALL_SHARED_LIB)

include $(SPDK_ROOT_DIR)/mk/spdk.subdirs.mk
