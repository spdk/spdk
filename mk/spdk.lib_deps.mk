#
#  BSD LICENSE
#
#  Copyright (c) Intel Corporation.
#  All rights reserved.
#
#  Redistribution and use in source and binary forms, with or without
#  modification, are permitted provided that the following conditions
#  are met:
#
#    * Redistributions of source code must retain the above copyright
#      notice, this list of conditions and the following disclaimer.
#    * Redistributions in binary form must reproduce the above copyright
#      notice, this list of conditions and the following disclaimer in
#      the documentation and/or other materials provided with the
#      distribution.
#    * Neither the name of Intel Corporation nor the names of its
#      contributors may be used to endorse or promote products derived
#      from this software without specific prior written permission.
#
#  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
#  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
#  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
#  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
#  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
#  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
#  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
#  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
#  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
#  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
#  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
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
DEPDIRS-vmd := log
DEPDIRS-dma := log
DEPDIRS-trace_parser := log
ifeq ($(CONFIG_VFIO_USER),y)
DEPDIRS-vfio_user := log
endif

DEPDIRS-conf := log util
DEPDIRS-json := log util
DEPDIRS-rdma := log util
DEPDIRS-reduce := log util
DEPDIRS-thread := log util trace

DEPDIRS-nvme := log sock util trace
ifeq ($(CONFIG_RDMA),y)
DEPDIRS-nvme += rdma dma
endif
ifeq ($(CONFIG_VFIO_USER),y)
DEPDIRS-nvme += vfio_user
endif

DEPDIRS-blob := log util thread
DEPDIRS-accel := log util thread json
DEPDIRS-jsonrpc := log util json
DEPDIRS-virtio := log util json thread

DEPDIRS-lvol := log util blob
DEPDIRS-rpc := log util json jsonrpc

DEPDIRS-net := log util $(JSON_LIBS)
DEPDIRS-notify := log util $(JSON_LIBS)
DEPDIRS-trace := log util $(JSON_LIBS)

DEPDIRS-bdev := log util thread $(JSON_LIBS) notify trace
DEPDIRS-blobfs := log thread blob trace
DEPDIRS-event := log util thread $(JSON_LIBS) trace init
DEPDIRS-init := jsonrpc json log rpc thread util

DEPDIRS-ftl := log util thread trace bdev
DEPDIRS-nbd := log util thread $(JSON_LIBS) bdev
DEPDIRS-nvmf := accel log sock util nvme thread $(JSON_LIBS) trace bdev
ifeq ($(CONFIG_RDMA),y)
DEPDIRS-nvmf += rdma
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
DEPDIRS-accel_idxd := log idxd thread $(JSON_LIBS) accel trace

# module/env_dpdk
DEPDIRS-env_dpdk_rpc := log $(JSON_LIBS)

# module/sock
DEPDIRS-sock_posix := log sock util
DEPDIRS-sock_uring := log sock util

# module/scheduler
DEPDIRS-scheduler_dynamic := event log thread util
ifeq ($(SPDK_ROOT_DIR)/lib/env_dpdk,$(CONFIG_ENV))
ifeq ($(OS),Linux)
DEPDIRS-scheduler_dpdk_governor := event log
DEPDIRS-scheduler_gscheduler := event log
endif
endif

# module/bdev
DEPDIRS-bdev_gpt := bdev json log thread util

DEPDIRS-bdev_error := $(BDEV_DEPS)
DEPDIRS-bdev_lvol := $(BDEV_DEPS) lvol blob blob_bdev
DEPDIRS-bdev_rpc := $(BDEV_DEPS)
DEPDIRS-bdev_split := $(BDEV_DEPS)

DEPDIRS-bdev_aio := $(BDEV_DEPS_THREAD)
DEPDIRS-bdev_compress := $(BDEV_DEPS_THREAD) reduce
DEPDIRS-bdev_crypto := $(BDEV_DEPS_THREAD)
DEPDIRS-bdev_delay := $(BDEV_DEPS_THREAD)
DEPDIRS-bdev_iscsi := $(BDEV_DEPS_THREAD)
DEPDIRS-bdev_malloc := $(BDEV_DEPS_THREAD) accel
DEPDIRS-bdev_null := $(BDEV_DEPS_THREAD)
DEPDIRS-bdev_nvme = $(BDEV_DEPS_THREAD) accel nvme
DEPDIRS-bdev_ocf := $(BDEV_DEPS_THREAD)
DEPDIRS-bdev_passthru := $(BDEV_DEPS_THREAD)
DEPDIRS-bdev_pmem := $(BDEV_DEPS_THREAD)
DEPDIRS-bdev_raid := $(BDEV_DEPS_THREAD)
DEPDIRS-bdev_rbd := $(BDEV_DEPS_THREAD)
DEPDIRS-bdev_uring := $(BDEV_DEPS_THREAD)
DEPDIRS-bdev_virtio := $(BDEV_DEPS_THREAD) virtio
DEPDIRS-bdev_zone_block := $(BDEV_DEPS_THREAD)
ifeq ($(OS),Linux)
DEPDIRS-bdev_ftl := $(BDEV_DEPS_THREAD) ftl
endif

# module/event

# module/event/subsystems
# These depdirs include subsystem interdependencies which
# are not related to symbols, but are defined directly in
# the SPDK event subsystem code.
DEPDIRS-event_accel := init accel
DEPDIRS-event_vmd := init vmd $(JSON_LIBS) log thread util

DEPDIRS-event_bdev := init bdev event_accel event_vmd event_sock

DEPDIRS-event_scheduler := event init json log

DEPDIRS-event_nbd := init nbd event_bdev
DEPDIRS-event_nvmf := init nvmf event_bdev event_scheduler event_sock thread log bdev util $(JSON_LIBS)
DEPDIRS-event_scsi := init scsi event_bdev

DEPDIRS-event_iscsi := init iscsi event_scheduler event_scsi event_sock
DEPDIRS-event_vhost := init vhost event_scheduler event_scsi
DEPDIRS-event_sock := init sock
