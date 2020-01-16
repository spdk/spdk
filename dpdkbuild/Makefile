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

.PHONY: all clean install uninstall

DPDK_FRAMEWORK = n
DPDK_OPTS =
DPDK_CFLAGS =

ifeq ($(CONFIG_SHARED),y)
DPDK_OPTS += CONFIG_RTE_BUILD_SHARED_LIB=y
DPDK_LDFLAGS+= -rpath $(SPDK_ROOT_DIR)/dpdk/build/lib
else
DPDK_OPTS += CONFIG_RTE_BUILD_SHARED_LIB=n
endif

ifeq ($(CONFIG_CRYPTO),y)
DPDK_FRAMEWORK = y
DPDK_OPTS += CONFIG_RTE_LIBRTE_PMD_AESNI_MB=y
DPDK_OPTS += CONFIG_RTE_LIBRTE_REORDER=y
DPDK_CFLAGS += -I$(IPSEC_MB_DIR)
DPDK_LDFLAGS += -L$(IPSEC_MB_DIR)
else
DPDK_OPTS += CONFIG_RTE_LIBRTE_PMD_AESNI_MB=n
DPDK_OPTS += CONFIG_RTE_LIBRTE_REORDER=n
endif

ifeq ($(CONFIG_REDUCE),y)
DPDK_FRAMEWORK = y
DPDK_OPTS += CONFIG_RTE_LIBRTE_PMD_ISAL=y
DPDK_CFLAGS += -I$(ISAL_DIR)
DPDK_LDFLAGS += -L$(ISAL_DIR)/.libs
else
DPDK_OPTS += CONFIG_RTE_LIBRTE_PMD_ISAL=n
endif

ifeq ($(CONFIG_VHOST),y)
DPDK_OPTS += CONFIG_RTE_LIBRTE_ETHER=y
DPDK_OPTS += CONFIG_RTE_LIBRTE_CMDLINE=y
DPDK_OPTS += CONFIG_RTE_LIBRTE_METER=y
DPDK_OPTS += CONFIG_RTE_LIBRTE_HASH=y
DPDK_OPTS += CONFIG_RTE_LIBRTE_VHOST=y
else
DPDK_OPTS += CONFIG_RTE_LIBRTE_ETHER=n
DPDK_OPTS += CONFIG_RTE_LIBRTE_CMDLINE=n
DPDK_OPTS += CONFIG_RTE_LIBRTE_METER=n
DPDK_OPTS += CONFIG_RTE_LIBRTE_HASH=n
DPDK_OPTS += CONFIG_RTE_LIBRTE_VHOST=n
endif

ifeq ($(DPDK_FRAMEWORK),y)
DPDK_OPTS += CONFIG_RTE_LIBRTE_PMD_QAT=y
DPDK_OPTS += CONFIG_RTE_LIBRTE_PMD_QAT_SYM=y
ifeq ($(CONFIG_IGB_UIO_DRIVER),y)
DPDK_OPTS += CONFIG_RTE_EAL_IGB_UIO=y
else
DPDK_OPTS += CONFIG_RTE_EAL_IGB_UIO=n
endif
else
DPDK_OPTS += CONFIG_RTE_LIBRTE_PMD_QAT=n
DPDK_OPTS += CONFIG_RTE_LIBRTE_PMD_QAT_SYM=n
endif

ifeq ($(TARGET_MACHINE),aarch64)
DPDK_CONFIG := arm64-armv8a
else
DPDK_CONFIG := $(TARGET_MACHINE)-native
endif

ifneq ($(CONFIG_CROSS_PREFIX),)
DPDK_OPTS += CROSS=$(CONFIG_CROSS_PREFIX)-
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
else
DPDK_CONFIG := $(DPDK_CONFIG)-gcc
endif

DPDK_CFLAGS += -fPIC

ifeq ($(CONFIG_DEBUG),y)
DPDK_CFLAGS += -O0 -g
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

$(DPDK_DIR):
	$(Q)rm -rf $(DPDK_DIR)
	$(Q)$(MAKE) -C "$(shell dirname $(DPDK_DIR))" config T=$(DPDK_CONFIG) $(DPDK_OPTS)

all: $(DPDK_DIR)
	$(Q)$(MAKE) -C $(DPDK_DIR) EXTRA_CFLAGS="$(DPDK_CFLAGS)" EXTRA_LDFLAGS="$(DPDK_LDFLAGS)" MAKEFLAGS="T=$(DPDK_CONFIG) -j$(NPROC)" $(DPDK_OPTS)

dpdkbuild: all

clean:
	$(Q)rm -rf $(DPDK_DIR)

install: all

uninstall:
	@:
