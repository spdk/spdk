#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  Copyright (c) 2020 Mellanox Technologies LTD
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 7
SO_MINOR := 0

C_SRCS = sock.c sock_rpc.c

LIBNAME = sock

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_sock.map)

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
