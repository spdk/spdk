#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2019 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

.PHONY: all clean install uninstall

ifneq ($(Q),)
REDIRECT=> /dev/null
endif

# Force-disable scan-build
SUB_CC = $(patsubst %ccc-analyzer,$(DEFAULT_CC),$(CC))

all:
	$(Q)$(MAKE) -C $(SPDK_ROOT_DIR)/intel-ipsec-mb all SHARED=n EXTRA_CFLAGS=-fPIC CC=$(SUB_CC) $(REDIRECT)

install: all

uninstall:
	@:

clean:
	$(Q)$(MAKE) -C $(SPDK_ROOT_DIR)/intel-ipsec-mb clean SHARED=n $(REDIRECT)
