#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2019 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

.PHONY: all clean install

ifneq ($(Q),)
REDIRECT=> /dev/null
endif

# Force-disable scan-build
SUB_CC = $(patsubst %ccc-analyzer,$(DEFAULT_CC),$(CC))

$(ISAL_DIR)/isa-l:
	@ln -s $(ISAL_DIR)/include $(ISAL_DIR)/isa-l

all: $(ISAL_DIR)/isa-l
	$(Q)$(MAKE) -C $(SPDK_ROOT_DIR)/isa-l all CC="$(SUB_CC)" $(REDIRECT)

install: all

uninstall:
	@:

clean:
	$(Q)$(MAKE) -C $(SPDK_ROOT_DIR)/isa-l clean $(REDIRECT)
	$(Q)rm -rf $(ISAL_DIR)/isa-l
