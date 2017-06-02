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

.PHONY: all clean

DPDK_CONFIG := spdk

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

ifeq ($(CONFIG_FIO_PLUGIN),y)
DPDK_CFLAGS = -fPIC
endif

ifeq ($(CONFIG_DEBUG),y)
DPDK_CFLAGS += -O0 -g
endif

ifeq ($(CONFIG_WERROR),y)
DPDK_CFLAGS += -Werror
else
DPDK_CFLAGS += -Wno-error
endif

$(SPDK_ROOT_DIR)/dpdk/build:
	$(Q)$(MAKE) -C $(SPDK_ROOT_DIR)/dpdk config T=$(DPDK_CONFIG)

all: $(SPDK_ROOT_DIR)/dpdk/build
	$(Q)$(MAKE) -C $(SPDK_ROOT_DIR)/dpdk/build EXTRA_CFLAGS="$(DPDK_CFLAGS)" MAKEFLAGS="T=$(DPDK_CONFIG) -j$(NPROC)"

clean:
	$(Q)rm -rf $(SPDK_ROOT_DIR)/dpdk/build
