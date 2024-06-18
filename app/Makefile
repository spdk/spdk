#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

DIRS-y += trace
DIRS-y += trace_record
DIRS-y += nvmf_tgt
DIRS-y += iscsi_tgt
DIRS-y += spdk_tgt
DIRS-y += spdk_lspci
DIRS-y += spdk_nvme_perf
DIRS-y += spdk_nvme_identify
DIRS-y += spdk_nvme_discover
ifneq ($(OS),Windows)
# TODO - currently disabled on Windows due to lack of support for curses
DIRS-y += spdk_top
endif
ifeq ($(OS),Linux)
DIRS-$(CONFIG_VHOST) += vhost
DIRS-y += spdk_dd
endif
DIRS-$(CONFIG_FIO_PLUGIN) += fio

.PHONY: all clean $(DIRS-y)

all: $(DIRS-y)
clean: $(DIRS-y)

include $(SPDK_ROOT_DIR)/mk/spdk.subdirs.mk
