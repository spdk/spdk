#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

APP = spdk_nvme_perf

C_SRCS := perf.c

SPDK_LIB_LIST += $(SOCK_MODULES_LIST) nvme keyring_file trace event accel thread
ifeq ($(CONFIG_VMD),y)
SPDK_LIB_LIST += vmd
endif

ifeq ($(CONFIG_RDMA_PROV),mlx5_dv)
SPDK_LIB_LIST += accel_mlx5
CFLAGS += -DPERF_WITH_MLX5
endif

ifeq ($(OS),Linux)
SYS_LIBS += -laio
CFLAGS += -DHAVE_LIBAIO
endif

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk

install: $(APP)
	$(INSTALL_APP)

uninstall:
	$(UNINSTALL_APP)
