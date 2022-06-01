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

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 4
SO_MINOR := 0

ifdef SPDK_FTL_VSS_EMU
CFLAGS += -DSPDK_FTL_VSS_EMU
endif

ifdef SPDK_FTL_L2P_FLAT
CFLAGS += -DSPDK_FTL_L2P_FLAT
endif

ifneq ($(strip $(SPDK_FTL_ZONE_EMU_BLOCKS)),)
CFLAGS += -DSPDK_FTL_ZONE_EMU_BLOCKS=$(SPDK_FTL_ZONE_EMU_BLOCKS)
endif

CFLAGS += -I.

FTL_SUBDIRS := mngt utils

C_SRCS = $(wildcard *.c)
C_SRCS += $(wildcard mngt/*.c)
C_SRCS += $(wildcard utils/*.c)

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_ftl.map)

LIBNAME = ftl

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk

CLEAN_FTL_SUBDIRS = $(addprefix clean_ftl_, $(FTL_SUBDIRS))

clean: $(CLEAN_FTL_SUBDIRS)

.PHONY: $(CLEAN_FTL_SUBDIRS)
$(CLEAN_FTL_SUBDIRS): _Q := $(Q)
$(CLEAN_FTL_SUBDIRS): Q :=
$(CLEAN_FTL_SUBDIRS): clean_ftl_%:%
	$(_Q)cd $< && $(CLEAN_C)
