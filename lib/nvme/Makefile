#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 9
SO_MINOR := 1

C_SRCS = nvme_ctrlr_cmd.c nvme_ctrlr.c nvme_fabric.c nvme_ns_cmd.c \
	nvme_ns.c nvme_pcie_common.c nvme_pcie.c nvme_qpair.c nvme.c \
	nvme_quirks.c nvme_transport.c nvme_discovery.c \
	nvme_ctrlr_ocssd_cmd.c nvme_ns_ocssd_cmd.c nvme_tcp.c \
	nvme_opal.c nvme_io_msg.c nvme_poll_group.c nvme_zns.c \
	nvme_cuse.c
ifeq ($(OS),Linux)
C_SRCS += nvme_vfio_user.c
endif
C_SRCS-$(CONFIG_RDMA) += nvme_rdma.c

LIBNAME = nvme
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

ifeq ($(CONFIG_NVME_CUSE),y)
# fuse requires to set _FILE_OFFSET_BITS to 64 bits even for 64 bit machines
CFLAGS += -D_FILE_OFFSET_BITS=64
endif

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_nvme.map)

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
