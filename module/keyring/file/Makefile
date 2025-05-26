# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2024 Intel Corporation.  All rights reserved.

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 3
SO_MINOR := 0

C_SRCS = keyring.c keyring_rpc.c
LIBNAME = keyring_file

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_keyring_file.map)

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
