#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
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
CFLAGS += -DSPDK_UNIT_TEST=1
LDFLAGS += -Wl,--gc-sections

SPDK_LIB_LIST += thread trace util log

LIBS += -lcunit $(SPDK_STATIC_LIB_LINKER_ARGS)

APP = $(TEST_FILE:.c=)$(EXEEXT)

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
