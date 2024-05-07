#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

DIRS-y = error ioat
DIRS-$(CONFIG_DPDK_COMPRESSDEV) += dpdk_compressdev
DIRS-$(CONFIG_IDXD) += dsa
DIRS-$(CONFIG_IDXD) += iaa
DIRS-$(CONFIG_CRYPTO) += dpdk_cryptodev
ifeq ($(CONFIG_RDMA_PROV),mlx5_dv)
DIRS-y += mlx5
endif

.PHONY: all clean $(DIRS-y)

all: $(DIRS-y)
clean: $(DIRS-y)

include $(SPDK_ROOT_DIR)/mk/spdk.subdirs.mk
