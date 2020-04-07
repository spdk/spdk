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

DEPDIRS-ioat := log
DEPDIRS-idxd := log util
DEPDIRS-sock := log
DEPDIRS-util := log
DEPDIRS-vmd := log

DEPDIRS-conf := log util
DEPDIRS-json := log util
DEPDIRS-nvme := log sock util
DEPDIRS-reduce := log util
DEPDIRS-thread := log util

DEPDIRS-blob := log util thread
DEPDIRS-accel := thread log rpc thread util $(JSON_LIBS)
DEPDIRS-jsonrpc := log util json
DEPDIRS-virtio := log util json thread

DEPDIRS-lvol := log util blob
DEPDIRS-rpc := log util json jsonrpc

DEPDIRS-log_rpc := log $(JSON_LIBS)
DEPDIRS-net := log util $(JSON_LIBS)
DEPDIRS-notify := log util $(JSON_LIBS)
DEPDIRS-trace := log util $(JSON_LIBS)

DEPDIRS-bdev := log util conf thread $(JSON_LIBS) notify trace
DEPDIRS-blobfs := log conf thread blob trace
DEPDIRS-event := log util conf thread $(JSON_LIBS) trace

DEPDIRS-ftl := log util thread trace bdev
DEPDIRS-nbd := log util thread $(JSON_LIBS) bdev
DEPDIRS-nvmf := log sock util nvme thread $(JSON_LIBS) trace bdev
DEPDIRS-scsi := log util thread $(JSON_LIBS) trace bdev

DEPDIRS-iscsi := log sock util conf thread $(JSON_LIBS) trace event scsi
DEPDIRS-vhost = log util conf thread $(JSON_LIBS) bdev event scsi
ifeq ($(CONFIG_VHOST_INTERNAL_LIB),y)
DEPDIRS-vhost += rte_vhost
endif

# ------------------------------------------------------------------------
# Start module/ directory - This section extends the organizational pattern from
# above. However, it introduces several more groupings which may not strictly follow
# the ordering pattern above. These are used for convenience and to help quickly
# determine the unique dependencies of a given module. It is also grouped by directory.

BDEV_DEPS = log util $(JSON_LIBS) bdev
BDEV_DEPS_CONF = $(BDEV_DEPS) conf
BDEV_DEPS_THREAD = $(BDEV_DEPS) thread
BDEV_DEPS_CONF_THREAD = $(BDEV_DEPS) conf thread

# module/blob
DEPDIRS-blob_bdev := log thread bdev

# module/blobfs
DEPDIRS-blobfs_bdev := $(BDEV_DEPS_THREAD) blob_bdev blobfs

# module/accel
DEPDIRS-accel_ioat := log ioat conf thread $(JSON_LIBS) accel
DEPDIRS-accel_idxd := log idxd thread $(JSON_LIBS) accel

# module/env_dpdk
DEPDIRS-env_dpdk_rpc := log $(JSON_LIBS)

# module/sock
DEPDIRS-sock_posix := log sock util
DEPDIRS-sock_uring := log sock util
DEPDIRS-sock_vpp := log sock util thread

# module/bdev
DEPDIRS-bdev_gpt := bdev conf json log thread util

DEPDIRS-bdev_lvol := $(BDEV_DEPS) lvol blob blob_bdev
DEPDIRS-bdev_rpc := $(BDEV_DEPS)

DEPDIRS-bdev_error := $(BDEV_DEPS_CONF)
DEPDIRS-bdev_malloc := $(BDEV_DEPS_CONF) accel
DEPDIRS-bdev_split := $(BDEV_DEPS_CONF)

DEPDIRS-bdev_compress := $(BDEV_DEPS_THREAD) reduce
DEPDIRS-bdev_delay := $(BDEV_DEPS_THREAD)
DEPDIRS-bdev_zone_block := $(BDEV_DEPS_THREAD)
ifeq ($(OS),Linux)
DEPDIRS-bdev_ftl := $(BDEV_DEPS_THREAD) ftl
endif

DEPDIRS-bdev_aio := $(BDEV_DEPS_CONF_THREAD)
DEPDIRS-bdev_crypto := $(BDEV_DEPS_CONF_THREAD)
DEPDIRS-bdev_iscsi := $(BDEV_DEPS_CONF_THREAD)
DEPDIRS-bdev_null := $(BDEV_DEPS_CONF_THREAD)
DEPDIRS-bdev_nvme = $(BDEV_DEPS_CONF_THREAD) nvme
DEPDIRS-bdev_ocf := $(BDEV_DEPS_CONF_THREAD)
DEPDIRS-bdev_passthru := $(BDEV_DEPS_CONF_THREAD)
DEPDIRS-bdev_pmem := $(BDEV_DEPS_CONF_THREAD)
DEPDIRS-bdev_raid := $(BDEV_DEPS_CONF_THREAD)
DEPDIRS-bdev_rbd := $(BDEV_DEPS_CONF_THREAD)
DEPDIRS-bdev_virtio := $(BDEV_DEPS_CONF_THREAD) virtio

# module/event
# module/event/app
DEPDIRS-app_rpc := log util thread event $(JSON_LIBS)

# module/event/subsystems
# These depdirs include subsystem interdependencies which
# are not related to symbols, but are defined directly in
# the SPDK event subsystem code.
DEPDIRS-event_accel := accel event
DEPDIRS-event_net := sock net event
DEPDIRS-event_vmd := vmd conf $(JSON_LIBS) event log thread

DEPDIRS-event_bdev := bdev event event_accel event_vmd

DEPDIRS-event_nbd := event nbd event_bdev
DEPDIRS-event_nvmf := $(BDEV_DEPS_CONF_THREAD) event nvme nvmf event_bdev
DEPDIRS-event_scsi := event scsi event_bdev

DEPDIRS-event_iscsi := event iscsi event_scsi
DEPDIRS-event_vhost := event vhost event_scsi
