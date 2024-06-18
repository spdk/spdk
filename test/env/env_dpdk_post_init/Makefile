#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)

APP = env_dpdk_post_init

include $(SPDK_ROOT_DIR)/mk/nvme.libtest.mk

CFLAGS += $(ENV_CFLAGS)
