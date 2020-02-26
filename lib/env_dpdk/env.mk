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

# This makefile snippet must define the following flags:
# ENV_CFLAGS
# ENV_CXXFLAGS
# ENV_LIBS
# ENV_LINKER_ARGS

DPDK_DIR = $(CONFIG_DPDK_DIR)

export DPDK_ABS_DIR = $(abspath $(DPDK_DIR))

ifneq (, $(wildcard $(DPDK_ABS_DIR)/include/rte_config.h))
DPDK_INC_DIR := $(DPDK_ABS_DIR)/include
else
DPDK_INC_DIR := $(DPDK_ABS_DIR)/include/dpdk
endif
DPDK_INC := -I$(DPDK_INC_DIR)

ifneq (, $(wildcard $(DPDK_ABS_DIR)/lib/librte_eal.a))
DPDK_LIB_EXT = .a
else
DPDK_LIB_EXT = .so
endif

DPDK_LIB_LIST = rte_eal rte_mempool rte_ring rte_mbuf

# librte_mempool_ring was new added from DPDK 17.05. Link this library used for
#   ring based mempool management API.
ifneq (, $(wildcard $(DPDK_ABS_DIR)/lib/librte_mempool_ring.*))
DPDK_LIB_LIST += rte_mempool_ring
endif

# librte_malloc was removed after DPDK 2.1.  Link this library conditionally based on its
#  existence to maintain backward compatibility.
ifneq ($(wildcard $(DPDK_ABS_DIR)/lib/librte_malloc.*),)
DPDK_LIB_LIST += rte_malloc
endif

# librte_pci and librte_bus_pci were added in DPDK 17.11. Link these libraries conditionally
# based on their existence to maintain backward compatibility.
ifneq (, $(wildcard $(DPDK_ABS_DIR)/lib/librte_pci.*))
DPDK_LIB_LIST += rte_pci
endif

ifneq (, $(wildcard $(DPDK_ABS_DIR)/lib/librte_bus_pci.*))
DPDK_LIB_LIST += rte_bus_pci
endif

# There are some complex dependencies when using crypto, reduce or both so
# here we add the feature specific ones and set a flag to add the common
# ones after that.
DPDK_FRAMEWORK=n
ifeq ($(CONFIG_CRYPTO),y)
DPDK_FRAMEWORK=y
DPDK_LIB_LIST += rte_pmd_aesni_mb rte_reorder
endif

ifeq ($(CONFIG_REDUCE),y)
DPDK_FRAMEWORK=y
DPDK_LIB_LIST += rte_pmd_isal_comp
endif

ifeq ($(DPDK_FRAMEWORK),y)
DPDK_LIB_LIST += rte_cryptodev rte_compressdev rte_bus_vdev rte_pmd_qat
endif

ifneq (, $(wildcard $(DPDK_ABS_DIR)/lib/librte_kvargs.*))
DPDK_LIB_LIST += rte_kvargs
endif

LINK_HASH=n

ifeq ($(CONFIG_VHOST),y)
ifneq ($(CONFIG_VHOST_INTERNAL_LIB),y)
DPDK_LIB_LIST += rte_vhost rte_net
LINK_HASH=y
ifneq ($(DPDK_FRAMEWORK),y)
DPDK_LIB_LIST += rte_cryptodev
endif
endif
endif

ifeq ($(CONFIG_RAID5),y)
LINK_HASH=y
endif

ifeq ($(LINK_HASH),y)
DPDK_LIB_LIST += rte_hash
endif

define dpdk_lib_list_to_libs
$(1:%=$(DPDK_ABS_DIR)/lib/lib%$(DPDK_LIB_EXT))
endef

define dpdk_env_linker_args
$(ENV_DPDK_FILE) -Wl,--whole-archive,--no-as-needed $(call dpdk_lib_list_to_libs,$1) -Wl,--no-whole-archive
endef

DPDK_LIB = $(call dpdk_lib_list_to_libs,$(DPDK_LIB_LIST))

# SPDK memory registration requires experimental (deprecated) rte_memory API for DPDK 18.05
ENV_CFLAGS = $(DPDK_INC) -Wno-deprecated-declarations
ENV_CXXFLAGS = $(ENV_CFLAGS)
ifeq ($(CONFIG_SHARED),y)
ENV_DPDK_FILE = $(call spdk_lib_list_to_shared_libs,env_dpdk)
else
ENV_DPDK_FILE = $(call spdk_lib_list_to_static_libs,env_dpdk)
endif
ENV_LIBS = $(ENV_DPDK_FILE) $(DPDK_LIB)
ENV_LINKER_ARGS = $(call dpdk_env_linker_args,$(DPDK_LIB_LIST))

ifeq ($(CONFIG_IPSEC_MB),y)
ENV_LINKER_ARGS += -lIPSec_MB -L$(IPSEC_MB_DIR)
endif

ifeq ($(CONFIG_REDUCE),y)
ENV_LINKER_ARGS += -lisal -L$(ISAL_DIR)/.libs
endif

ifneq (,$(wildcard $(DPDK_INC_DIR)/rte_config.h))
ifneq (,$(shell grep -e "define RTE_LIBRTE_VHOST_NUMA 1" -e "define RTE_EAL_NUMA_AWARE_HUGEPAGES 1" $(DPDK_INC_DIR)/rte_config.h))
ENV_LINKER_ARGS += -lnuma
endif
endif

ifeq ($(OS),Linux)
ENV_LINKER_ARGS += -ldl
endif
ifeq ($(OS),FreeBSD)
ENV_LINKER_ARGS += -lexecinfo
endif
