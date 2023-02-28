#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation.
#  Copyright (c) 2021, 2022 NVIDIA CORPORATION & AFFILIATES.
#  All rights reserved.
#

BLOCKDEV_MODULES_LIST = bdev_malloc bdev_null bdev_nvme bdev_passthru bdev_lvol
BLOCKDEV_MODULES_LIST += bdev_raid bdev_error bdev_gpt bdev_split bdev_delay
BLOCKDEV_MODULES_LIST += bdev_zone_block
BLOCKDEV_MODULES_LIST += blobfs blobfs_bdev blob_bdev blob lvol vmd nvme

# Some bdev modules don't have pollers, so they can directly run in interrupt mode
INTR_BLOCKDEV_MODULES_LIST = bdev_malloc bdev_passthru bdev_error bdev_gpt bdev_split bdev_raid
# Logical volume, blobstore and blobfs can directly run in both interrupt mode and poll mode.
INTR_BLOCKDEV_MODULES_LIST += bdev_lvol blobfs blobfs_bdev blob_bdev blob lvol

ifeq ($(CONFIG_XNVME),y)
BLOCKDEV_MODULES_LIST += bdev_xnvme
endif

ifeq ($(CONFIG_VFIO_USER),y)
BLOCKDEV_MODULES_LIST += vfio_user
endif

ifeq ($(CONFIG_CRYPTO),y)
BLOCKDEV_MODULES_LIST += bdev_crypto
ifeq ($(CONFIG_CRYPTO_MLX5),y)
BLOCKDEV_MODULES_PRIVATE_LIBS += -lmlx5 -libverbs
endif
endif

ifeq ($(CONFIG_OCF),y)
BLOCKDEV_MODULES_LIST += bdev_ocf
BLOCKDEV_MODULES_LIST += ocfenv
endif

ifeq ($(CONFIG_VBDEV_COMPRESS),y)
BLOCKDEV_MODULES_LIST += bdev_compress reduce
BLOCKDEV_MODULES_PRIVATE_LIBS += -lpmem
ifeq ($(CONFIG_VBDEV_COMPRESS_MLX5),y)
BLOCKDEV_MODULES_PRIVATE_LIBS += -lmlx5 -libverbs
endif
endif

ifeq ($(CONFIG_RDMA),y)
BLOCKDEV_MODULES_LIST += rdma
BLOCKDEV_MODULES_PRIVATE_LIBS += -libverbs -lrdmacm
ifeq ($(CONFIG_RDMA_PROV),mlx5_dv)
BLOCKDEV_MODULES_PRIVATE_LIBS += -lmlx5
endif
endif

ifeq ($(OS),Linux)
BLOCKDEV_MODULES_LIST += bdev_aio
BLOCKDEV_MODULES_PRIVATE_LIBS += -laio
INTR_BLOCKDEV_MODULES_LIST += bdev_aio
BLOCKDEV_MODULES_LIST += bdev_ftl ftl
ifeq ($(CONFIG_VIRTIO),y)
BLOCKDEV_MODULES_LIST += bdev_virtio virtio
endif
ifeq ($(CONFIG_ISCSI_INITIATOR),y)
BLOCKDEV_MODULES_LIST += bdev_iscsi
# Fedora installs libiscsi to /usr/lib64/iscsi for some reason.
BLOCKDEV_MODULES_PRIVATE_LIBS += -L/usr/lib64/iscsi -liscsi
endif
endif

ifeq ($(CONFIG_URING),y)
BLOCKDEV_MODULES_LIST += bdev_uring
BLOCKDEV_MODULES_PRIVATE_LIBS += -luring
ifneq ($(strip $(CONFIG_URING_PATH)),)
CFLAGS += -I$(CONFIG_URING_PATH)
BLOCKDEV_MODULES_PRIVATE_LIBS += -L$(CONFIG_URING_PATH)
endif
endif

ifeq ($(CONFIG_RBD),y)
BLOCKDEV_MODULES_LIST += bdev_rbd
BLOCKDEV_MODULES_PRIVATE_LIBS += -lrados -lrbd
endif

ifeq ($(CONFIG_PMDK),y)
BLOCKDEV_MODULES_LIST += bdev_pmem
BLOCKDEV_MODULES_PRIVATE_LIBS += -lpmemblk -lpmem
endif

ifeq ($(CONFIG_DAOS),y)
BLOCKDEV_MODULES_LIST += bdev_daos
BLOCKDEV_MODULES_PRIVATE_LIBS += -ldaos -ldaos_common -ldfs -lgurt -luuid -ldl
endif

SOCK_MODULES_LIST = sock_posix

ifeq ($(OS), Linux)
ifeq ($(CONFIG_URING),y)
SOCK_MODULES_LIST += sock_uring
endif
endif

ACCEL_MODULES_LIST = accel_ioat ioat
ifeq ($(CONFIG_IDXD),y)
ACCEL_MODULES_LIST += accel_dsa accel_iaa idxd
endif
ifeq ($(CONFIG_CRYPTO),y)
ACCEL_MODULES_LIST += accel_dpdk_cryptodev
endif
ifeq ($(CONFIG_DPDK_COMPRESSDEV),y)
ACCEL_MODULES_LIST += accel_dpdk_compressdev
endif

ifeq ($(CONFIG_RDMA_PROV)|$(CONFIG_CRYPTO),mlx5_dv|y)
ACCEL_MODULES_LIST += accel_mlx5
endif

SCHEDULER_MODULES_LIST = scheduler_dynamic
ifeq (y,$(DPDK_POWER))
SCHEDULER_MODULES_LIST += env_dpdk scheduler_dpdk_governor scheduler_gscheduler
endif

ifeq ($(CONFIG_VFIO_USER),y)
VFU_DEVICE_MODULES_LIST = vfu_device
endif

EVENT_BDEV_SUBSYSTEM = event_bdev event_accel event_vmd event_sock event_iobuf

ALL_MODULES_LIST = $(BLOCKDEV_MODULES_LIST) $(ACCEL_MODULES_LIST) $(SCHEDULER_MODULES_LIST) $(SOCK_MODULES_LIST)
ALL_MODULES_LIST += $(VFU_DEVICE_MODULES_LIST)
SYS_LIBS += $(BLOCKDEV_MODULES_PRIVATE_LIBS)
