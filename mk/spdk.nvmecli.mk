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

SPDK_ROOT_DIR ?= $(abspath $(CURDIR)/..)
SPDK_LIB_DIR ?= $(SPDK_ROOT_DIR)/build/lib

include $(SPDK_ROOT_DIR)/mk/config.mk

DPDK_LIB_DIR ?= $(CONFIG_DPDK_DIR)/lib
DPDK_LIB_LIST = -lrte_eal -lrte_mempool -lrte_ring -lrte_pci -lrte_bus_pci -lrte_mbuf

ifneq (, $(wildcard $(DPDK_LIB_DIR)/librte_kvargs.*))
DPDK_LIB_LIST += -lrte_kvargs
endif

ifneq (, $(wildcard $(DPDK_LIB_DIR)/librte_power.*))
DPDK_LIB_LIST += -lrte_power
endif

ifneq (, $(wildcard $(DPDK_LIB_DIR)/librte_telemetry.*))
DPDK_LIB_LIST += -lrte_telemetry
endif

NVMECLI_SPDK_LIBS = -lspdk_log -lspdk_sock -lspdk_nvme -lspdk_env_dpdk -lspdk_util -lspdk_jsonrpc -lspdk_json -lspdk_rpc

ifeq ($(CONFIG_RDMA),y)
NVMECLI_SPDK_LIBS += -lspdk_rdma
endif

ifeq ($(CONFIG_OCF),y)
NVMECLI_SPDK_LIBS += -lspdk_ocfenv
endif

ifeq ($(CONFIG_VHOST),y)
DPDK_LIB_LIST += -lrte_vhost -lrte_net -lrte_cryptodev -lrte_hash
endif

override CFLAGS += -I$(SPDK_ROOT_DIR)/include
override LDFLAGS += \
	-Wl,--whole-archive \
	-L$(SPDK_LIB_DIR) $(NVMECLI_SPDK_LIBS) \
	-L$(DPDK_LIB_DIR) $(DPDK_LIB_LIST) \
	-Wl,--no-whole-archive \
	-ldl -pthread -lrt -lrdmacm -lnuma -libverbs

ifeq ($(CONFIG_ISAL), y)
ISAL_DIR=$(SPDK_ROOT_DIR)/isa-l
override LDFLAGS += -L$(ISAL_DIR)/.libs -lisal
override CFLAGS += -I$(ISAL_DIR)/..
endif

ifeq ($(CONFIG_ASAN),y)
override CFLAGS += -fsanitize=address
override LDFLAGS += -fsanitize=address
endif

ifeq ($(CONFIG_UBSAN),y)
override CFLAGS += -fsanitize=undefined
override LDFLAGS += -fsanitize=undefined
endif

ifeq ($(CONFIG_TSAN),y)
override CFLAGS += -fsanitize=thread
override LDFLAGS += -fsanitize=thread
endif

ifeq ($(CONFIG_COVERAGE), y)
override CFLAGS += -fprofile-arcs -ftest-coverage
override LDFLAGS += -fprofile-arcs -ftest-coverage
endif

ifeq ($(CONFIG_ISCSI_INITIATOR),y)
override LDFLAGS += -L/usr/lib64/iscsi -liscsi
endif
