#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

HEADERS := $(wildcard $(SPDK_ROOT_DIR)/include/spdk/*.h)

# On Linux, queue_extras.h is pulled in with queue.h,
# on FreeBSD, we want to ignore queue_extras.h entirely.
HEADERS := $(filter-out $(SPDK_ROOT_DIR)/include/spdk/queue_extras.h,$(HEADERS))
HEADERS := $(filter-out $(SPDK_ROOT_DIR)/include/spdk/vfu_target.h,$(HEADERS))
CXX_SRCS := $(patsubst %.h,%.cpp,$(notdir $(HEADERS)))

install : all

%.cpp: $(SPDK_ROOT_DIR)/include/spdk/%.h
	$(Q)echo "  TEST_HEADER include/spdk/$(notdir $<)"; \
	echo '#include "spdk/$(notdir $<)"' > $@

all : $(CXX_SRCS) $(OBJS)
	@:

clean :
	$(CLEAN_C) $(CXX_SRCS)

include $(SPDK_ROOT_DIR)/mk/spdk.deps.mk

uninstall:
	@:
