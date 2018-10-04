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

SPDK_ROOT_DIR := $(abspath $(CURDIR)/..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

.PHONY: all clean install

IPSEC_OPTS=SHARED=n
INTEL_IPSEC_MB_LIB=
CRYPTO_ENABLED=n

ifeq ($(CONFIG_CRYPTO),y)
ifeq ($(TARGET_MACHINE),x86_64)
ifneq ($(wildcard $(SPDK_ROOT_DIR)/intel-ipsec-mb/README),)
INTEL_IPSEC_MB_LIB=$(SPDK_ROOT_DIR)/intel-ipsec-mb/libIPSec_MB.a
IPSEC_MB_CFLAGS = -fPIC
CRYPTO_ENABLED = y
DPDK_OPTS += CONFIG_RTE_LIBRTE_PMD_AESNI_MB=y
DPDK_OPTS += CONFIG_RTE_LIBRTE_CRYPTODEV=y
DPDK_OPTS += CONFIG_RTE_LIBRTE_REORDER=y
DPDK_OPTS += CONFIG_RTE_LIBRTE_PMD_QAT=y
DPDK_OPTS += CONFIG_RTE_LIBRTE_PMD_QAT_SYM=y
ifeq ($(CONFIG_IGB_UIO_DRIVER),y)
DPDK_OPTS += CONFIG_RTE_EAL_IGB_UIO=y
else
DPDK_OPTS += CONFIG_RTE_EAL_IGB_UIO=n
endif
endif
endif
endif

ifeq ($(CRYPTO_ENABLED),n)
DPDK_OPTS += CONFIG_RTE_EAL_IGB_UIO=n
DPDK_OPTS += CONFIG_RTE_LIBRTE_PMD_AESNI_MB=n
DPDK_OPTS += CONFIG_RTE_LIBRTE_CRYPTODEV=n
DPDK_OPTS += CONFIG_RTE_LIBRTE_REORDER=n
DPDK_OPTS += CONFIG_RTE_LIBRTE_PMD_QAT=n
DPDK_OPTS += CONFIG_RTE_LIBRTE_PMD_QAT_SYM=n
endif

ifeq ($(TARGET_MACHINE),aarch64)
DPDK_CONFIG := arm64-armv8a
else
DPDK_CONFIG := $(TARGET_MACHINE)-native
endif

ifeq ($(OS),Linux)
DPDK_CONFIG := $(DPDK_CONFIG)-linuxapp
NPROC := $(shell nproc)
else
ifeq ($(OS),FreeBSD)
DPDK_CONFIG := $(DPDK_CONFIG)-bsdapp
NPROC := $(shell sysctl hw.ncpu | awk '{print $$NF}')
endif
endif

ifeq ($(CC_TYPE),clang)
DPDK_CONFIG := $(DPDK_CONFIG)-clang
IPSEC_CC := clang
else
DPDK_CONFIG := $(DPDK_CONFIG)-gcc
IPSEC_CC := gcc
endif

DPDK_CFLAGS = -fPIC

ifeq ($(CONFIG_DEBUG),y)
DPDK_CFLAGS += -O0 -g
IPSEC_OPTS += DEBUG=y
endif

ifeq ($(CONFIG_WERROR),y)
DPDK_CFLAGS += -Werror
else
DPDK_CFLAGS += -Wno-error
endif

ifdef EXTRA_DPDK_CFLAGS
$(warning EXTRA_DPDK_CFLAGS defined, possibly to work around an unsupported compiler version)
$(shell sleep 1)
endif

# Allow users to specify EXTRA_DPDK_CFLAGS if they want to build DPDK using unsupported compiler versions
DPDK_CFLAGS += $(EXTRA_DPDK_CFLAGS)

$(SPDK_ROOT_DIR)/dpdk/build: $(INTEL_IPSEC_MB_LIB)
	$(Q)$(MAKE) -C $(SPDK_ROOT_DIR)/dpdk config T=$(DPDK_CONFIG) $(DPDK_OPTS)

all: $(SPDK_ROOT_DIR)/dpdk/build $(INTEL_IPSEC_MB_LIB)
	$(Q)$(MAKE) -C $(SPDK_ROOT_DIR)/dpdk/build EXTRA_CFLAGS="$(DPDK_CFLAGS)" MAKEFLAGS="T=$(DPDK_CONFIG) -j$(NPROC)" $(DPDK_OPTS)

ifeq ($(CONFIG_CRYPTO),y)
$(SPDK_ROOT_DIR)/intel-ipsec-mb/libIPSec_MB.a:
	$(Q)$(MAKE) -C $(SPDK_ROOT_DIR)/intel-ipsec-mb EXTRA_CFLAGS="$(IPSEC_MB_CFLAGS)" $(IPSEC_OPTS) MAKEFLAGS="j$(NPROC)" CC=$(IPSEC_CC)
endif

clean:
	$(Q)rm -rf $(SPDK_ROOT_DIR)/dpdk/build; \
	rm -rf $(SPDK_ROOT_DIR)/intel-ipsec-mb/obj/*; \
	rm -rf $(SPDK_ROOT_DIR)/intel-ipsec-mb/libIPSec_MB.a

install: all
