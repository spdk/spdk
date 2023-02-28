#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)

APP = identify

include $(SPDK_ROOT_DIR)/mk/nvme.libtest.mk

install: $(APP)
	$(INSTALL_EXAMPLE)

uninstall:
	$(UNINSTALL_EXAMPLE)
