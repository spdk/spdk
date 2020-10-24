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

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

APP = interrupt_tgt

C_SRCS := interrupt_tgt.c

# Basic libaries to support spdk application
SPDK_LIB_LIST += event thread
SPDK_LIB_LIST += jsonrpc json rpc trace conf util log
# Basic bdev libraries
SPDK_LIB_LIST += bdev notify accel vmd sock
SPDK_LIB_LIST += $(EVENT_BDEV_SUBSYSTEM) # event_bdev depends on some other event modules, but they dont support edriven yet
# Aio bdev library
SPDK_LIB_LIST += bdev_aio
# NBD libraries
SPDK_LIB_LIST += nbd event_nbd
# some bdev modules don't have pollers, so they can be directly runnning in edriven mode
SPDK_LIB_LIST += bdev_malloc bdev_passthru bdev_error bdev_gpt bdev_split bdev_raid
# logical volume and blobstore can directly run in edriven mode
SPDK_LIB_LIST += bdev_lvol lvol blob_bdev blob
# blobfs libraries
SPDK_LIB_LIST += blobfs blobfs_bdev
# vhost blk related libraries.
SPDK_LIB_LIST += vhost event_vhost
SPDK_LIB_LIST += scsi event_scsi # vhost-scsi is not supported, just because vhost lib requires scsi related libs

ifeq ($(SPDK_ROOT_DIR)/lib/env_dpdk,$(CONFIG_ENV))
SPDK_LIB_LIST += env_dpdk_rpc
endif

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
