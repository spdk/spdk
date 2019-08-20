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

# OCF requires users to build with their sources
# If SPDK is configured with OCF source directory,
# we export its files and then compile SPDK LIB with them
# Else if SPDK is configured with OCF precompiled library
# we just use it as SPDK lib by copying it to /build/lib/

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
OCFDIR=$(CONFIG_OCF_DIR)

include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

LIBNAME := ocfenv

CFLAGS +=  $(ENV_CFLAGS) -I$(CURDIR) -I$(CURDIR)/include -w
C_SRCS = $(shell find -name \*.c)

LIB = $(call spdk_lib_list_to_static_libs,$(LIBNAME))


ifeq ($(CONFIG_CUSTOMOCF),y)

.PHONY: all clean install

all:
	$(Q)$(MAKE) $(LIB)

clean:
	$(Q)rm -f $(LIB)

$(LIB):
	cp $(CONFIG_OCF_PATH) $(LIB)

install:

uninstall:
	$(UNINSTALL_LIB)

else

.PHONY: all clean install ocf_inc ocf_src ocf_distclean all exportlib

all: ocf_inc ocf_src
	$(Q)$(MAKE) $(LIB)

ocf_inc:
	$(Q)$(MAKE) -C "$(CONFIG_OCF_PATH)" inc O="$(SPDK_ROOT_DIR)/lib/env_ocf/" ENV= --quiet

ocf_src: ocf_inc
	$(Q)$(MAKE) -C "$(CONFIG_OCF_PATH)" src O="$(SPDK_ROOT_DIR)/lib/env_ocf/" CMD=cp ENV= --quiet

ocf_distclean:
	$(Q)$(MAKE) -C "$(CONFIG_OCF_PATH)" distclean O="$(SPDK_ROOT_DIR)/lib/env_ocf/" ENV= --quiet

clean: ocf_distclean
	$(Q)rm -rf	"$(SPDK_ROOT_DIR)/lib/env_ocf/include" \
				"$(SPDK_ROOT_DIR)/lib/env_ocf/src" \
				$(LIB) $(OBJS);

$(LIB): $(OBJS)
	$(LIB_C)

install:

uninstall:
	$(UNINSTALL_LIB)

endif

exportlib: all
	@ if [ -z $(O) ]; then echo "No output specified"; exit 1; fi
	cp $(LIB) $(O)

help:
	@ echo "all                     Default"
	@ echo "exportlib O=<outpath>   Default build to specified outpath"
