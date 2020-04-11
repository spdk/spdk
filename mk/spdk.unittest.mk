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
include $(SPDK_ROOT_DIR)/mk/spdk.app_vars.mk
include $(SPDK_ROOT_DIR)/mk/spdk.mock.unittest.mk

# We don't want to run scan-build against the unit tests
# because it can't understand our mock function macros and
# throws false positives because of them.

# Scan-build inserts a phony compiler by overriding the value
# of CC, so we store the original CC under DEFAULT_CC and
# re-assign it here.
override CC=$(DEFAULT_CC)

C_SRCS = $(TEST_FILE)

CFLAGS += -I$(SPDK_ROOT_DIR)/lib
CFLAGS += -I$(SPDK_ROOT_DIR)/module
CFLAGS += -I$(SPDK_ROOT_DIR)/test
CFLAGS += -ffunction-sections
LDFLAGS += -Wl,--gc-sections

SPDK_LIB_LIST += thread util log

LIBS += -lcunit $(SPDK_STATIC_LIB_LINKER_ARGS)

APP = $(TEST_FILE:.c=)

ifneq ($(UNIT_TEST_LINK_ENV),1)
ENV_LINKER_ARGS =
else
# Rewrite the env linker args to be static.
ENV_DPDK_FILE = $(call spdk_lib_list_to_static_libs,env_dpdk)
endif

install: all

all: $(APP)
	@:

$(APP) : $(OBJS) $(SPDK_LIB_FILES) $(ENV_LIBS)
	$(LINK_C)

clean:
	$(CLEAN_C) $(APP)

include $(SPDK_ROOT_DIR)/mk/spdk.deps.mk

uninstall:
	@:
