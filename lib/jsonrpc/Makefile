#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 4
SO_MINOR := 0

LIBNAME = jsonrpc
C_SRCS = jsonrpc_server.c jsonrpc_server_tcp.c
C_SRCS += jsonrpc_client.c jsonrpc_client_tcp.c

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_jsonrpc.map)

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
