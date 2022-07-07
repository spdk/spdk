#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES
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
DEPDIRS-sock := log $(JSON_LIBS)
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
DEPDIRS-rdma := log util
DEPDIRS-reduce := log util
DEPDIRS-thread := log util trace

DEPDIRS-nvme := log sock util trace
ifeq ($(OS),Linux)
DEPDIRS-nvme += vfio_user
endif
ifeq ($(CONFIG_RDMA),y)
DEPDIRS-nvme += rdma dma
endif

DEPDIRS-blob := log util thread dma
DEPDIRS-accel := log util thread json rpc jsonrpc dma
DEPDIRS-jsonrpc := log util json
DEPDIRS-virtio := log util json thread vfio_user

DEPDIRS-lvol := log util blob
DEPDIRS-rpc := log util json jsonrpc

DEPDIRS-net := log util $(JSON_LIBS)
DEPDIRS-notify := log util $(JSON_LIBS)
DEPDIRS-trace := log util $(JSON_LIBS)

DEPDIRS-bdev := log util thread $(JSON_LIBS) notify trace dma
DEPDIRS-blobfs := log thread blob trace util
DEPDIRS-event := log util thread $(JSON_LIBS) trace init
DEPDIRS-init := jsonrpc json log rpc thread util

DEPDIRS-ftl := log util thread bdev trace
DEPDIRS-nbd := log util thread $(JSON_LIBS) bdev
ifeq ($(CONFIG_UBLK),y)
DEPDIRS-ublk := log util thread $(JSON_LIBS) bdev
endif
DEPDIRS-nvmf := accel log sock util nvme thread $(JSON_LIBS) trace bdev
ifeq ($(CONFIG_RDMA),y)
DEPDIRS-nvmf += rdma
endif
ifeq ($(CONFIG_RDMA_PROV),mlx5_dv)
DEPDIRS-mlx5 = log rdma util
endif
DEPDIRS-scsi := log util thread $(JSON_LIBS) trace bdev

DEPDIRS-iscsi := log sock util conf thread $(JSON_LIBS) trace scsi
DEPDIRS-vhost = log util thread $(JSON_LIBS) bdev scsi

# ------------------------------------------------------------------------
# Start module/ directory - This section extends the organizational pattern from
# above. However, it introduces several more groupings which may not strictly follow
# the ordering pattern above. These are used for convenience and to help quickly
# determine the unique dependencies of a given module. It is also grouped by directory.

BDEV_DEPS = log util $(JSON_LIBS) bdev
BDEV_DEPS_THREAD = $(BDEV_DEPS) thread

# module/blob
DEPDIRS-blob_bdev := log thread bdev

# module/blobfs
DEPDIRS-blobfs_bdev := $(BDEV_DEPS_THREAD) blob_bdev blobfs
ifeq ($(CONFIG_FUSE),y)
DEPDIRS-blobfs_bdev += event
endif

# module/accel
DEPDIRS-accel_ioat := log ioat thread jsonrpc rpc accel
DEPDIRS-accel_dsa := log idxd thread $(JSON_LIBS) accel trace
DEPDIRS-accel_iaa := log idxd thread $(JSON_LIBS) accel trace
DEPDIRS-accel_dpdk_cryptodev := log thread $(JSON_LIBS) accel
DEPDIRS-accel_dpdk_compressdev := log thread $(JSON_LIBS) accel util

ifeq ($(CONFIG_RDMA_PROV),mlx5_dv)
DEPDIRS-accel_mlx5 := accel thread log mlx5 rdma util
endif

# module/env_dpdk
DEPDIRS-env_dpdk_rpc := log $(JSON_LIBS)

# module/sock
DEPDIRS-sock_posix := log sock util
DEPDIRS-sock_uring := log sock util

# module/scheduler
DEPDIRS-scheduler_dynamic := event log thread util json
ifeq (y,$(DPDK_POWER))
DEPDIRS-scheduler_dpdk_governor := event log
DEPDIRS-scheduler_gscheduler := event log
endif

# module/bdev
ifeq ($(OS),Linux)
DEPDIRS-bdev_ftl := $(BDEV_DEPS) ftl
endif
DEPDIRS-bdev_gpt := bdev json log thread util

DEPDIRS-bdev_error := $(BDEV_DEPS)
DEPDIRS-bdev_lvol := $(BDEV_DEPS) lvol blob blob_bdev
DEPDIRS-bdev_rpc := $(BDEV_DEPS)
DEPDIRS-bdev_split := $(BDEV_DEPS)

DEPDIRS-bdev_aio := $(BDEV_DEPS_THREAD)
DEPDIRS-bdev_compress := $(BDEV_DEPS_THREAD) reduce accel
DEPDIRS-bdev_crypto := $(BDEV_DEPS_THREAD) accel
DEPDIRS-bdev_delay := $(BDEV_DEPS_THREAD)
DEPDIRS-bdev_iscsi := $(BDEV_DEPS_THREAD)
DEPDIRS-bdev_malloc := $(BDEV_DEPS_THREAD) accel
DEPDIRS-bdev_null := $(BDEV_DEPS_THREAD)
DEPDIRS-bdev_nvme = $(BDEV_DEPS_THREAD) accel nvme trace
DEPDIRS-bdev_ocf := $(BDEV_DEPS_THREAD)
DEPDIRS-bdev_passthru := $(BDEV_DEPS_THREAD)
DEPDIRS-bdev_pmem := $(BDEV_DEPS_THREAD)
DEPDIRS-bdev_raid := $(BDEV_DEPS_THREAD)
DEPDIRS-bdev_rbd := $(BDEV_DEPS_THREAD)
DEPDIRS-bdev_uring := $(BDEV_DEPS_THREAD)
DEPDIRS-bdev_virtio := $(BDEV_DEPS_THREAD) virtio
DEPDIRS-bdev_zone_block := $(BDEV_DEPS_THREAD)
DEPDIRS-bdev_xnvme := $(BDEV_DEPS_THREAD)

# module/event

# module/event/subsystems
# These depdirs include subsystem interdependencies which
# are not related to symbols, but are defined directly in
# the SPDK event subsystem code.
DEPDIRS-event_accel := init accel event_iobuf
DEPDIRS-event_vmd := init vmd $(JSON_LIBS) log thread util

DEPDIRS-event_bdev := init bdev event_accel event_vmd event_sock event_iobuf

DEPDIRS-event_scheduler := event init json log

DEPDIRS-event_nbd := init nbd event_bdev
ifeq ($(CONFIG_UBLK),y)
DEPDIRS-event_ublk := init ublk event_bdev
endif
DEPDIRS-event_nvmf := init nvmf event_bdev event_scheduler event_sock thread log bdev util $(JSON_LIBS)
DEPDIRS-event_scsi := init scsi event_bdev

DEPDIRS-event_iscsi := init iscsi event_scheduler event_scsi event_sock
DEPDIRS-event_vhost_blk := init vhost
DEPDIRS-event_vhost_scsi := init vhost event_scheduler event_scsi
DEPDIRS-event_sock := init sock
DEPDIRS-event_vfu_tgt := init vfu_tgt
DEPDIRS-event_iobuf := init log thread util $(JSON_LIBS)

# module/vfu_device

ifeq ($(CONFIG_VFIO_USER),y)
DEPDIRS-vfu_device := $(BDEV_DEPS_THREAD) scsi vfu_tgt
endif
