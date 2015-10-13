#
#  BSD LICENSE
#
#  Copyright(c) 2010-2015 Intel Corporation. All rights reserved.
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

include $(SPDK_ROOT_DIR)/CONFIG

C_OPT ?= -fno-omit-frame-pointer
Q ?= @
S ?= $(notdir $(CURDIR))

ifeq ($(MAKECMDGOALS),)
MAKECMDGOALS=$(.DEFAULT_GOAL)
endif

OS := $(shell uname)

COMMON_CFLAGS = -g $(C_OPT) -Wall -Werror -fno-strict-aliasing -march=native -m64 -I$(SPDK_ROOT_DIR)/include

COMMON_CFLAGS += -Wformat -Wformat-security -Wformat-nonliteral

# Always build PIC code so that objects can be used in shared libs and position-independent executables
COMMON_CFLAGS += -fPIC

# Enable stack buffer overflow checking
COMMON_CFLAGS += -fstack-protector

# Enable full RELRO - no lazy relocation (resolve everything at load time).
# This allows the GOT to be made read-only early in the loading process.
LDFLAGS += -Wl,-z,relro,-z,now

# Make the stack non-executable.
# This is the default in most environments, but it doesn't hurt to set it explicitly.
LDFLAGS += -Wl,-z,noexecstack

ifeq ($(OS),FreeBSD)
LIBS += -L/usr/local/lib
COMMON_CFLAGS += -I/usr/local/include
endif

ifeq ($(CONFIG_DEBUG), y)
COMMON_CFLAGS += -DDEBUG -O0
else
COMMON_CFLAGS += -DNDEBUG -O2
# Enable _FORTIFY_SOURCE checks - these only work when optimizations are enabled.
COMMON_CFLAGS += -D_FORTIFY_SOURCE=2
endif

CFLAGS   += $(COMMON_CFLAGS) -Wno-pointer-sign -std=gnu11

MAKEFLAGS += --no-print-directory

%.o : %.c
	@echo "  CC $@"
	$(Q)$(CC) $(CFLAGS) -c $<
	$(Q)$(CC) -MM $(CFLAGS) $*.c > $*.d
	@mv -f $*.d $*.d.tmp
	@sed -e 's|.*:|$*.o:|' < $*.d.tmp > $*.d
	@sed -e 's/.*://' -e 's/\\$$//' < $*.d.tmp | fmt -1 | \
		sed -e 's/^ *//' -e 's/$$/:/' >> $*.d
	@rm -f $*.d.tmp

DPDK_DIR ?= $(CONFIG_DPDK_DIR)
DPDK_INC_DIR ?= $(DPDK_DIR)/include
DPDK_LIB_DIR ?= $(DPDK_DIR)/lib

DPDK_INC = -I$(DPDK_INC_DIR)
DPDK_LIB = -L$(DPDK_LIB_DIR) -lrte_eal -lrte_malloc -lrte_mempool -lrte_ring -Wl,-rpath=$(DPDK_LIB_DIR)
# DPDK requires dl library for dlopen/dlclose on Linux.
ifeq ($(OS),Linux)
DPDK_LIB += -ldl
endif
ifeq ($(OS),FreeBSD)
DPDK_LIB += -lexecinfo
endif
