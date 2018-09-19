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
COMBINED_LINKERNAME_LIB = $(SPDK_ROOT_DIR)/build/lib/libspdk.so
COMBINED_SHARED_LIB = $(COMBINED_LINKERNAME_LIB).$(SO_SUFFIX_ALL)

LIBS += $(BLOCKDEV_MODULES_LINKER_ARGS)
LIBS += $(COPY_MODULES_LINKER_ARGS)
LIBS += $(SOCK_MODULES_LINKER_ARGS)
LIBS += $(SPDK_LIB_LINKER_ARGS)
LIBS += $(ENV_LINKER_ARGS)

comma := ,

$(COMBINED_SHARED_LIB): $(SPDK_LIB_FILES) $(SPDK_WHOLE_LIBS) $(BLOCKDEV_MODULES_FILES) $(COPY_MODULES_FILES) $(SOCK_MODULES_FILES) $(LINKER_MODULES) $(ENV_LIBS) $(MAKEFILE_LIST) spdk.map
	$(Q)echo "  SO $(notdir $@)"; \
	rm -f $@; \
	$(call spdk_build_realname_shared_lib,$(filter-out -Wl$(comma)--no-whole-archive,$(LIBS)),spdk.map,$(SYS_LIBS))

$(COMBINED_LINKERNAME_LIB) : %.so : %.so.$(SO_SUFFIX_ALL)
	$(Q)echo "  SYMLINK $(notdir $@)"; $(BUILD_LINKERNAME_LIB)

.PHONY: all clean $(DIRS-y)

all: $(COMBINED_SHARED_LIB) $(COMBINED_LINKERNAME_LIB)

clean:
	$(CLEAN_C) $(COMBINED_SHARED_LIB) $(COMBINED_LINKERNAME_LIB)

install:
	$(call spdk_install_shared_libs, $(COMBINED_LINKERNAME_LIB))

include $(SPDK_ROOT_DIR)/mk/spdk.subdirs.mk
