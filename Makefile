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

S :=

SPDK_ROOT_DIR := $(CURDIR)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

DIRS-y += lib

.PHONY: all clean $(DIRS-y) config.h CONFIG.local mk/cc.mk

ifeq ($(CURDIR)/dpdk/build,$(CONFIG_DPDK_DIR))
DPDKBUILD = dpdkbuild
DIRS-y += dpdkbuild
endif

all: $(DIRS-y)
clean: $(DIRS-y)
	$(Q)rm -f mk/cc.mk
	$(Q)rm -f config.h

lib: $(DPDKBUILD)
app: lib
test: lib
examples: lib

$(DIRS-y): mk/cc.mk config.h

mk/cc.mk:
	$(Q)scripts/detect_cc.sh --cc=$(CC) --cxx=$(CXX) --lto=$(CONFIG_LTO) > $@.tmp; \
	cmp -s $@.tmp $@ || mv $@.tmp $@ ; \
	rm -f $@.tmp

config.h: CONFIG CONFIG.local scripts/genconfig.py
	$(Q)python scripts/genconfig.py $(MAKEFLAGS) > $@.tmp; \
	cmp -s $@.tmp $@ || mv $@.tmp $@ ; \
	rm -f $@.tmp

include $(SPDK_ROOT_DIR)/mk/spdk.subdirs.mk
