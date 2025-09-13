#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES.
#  All rights reserved.
#

# A quick note on organization:
#
# Each grouping is independent from itself. it depends only on libraries
# in the grouping above it. All dependencies are listed alphabetically within
# groups. The only exception to this is the JSON_LIBS grouping which is a special
# case since they almost always occur together.

JSON_LIBS := json jsonrpc rpc

DEPDIRS-env_ocf :=
DEPDIRS-log :=
DEPDIRS-rte_vhost :=

DEPDIRS-env_dpdk := log util

DEPDIRS-ioat := log
DEPDIRS-idxd := log util
DEPDIRS-sock := log $(JSON_LIBS) trace util
DEPDIRS-util := log
DEPDIRS-vmd := log util
DEPDIRS-dma := log
DEPDIRS-trace_parser := log
ifeq ($(OS),Linux)
DEPDIRS-vfio_user := log
endif
ifeq ($(CONFIG_VFIO_USER),y)
DEPDIRS-vfu_tgt := log util thread $(JSON_LIBS)
endif

DEPDIRS-conf := log util
DEPDIRS-json := log util
DEPDIRS-rdma_utils := dma log util
DEPDIRS-rdma_provider := log util rdma_utils
ifeq ($(CONFIG_RDMA_PROV),mlx5_dv)
DEPDIRS-rdma_provider += dma mlx5
endif
DEPDIRS-thread := log util trace
DEPDIRS-keyring := log util $(JSON_LIBS)

DEPDIRS-nvme := log keyring sock util trace dma
ifeq ($(CONFIG_VFIO_USER),y)
DEPDIRS-nvme += vfio_user
endif
ifeq ($(CONFIG_RDMA),y)
DEPDIRS-nvme += rdma_provider rdma_utils
endif

DEPDIRS-blob := log util thread dma trace
DEPDIRS-accel := log util thread json rpc jsonrpc dma
DEPDIRS-jsonrpc := log util json
DEPDIRS-virtio := log util json thread vfio_user

DEPDIRS-lvol := log util blob thread
DEPDIRS-rpc := log util json jsonrpc

DEPDIRS-net := log util $(JSON_LIBS)
DEPDIRS-notify := log util $(JSON_LIBS)
DEPDIRS-trace := log util $(JSON_LIBS)

DEPDIRS-bdev := accel log util thread $(JSON_LIBS) notify trace dma
DEPDIRS-event := log util thread $(JSON_LIBS) trace init
DEPDIRS-init := jsonrpc json log rpc thread util
DEPDIRS-ftl := log util thread bdev json jsonrpc
ifeq ($(CONFIG_DEBUG),y)
DEPDIRS-ftl += trace
endif
DEPDIRS-nbd := log util thread $(JSON_LIBS) bdev
ifeq ($(CONFIG_UBLK),y)
DEPDIRS-ublk := log util thread $(JSON_LIBS) bdev
endif
DEPDIRS-nvmf := accel log sock util nvme thread $(JSON_LIBS) trace bdev keyring
ifeq ($(CONFIG_RDMA),y)
DEPDIRS-nvmf += rdma_provider rdma_utils
endif
ifeq ($(CONFIG_RDMA_PROV),mlx5_dv)
DEPDIRS-mlx5 = log rdma_utils util
endif
DEPDIRS-scsi := log util thread $(JSON_LIBS) trace bdev

DEPDIRS-iscsi := log sock util conf thread $(JSON_LIBS) trace scsi
DEPDIRS-vhost = log util thread $(JSON_LIBS) bdev scsi

DEPDIRS-fsdev := log thread util $(JSON_LIBS) notify
DEPDIRS-fuse_dispatcher := log thread util fsdev

# ------------------------------------------------------------------------
# Start module/ directory - This section extends the organizational pattern from
# above. However, it introduces several more groupings which may not strictly follow
# the ordering pattern above. These are used for convenience and to help quickly
# determine the unique dependencies of a given module. It is also grouped by directory.

BDEV_DEPS = log util $(JSON_LIBS) bdev
BDEV_DEPS_THREAD = $(BDEV_DEPS) thread

FSDEV_DEPS = log util $(JSON_LIBS) fsdev
FSDEV_DEPS_THREAD = $(FSDEV_DEPS) thread

# module/blob
DEPDIRS-blob_bdev := log thread bdev

# module/accel
DEPDIRS-accel_ioat := log ioat thread $(JSON_LIBS) accel
DEPDIRS-accel_dsa := log util idxd thread $(JSON_LIBS) accel trace
DEPDIRS-accel_iaa := log util idxd thread $(JSON_LIBS) accel trace
DEPDIRS-accel_dpdk_cryptodev := log thread $(JSON_LIBS) accel util
DEPDIRS-accel_dpdk_compressdev := log thread $(JSON_LIBS) accel util
DEPDIRS-accel_error := accel $(JSON_LIBS) thread util

ifeq ($(CONFIG_RDMA_PROV),mlx5_dv)
DEPDIRS-accel_mlx5 := accel thread log mlx5 rdma_utils util
endif

ifeq ($(CONFIG_CUDA),y)
DEPDIRS-accel_cuda := accel thread log jsonrpc rpc
endif

# module/env_dpdk
DEPDIRS-env_dpdk_rpc := $(JSON_LIBS)

# module/sock
DEPDIRS-sock_posix := log sock util thread trace
DEPDIRS-sock_uring := log sock util thread trace

# module/scheduler
DEPDIRS-scheduler_dynamic := event log thread util json
ifeq (y,$(DPDK_POWER))
DEPDIRS-scheduler_dpdk_governor := event json log util
DEPDIRS-scheduler_gscheduler := event log util
endif

# module/bdev
ifeq ($(OS),Linux)
DEPDIRS-bdev_ftl := $(BDEV_DEPS) ftl
endif
DEPDIRS-bdev_gpt := bdev json log thread util

DEPDIRS-bdev_lvol := $(BDEV_DEPS) lvol blob blob_bdev
DEPDIRS-bdev_rpc := $(BDEV_DEPS)
DEPDIRS-bdev_split := $(BDEV_DEPS)

DEPDIRS-bdev_aio := $(BDEV_DEPS_THREAD)
DEPDIRS-bdev_crypto := $(BDEV_DEPS_THREAD) accel
DEPDIRS-bdev_delay := $(BDEV_DEPS_THREAD)
DEPDIRS-bdev_error := $(BDEV_DEPS_THREAD)
DEPDIRS-bdev_iscsi := $(BDEV_DEPS_THREAD)
DEPDIRS-bdev_malloc := $(BDEV_DEPS_THREAD) accel dma
DEPDIRS-bdev_null := $(BDEV_DEPS_THREAD)
DEPDIRS-bdev_nvme = $(BDEV_DEPS_THREAD) accel keyring nvme trace
DEPDIRS-bdev_ocf := $(BDEV_DEPS_THREAD)
DEPDIRS-bdev_passthru := $(BDEV_DEPS_THREAD)
DEPDIRS-bdev_raid := $(BDEV_DEPS_THREAD) trace
ifeq ($(CONFIG_RAID5F),y)
DEPDIRS-bdev_raid += accel
endif
DEPDIRS-bdev_rbd := $(BDEV_DEPS_THREAD)
DEPDIRS-bdev_uring := $(BDEV_DEPS_THREAD)
DEPDIRS-bdev_virtio := $(BDEV_DEPS_THREAD) virtio
DEPDIRS-bdev_zone_block := $(BDEV_DEPS_THREAD)
DEPDIRS-bdev_xnvme := $(BDEV_DEPS_THREAD)

# module/fsdev
DEPDIRS-fsdev_aio := $(FSDEV_DEPS_THREAD)

# module/event

# module/event/subsystems
# These depdirs include subsystem interdependencies which
# are not related to symbols, but are defined directly in
# the SPDK event subsystem code.
DEPDIRS-event_accel := init accel event_iobuf
DEPDIRS-event_vmd := init vmd $(JSON_LIBS) log thread util

DEPDIRS-event_bdev := init bdev event_accel event_vmd event_sock event_iobuf event_keyring

DEPDIRS-event_scheduler := event init json log

DEPDIRS-event_nbd := init nbd event_bdev
ifeq ($(CONFIG_UBLK),y)
DEPDIRS-event_ublk := init ublk event_bdev
endif
DEPDIRS-event_nvmf := init nvme nvmf event_bdev event_scheduler event_sock event_keyring \
		      thread log bdev util $(JSON_LIBS)
DEPDIRS-event_scsi := init scsi event_bdev

DEPDIRS-event_iscsi := init iscsi event_scheduler event_scsi event_sock
DEPDIRS-event_vhost_blk := init vhost
DEPDIRS-event_vhost_scsi := init vhost event_scheduler event_scsi
DEPDIRS-event_sock := init sock log
DEPDIRS-event_vfu_tgt := init vfu_tgt
DEPDIRS-event_iobuf := init log thread util $(JSON_LIBS)
DEPDIRS-event_keyring := init json keyring
DEPDIRS-event_fsdev := init fsdev

# module/vfu_device

ifeq ($(CONFIG_VFIO_USER),y)
DEPDIRS-vfu_device := $(BDEV_DEPS_THREAD) scsi vfu_tgt
ifeq ($(CONFIG_FSDEV),y)
DEPDIRS-vfu_device += fuse_dispatcher
endif
endif

# module/keyring
DEPDIRS-keyring_file := log keyring util $(JSON_LIBS)
DEPDIRS-keyring_linux := log keyring util $(JSON_LIBS)
