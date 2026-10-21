#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../../../..)

SPDK_LIB_LIST = json
TEST_FILE = vfio_user_ut.c

include $(SPDK_ROOT_DIR)/mk/spdk.unittest.mk

# This test compiles vfio_user.c directly instead of linking libspdk_nvmf.
ifeq ($(CONFIG_SHARED),n)
$(APP): $(VFIO_USER_LIBRARY_DIR)/libvfio-user.a
endif
