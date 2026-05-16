#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 12
SO_MINOR := 0

C_SRCS = base64.c bit_array.c cpuset.c crc16.c crc32.c crc32c.c crc32_ieee.c crc64.c \
	 dif.c fd.c fd_group.c file.c hexlify.c iov.c math.c net.c \
	 pipe.c strerror_tls.c string.c uuid.c xor.c zipf.c md5.c
LIBNAME = util

ifeq ($(CONFIG_HAVE_LIBUUID),y)
LOCAL_SYS_LIBS = -luuid
endif

ifeq ($(CONFIG_HAVE_UUID_GENERATE_SHA1), n)
LOCAL_SYS_LIBS += -lssl
endif

ifeq ($(CONFIG_ISAL),y)
LOCAL_SYS_LIBS += -L$(ISAL_DIR)/.libs -L$(ISAL_DIR)/lib64 -lisal
endif


CFLAGS += -Wpointer-arith

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_util.map)

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
