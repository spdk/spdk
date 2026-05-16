#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 18
SO_MINOR := 0
SO_SUFFIX := $(SO_VER).$(SO_MINOR)

LIBNAME = accel
C_SRCS = accel.c accel_rpc.c accel_sw.c

ifeq ($(CONFIG_HAVE_LZ4),y)
LOCAL_SYS_LIBS += -llz4
endif

ifeq ($(CONFIG_ISAL_CRYPTO),y)
LOCAL_SYS_LIBS += -L$(ISAL_CRYPTO_DIR/.libs -L$(ISAL_CRYPTO_DIR)/lib64 -lisal_crypto
endif

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_accel.map)

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
