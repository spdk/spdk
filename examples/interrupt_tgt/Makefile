#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2020 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

APP = interrupt_tgt

C_SRCS := interrupt_tgt.c

SPDK_LIB_LIST = $(INTR_BLOCKDEV_MODULES_LIST) event event_bdev conf

SPDK_LIB_LIST += event_nbd
SPDK_LIB_LIST += event_vhost_blk event_vhost_scsi

ifeq ($(SPDK_ROOT_DIR)/lib/env_dpdk,$(CONFIG_ENV))
SPDK_LIB_LIST += env_dpdk_rpc
endif

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
