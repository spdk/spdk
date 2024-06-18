#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2019 Intel Corporation
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

APP = vhost_fuzz

CFLAGS += -I$(SPDK_ROOT_DIR)/test/app/fuzz/common

C_SRCS := vhost_fuzz_rpc.c vhost_fuzz.c

SPDK_LIB_LIST += event conf rpc sock virtio
include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
