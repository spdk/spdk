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
DIRS-y += module
DIRS-$(CONFIG_SHARED) += shared_lib
DIRS-y += examples app include
DIRS-$(CONFIG_TESTS) += test
DIRS-$(CONFIG_IPSEC_MB) += ipsecbuild
DIRS-$(CONFIG_ISAL) += isalbuild

.PHONY: all clean $(DIRS-y) include/spdk/config.h mk/config.mk \
	cc_version cxx_version .libs_only_other .ldflags ldflags install \
	uninstall

ifeq ($(SPDK_ROOT_DIR)/lib/env_dpdk,$(CONFIG_ENV))
ifeq ($(CURDIR)/dpdk/build,$(CONFIG_DPDK_DIR))
ifneq ($(SKIP_DPDK_BUILD),1)
DPDKBUILD = dpdkbuild
DIRS-y += dpdkbuild
endif
endif
endif

ifeq ($(CONFIG_SHARED),y)
LIB = shared_lib
else
LIB = module
endif

ifeq ($(CONFIG_IPSEC_MB),y)
LIB += ipsecbuild
DPDK_DEPS += ipsecbuild
endif

ifeq ($(CONFIG_ISAL),y)
LIB += isalbuild
DPDK_DEPS += isalbuild
endif

all: mk/cc.mk $(DIRS-y)
clean: $(DIRS-y)
	$(Q)rm -f include/spdk/config.h

install: all
	$(Q)echo "Installed to $(DESTDIR)$(CONFIG_PREFIX)"

uninstall: $(DIRS-y)
	$(Q)echo "Uninstalled spdk"

ifneq ($(SKIP_DPDK_BUILD),1)
dpdkbuild: $(DPDK_DEPS)
endif

lib: $(DPDKBUILD)
module: lib
shared_lib: module
app: $(LIB)
test: $(LIB)
examples: $(LIB)
pkgdep:
	sh ./scripts/pkgdep.sh

$(DIRS-y): include/spdk/config.h

mk/cc.mk:
	$(Q)echo "Please run configure prior to make"
	false

include/spdk/config.h: mk/config.mk scripts/genconfig.py
	$(Q)echo "#ifndef SPDK_CONFIG_H" > $@.tmp; \
	echo "#define SPDK_CONFIG_H" >> $@.tmp; \
	scripts/genconfig.py $(MAKEFLAGS) >> $@.tmp; \
	echo "#endif /* SPDK_CONFIG_H */" >> $@.tmp; \
	cmp -s $@.tmp $@ || mv $@.tmp $@ ; \
	rm -f $@.tmp

cc_version: mk/cc.mk
	$(Q)echo "SPDK using CC=$(CC)"; $(CC) -v

cxx_version: mk/cc.mk
	$(Q)echo "SPDK using CXX=$(CXX)"; $(CXX) -v

.libs_only_other:
	$(Q)echo -n '$(SYS_LIBS) '
	$(Q)if [ "$(CONFIG_SHARED)" = "y" ]; then \
		echo -n '-lspdk '; \
	fi

.ldflags:
	$(Q)echo -n '$(LDFLAGS) '

ldflags: .ldflags .libs_only_other
	$(Q)echo ''

include $(SPDK_ROOT_DIR)/mk/spdk.subdirs.mk
