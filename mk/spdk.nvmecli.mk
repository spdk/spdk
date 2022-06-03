#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) Intel Corporation.
#  All rights reserved.
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
ifneq (, $(wildcard $(DPDK_LIB_DIR)/librte_dmadev.*))
DPDK_LIB_LIST += -lrte_dmadev
endif
endif

override CFLAGS += -I$(SPDK_ROOT_DIR)/include
override LDFLAGS += \
	-Wl,--whole-archive \
	-L$(SPDK_LIB_DIR) $(NVMECLI_SPDK_LIBS) \
	-L$(DPDK_LIB_DIR) $(DPDK_LIB_LIST) \
	-Wl,--no-whole-archive \
	-ldl -pthread -lrt -lrdmacm -lnuma -libverbs

ifeq ($(CONFIG_HAVE_LIBBSD),y)
override LDFLAGS += -lbsd
endif

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
