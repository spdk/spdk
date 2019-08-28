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

SEASTAR_FLAGS := $(shell pkg-config --libs --cflags --static ../../../seastar/build/debug/seastar.pc)

SPDK_ROOT_DIR = ../..
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.app_vars.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

FLAGS = -g -O0 -I$(SPDK_ROOT_DIR)/include -Iinclude/

SOCK_MODULES_LIST += sock_seastar

# Functions in SPDK virtio library conflict with DPDK virtio PMD
BLOCKDEV_MODULES_LIST := $(filter-out bdev_virtio virtio,$(BLOCKDEV_MODULES_LIST))

SPDK_LIB_LIST = $(ALL_MODULES_LIST)
SPDK_LIB_LIST += event_bdev event_copy event_nvmf event_vmd event_iscsi event_net event_scsi
SPDK_LIB_LIST += nvmf bdev_rpc bdev iscsi scsi net copy event util conf trace log
SPDK_LIB_LIST += jsonrpc json rpc sock thread notify

EXTRA_DPDK_LIBS = rte_ring rte_mempool_ring rte_pci rte_bus_pci rte_pmd_virtio rte_bus_vdev rte_net

FLAGS += $(SPDK_LIB_LINKER_ARGS)
FLAGS += $(SEASTAR_FLAGS)
FLAGS += $(call dpdk_env_linker_args,$(EXTRA_DPDK_LIBS))
FLAGS += $(SYS_LIBS)

.PHONY : nvme spdk_tgt sock

all: nvme spdk_tgt sock

nvme:
	g++ nvme.cc $(FLAGS) -o nvme

spdk_tgt:
	g++ spdk_tgt.cc $(FLAGS) -o spdk_tgt

sock:
	g++ sock.cc $(FLAGS) -o sock
