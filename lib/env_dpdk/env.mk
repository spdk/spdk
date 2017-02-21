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

DPDK_DIR ?= $(CONFIG_DPDK_DIR)

ifeq ($(DPDK_DIR), )
ifeq ($(OS),FreeBSD)
export DPDK_ABS_DIR = /usr/local/share/dpdk/x86_64-native-freebsdapp-clang
else
export DPDK_ABS_DIR = /usr/local/share/dpdk/x86_64-native-linuxapp-gcc
endif
else
export DPDK_ABS_DIR = $(abspath $(DPDK_DIR))
endif

ifneq (, $(wildcard $(DPDK_ABS_DIR)/include/rte_config.h))
DPDK_INC = -I$(DPDK_ABS_DIR)/include
else
DPDK_INC = -I$(DPDK_ABS_DIR)/include/dpdk
endif

ifneq (, $(wildcard $(DPDK_ABS_DIR)/lib/librte_eal.a))
DPDK_LIB_EXT = .a
else
DPDK_LIB_EXT = .so
endif

DPDK_LIB_LIST = rte_eal rte_mempool rte_ring

# librte_malloc was removed after DPDK 2.1.  Link this library conditionally based on its
#  existence to maintain backward compatibility.
ifneq ($(wildcard $(DPDK_ABS_DIR)/lib/librte_malloc.*),)
DPDK_LIB_LIST += rte_malloc
endif

DPDK_LIB = $(DPDK_LIB_LIST:%=$(DPDK_ABS_DIR)/lib/lib%$(DPDK_LIB_EXT))

ENV_CFLAGS = $(DPDK_INC)
ENV_CXXFLAGS = $(ENV_CFLAGS)
ENV_DPDK_FILE = $(call spdk_lib_list_to_files,env_dpdk)
ENV_LIBS = $(ENV_DPDK_FILE) $(DPDK_LIB)
ENV_LINKER_ARGS = $(ENV_DPDK_FILE) -Wl,--start-group -Wl,--whole-archive $(DPDK_LIB) -Wl,--end-group -Wl,--no-whole-archive

ifeq ($(OS),Linux)
ENV_LINKER_ARGS += -ldl
endif
ifeq ($(OS),FreeBSD)
ENV_LINKER_ARGS += -lexecinfo
endif
