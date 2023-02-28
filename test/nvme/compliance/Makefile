#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)

APP = nvme_compliance

# spdk_cunit.h
CFLAGS += -I$(SPDK_ROOT_DIR)/test
LIBS += -lcunit

include $(SPDK_ROOT_DIR)/mk/nvme.libtest.mk
