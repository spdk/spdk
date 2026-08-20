#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright Hewlett Packard Enterprise Development LP
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../../../..)

TEST_FILE = aio_ut.c

SPDK_MOCK_SYMBOLS += ioctl fallocate

include $(SPDK_ROOT_DIR)/mk/spdk.unittest.mk

LIBS += -laio
