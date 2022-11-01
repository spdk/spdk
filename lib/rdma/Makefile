#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2020 Intel Corporation. All rights reserved.
#  Copyright (c) Mellanox Technologies LTD. All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 4
SO_MINOR := 1

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_rdma.map)

LIBNAME = rdma

C_SRCS = common.c

ifeq ($(CONFIG_RDMA_PROV),verbs)
C_SRCS += rdma_verbs.c
else ifeq ($(CONFIG_RDMA_PROV),mlx5_dv)
C_SRCS += rdma_mlx5_dv.c
LOCAL_SYS_LIBS += -lmlx5
else
$(error Wrong RDMA provider specified: $(CONFIG_RDMA_PROV))
endif

LOCAL_SYS_LIBS += -libverbs -lrdmacm
#Attach only if FreeBSD and RDMA is specified with configure
ifeq ($(OS),FreeBSD)
# Mellanox - MLX4 HBA Userspace Library
ifneq ("$(wildcard /usr/lib/libmlx4.*)","")
LOCAL_SYS_LIBS += -lmlx4
endif
# Mellanox - MLX5 HBA Userspace Library
ifneq ("$(wildcard /usr/lib/libmlx5.*)","")
LOCAL_SYS_LIBS += -lmlx5
endif
# Chelsio HBA Userspace Library
ifneq ("$(wildcard /usr/lib/libcxgb4.*)","")
LOCAL_SYS_LIBS += -lcxgb4
endif
endif

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
