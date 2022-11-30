#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#  Copyright (c) 2022 Dell Inc, or its subsidiaries.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 5
SO_MINOR := 0

C_SRCS = bdev_nvme.c bdev_nvme_rpc.c nvme_rpc.c bdev_mdns_client.c
C_SRCS-$(CONFIG_NVME_CUSE) += bdev_nvme_cuse_rpc.c

ifeq ($(OS),Linux)
C_SRCS += vbdev_opal.c vbdev_opal_rpc.c
endif
LIBNAME = bdev_nvme

SPDK_MAP_FILE = $(SPDK_ROOT_DIR)/mk/spdk_blank.map

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
