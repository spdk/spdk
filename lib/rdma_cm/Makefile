#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2026 Alibaba Cloud and its affiliates. All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 1
SO_MINOR := 0

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_rdma_cm.map)

LIBNAME = rdma_cm

ifeq ($(CONFIG_RDMA_CM),cma)
C_SRCS = rdma_cm_cma.c
LOCAL_SYS_LIBS += -libverbs -lrdmacm
else ifeq ($(CONFIG_RDMA_CM),mock)
C_SRCS = rdma_cm_mock.c
else
$(error Wrong RDMA CM library specified: $(CONFIG_RDMA_CM))
endif

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
