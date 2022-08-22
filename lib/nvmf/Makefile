#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 14
SO_MINOR := 0

C_SRCS = ctrlr.c ctrlr_discovery.c ctrlr_bdev.c \
	 subsystem.c nvmf.c nvmf_rpc.c transport.c tcp.c

C_SRCS-$(CONFIG_RDMA) += rdma.c
LIBNAME = nvmf
LOCAL_SYS_LIBS =
ifeq ($(CONFIG_RDMA),y)
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
endif

ifeq ($(CONFIG_VFIO_USER), y)
C_SRCS += vfio_user.c
CFLAGS += -I$(VFIO_USER_INCLUDE_DIR)
LDFLAGS += -L$(VFIO_USER_LIBRARY_DIR)
LOCAL_SYS_LIBS += -lvfio-user -ljson-c
endif

ifeq ($(CONFIG_FC),y)
C_SRCS += fc.c fc_ls.c
CFLAGS += -I$(CURDIR) $(ENV_CFLAGS)
ifneq ($(strip $(CONFIG_FC_PATH)),)
CFLAGS += -I$(CONFIG_FC_PATH)
endif
endif

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_nvmf.map)

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
