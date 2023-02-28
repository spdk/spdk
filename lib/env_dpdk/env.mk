#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation.
#  All rights reserved.
#  Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

# This makefile snippet must define the following flags:
# ENV_CFLAGS
# ENV_CXXFLAGS
# ENV_LIBS
# ENV_LINKER_ARGS

DPDK_DIR = $(CONFIG_DPDK_DIR)

export DPDK_ABS_DIR = $(abspath $(DPDK_DIR))

ifneq ($(CONFIG_DPDK_LIB_DIR),)
DPDK_LIB_DIR = $(CONFIG_DPDK_LIB_DIR)
else
DPDK_LIB_DIR = $(DPDK_ABS_DIR)/lib
endif

ifneq ($(CONFIG_DPDK_INC_DIR),)
DPDK_INC_DIR = $(CONFIG_DPDK_INC_DIR)
else
ifneq (, $(wildcard $(DPDK_ABS_DIR)/include/rte_config.h))
DPDK_INC_DIR := $(DPDK_ABS_DIR)/include
else
DPDK_INC_DIR := $(DPDK_ABS_DIR)/include/dpdk
endif
endif

DPDK_INC := -I$(DPDK_INC_DIR)

DPDK_LIB_LIST = rte_eal rte_mempool rte_ring rte_mbuf rte_bus_pci rte_pci rte_mempool_ring
DPDK_LIB_LIST += rte_telemetry rte_kvargs

DPDK_POWER=n

ifeq ($(OS),Linux)
# Despite rte_power was added DPDK 1.6,
# some DPDK packages do not include it. See #2534.
ifneq (, $(wildcard $(DPDK_LIB_DIR)/librte_power.*))
DPDK_POWER=y
# Since DPDK 21.02 rte_power depends on rte_ethdev that
# in turn depends on rte_net.
DPDK_LIB_LIST += rte_power rte_ethdev rte_net
endif
endif

# There are some complex dependencies when using crypto, compress or both so
# here we add the feature specific ones and set a flag to add the common
# ones after that.
DPDK_FRAMEWORK=n

ifeq ($(findstring y,$(CONFIG_CRYPTO_MLX5)$(CONFIG_VBDEV_COMPRESS_MLX5)),y)
DPDK_LIB_LIST += rte_common_mlx5
# Introduced in DPDK 21.08
ifneq (, $(wildcard $(DPDK_LIB_DIR)/librte_bus_auxiliary.*))
DPDK_LIB_LIST += rte_bus_auxiliary
endif
endif

ifeq ($(CONFIG_CRYPTO),y)
DPDK_FRAMEWORK=y
ifneq (, $(wildcard $(DPDK_LIB_DIR)/librte_crypto_ipsec_mb.*))
# PMD name as of DPDK 21.11
DPDK_LIB_LIST += rte_crypto_ipsec_mb
else
ifneq (, $(wildcard $(DPDK_LIB_DIR)/librte_crypto_aesni_mb.*))
# PMD name for DPDK 21.08 and earlier
DPDK_LIB_LIST += rte_crypto_aesni_mb
endif
endif

ifeq ($(CONFIG_CRYPTO_MLX5),y)
ifneq (, $(wildcard $(DPDK_LIB_DIR)/librte_crypto_mlx5.*))
DPDK_LIB_LIST += rte_crypto_mlx5
endif
endif
endif

ifeq ($(findstring y,$(CONFIG_DPDK_COMPRESSDEV)$(CONFIG_VBDEV_COMPRESS)),y)
DPDK_FRAMEWORK=y
ifneq (, $(wildcard $(DPDK_LIB_DIR)/librte_compress_isal.*))
DPDK_LIB_LIST += rte_compress_isal
endif
ifeq ($(CONFIG_VBDEV_COMPRESS_MLX5),y)
DPDK_LIB_LIST += rte_compress_mlx5
endif
endif

ifeq ($(DPDK_FRAMEWORK),y)
DPDK_LIB_LIST += rte_cryptodev rte_compressdev rte_bus_vdev
DPDK_LIB_LIST += rte_common_qat
endif

LINK_HASH=n

ifeq ($(CONFIG_VHOST),y)
DPDK_LIB_LIST += rte_vhost rte_net
ifneq (, $(wildcard $(DPDK_LIB_DIR)/librte_dmadev.*))
# Introduced in DPDK 21.11, and rte_vhost became dependent on
# it shortly thereafter
DPDK_LIB_LIST += rte_dmadev
endif
LINK_HASH=y
ifneq ($(DPDK_FRAMEWORK),y)
DPDK_LIB_LIST += rte_cryptodev
endif
endif

ifeq ($(CONFIG_FC),y)
LINK_HASH=y
endif

ifeq ($(LINK_HASH),y)
DPDK_LIB_LIST += rte_hash
ifneq (, $(wildcard $(DPDK_LIB_DIR)/librte_rcu.*))
DPDK_LIB_LIST += rte_rcu
endif
endif


DPDK_LIB_LIST_SORTED = $(sort $(DPDK_LIB_LIST))

DPDK_SHARED_LIB = $(DPDK_LIB_LIST_SORTED:%=$(DPDK_LIB_DIR)/lib%.so)
DPDK_STATIC_LIB = $(DPDK_LIB_LIST_SORTED:%=$(DPDK_LIB_DIR)/lib%.a)
DPDK_SHARED_LIB_LINKER_ARGS = $(call add_no_as_needed,$(DPDK_SHARED_LIB))
DPDK_STATIC_LIB_LINKER_ARGS = $(call add_whole_archive,$(DPDK_STATIC_LIB))

ENV_CFLAGS = $(DPDK_INC) -DALLOW_EXPERIMENTAL_API
ENV_CXXFLAGS = $(ENV_CFLAGS)

DPDK_PRIVATE_LINKER_ARGS =

ifeq ($(CONFIG_IPSEC_MB),y)
DPDK_PRIVATE_LINKER_ARGS += -lIPSec_MB
ifneq ($(IPSEC_MB_DIR),)
DPDK_PRIVATE_LINKER_ARGS += -L$(IPSEC_MB_DIR)
endif
endif

ifeq ($(CONFIG_HAVE_LIBBSD),y)
DPDK_PRIVATE_LINKER_ARGS += -lbsd
endif

ifeq ($(CONFIG_HAVE_LIBARCHIVE),y)
DPDK_PRIVATE_LINKER_ARGS += -larchive
endif

ifeq ($(CONFIG_CRYPTO),y)
ifeq ($(CONFIG_CRYPTO_MLX5),y)
DPDK_PRIVATE_LINKER_ARGS += -lmlx5 -libverbs
endif
endif

ifeq ($(CONFIG_VBDEV_COMPRESS),y)
DPDK_PRIVATE_LINKER_ARGS += -lisal -L$(ISAL_DIR)/.libs
ifeq ($(CONFIG_VBDEV_COMPRESS_MLX5),y)
DPDK_PRIVATE_LINKER_ARGS += -lmlx5 -libverbs
endif
endif

ifneq (,$(wildcard $(DPDK_INC_DIR)/rte_config.h))
ifneq (,$(shell grep -e "define RTE_LIBRTE_VHOST_NUMA 1" -e "define RTE_EAL_NUMA_AWARE_HUGEPAGES 1" $(DPDK_INC_DIR)/rte_config.h))
DPDK_PRIVATE_LINKER_ARGS += -lnuma
endif
endif

# DPDK built with meson puts those defines elsewhere
ifneq (,$(wildcard $(DPDK_INC_DIR)/rte_build_config.h))
ifneq (,$(shell grep -e "define RTE_LIBRTE_VHOST_NUMA 1" -e "define RTE_EAL_NUMA_AWARE_HUGEPAGES 1" $(DPDK_INC_DIR)/rte_build_config.h))
DPDK_PRIVATE_LINKER_ARGS += -lnuma
endif
endif

ifeq ($(OS),Linux)
DPDK_PRIVATE_LINKER_ARGS += -ldl
endif
ifeq ($(OS),FreeBSD)
DPDK_PRIVATE_LINKER_ARGS += -lexecinfo
endif

ifeq ($(CONFIG_SHARED),y)
ENV_DPDK_FILE = $(call spdk_lib_list_to_shared_libs,env_dpdk)
ENV_LIBS = $(ENV_DPDK_FILE) $(DPDK_SHARED_LIB)
DPDK_LINKER_ARGS = $(DPDK_SHARED_LIB_LINKER_ARGS)
ENV_LINKER_ARGS = $(ENV_DPDK_FILE) $(DPDK_LINKER_ARGS)
else
ENV_DPDK_FILE = $(call spdk_lib_list_to_static_libs,env_dpdk)
ENV_LIBS = $(ENV_DPDK_FILE) $(DPDK_STATIC_LIB)
DPDK_LINKER_ARGS = $(DPDK_STATIC_LIB_LINKER_ARGS)
ENV_LINKER_ARGS = $(ENV_DPDK_FILE) $(DPDK_LINKER_ARGS)
ENV_LINKER_ARGS += $(DPDK_PRIVATE_LINKER_ARGS)
endif
