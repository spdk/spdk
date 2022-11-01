#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2017 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

HEADERS := $(wildcard $(SPDK_ROOT_DIR)/include/spdk/*.h)
INSTALLED_HEADERS := $(patsubst $(SPDK_ROOT_DIR)/include%,$(DESTDIR)$(includedir)%,$(HEADERS))

$(INSTALLED_HEADERS):
ifeq ($(MAKECMDGOALS),install)
	$(INSTALL_HEADER)
else
	$(UNINSTALL_HEADER)
endif

.PHONY: $(INSTALLED_HEADERS)

all:
	$(Q)cp $(SPDK_ROOT_DIR)/include/spdk/*.h $(SPDK_ROOT_DIR)/build/include/spdk/

clean:
	@:

install: $(INSTALLED_HEADERS)

uninstall: $(INSTALLED_HEADERS)

include $(SPDK_ROOT_DIR)/mk/spdk.deps.mk
