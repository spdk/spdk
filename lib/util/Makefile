#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 5
SO_MINOR := 2

C_SRCS = base64.c bit_array.c cpuset.c crc16.c crc32.c crc32c.c crc32_ieee.c \
	 dif.c fd.c file.c hexlify.c iov.c math.c pipe.c strerror_tls.c string.c uuid.c \
	 fd_group.c xor.c zipf.c
LIBNAME = util
LOCAL_SYS_LIBS = -luuid

ifeq ($(CONFIG_ISAL), y)
LOCAL_SYS_LIBS += -L$(ISAL_DIR)/.libs -lisal
endif

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_util.map)

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
