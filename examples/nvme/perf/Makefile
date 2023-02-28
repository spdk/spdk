#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)

APP = perf

include $(SPDK_ROOT_DIR)/mk/nvme.libtest.mk

ifeq ($(OS),Linux)
SYS_LIBS += -laio
CFLAGS += -DHAVE_LIBAIO
endif

install: $(APP)
	$(INSTALL_EXAMPLE)

uninstall:
	$(UNINSTALL_EXAMPLE)
