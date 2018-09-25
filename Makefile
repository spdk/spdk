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
DIRS-$(CONFIG_SHARED) += shared_lib
DIRS-y += examples app include
DIRS-$(CONFIG_TESTS) += test

.PHONY: all clean $(DIRS-y) config.h mk/config.mk mk/cc.mk cc_version cxx_version

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
LIB = lib
endif

all: $(DIRS-y)
clean: $(DIRS-y)
	$(Q)rm -f mk/cc.mk
	$(Q)rm -f config.h

install: all
	$(Q)echo "Installed to $(DESTDIR)$(CONFIG_PREFIX)"

shared_lib: lib
lib: $(DPDKBUILD)
app: $(LIB)
test: $(LIB)
examples: $(LIB)
pkgdep:
	sh ./scripts/pkgdep.sh

$(DIRS-y): mk/cc.mk config.h

mk/cc.mk:
	$(Q)scripts/detect_cc.sh --cc=$(CC) --cxx=$(CXX) --lto=$(CONFIG_LTO) > $@.tmp; \
	cmp -s $@.tmp $@ || mv $@.tmp $@ ; \
	rm -f $@.tmp

# Transform config.mk into config.h by:
# 1. Apply command line variables by directly replacing CONFIG_XXX lines.
# 2. Remove empty lines and comments
# 3. n or empty option -> undef
# 4. y -> define 1
# 5. Other values (like paths) -> define value
# 6. Perform sanity check: compare number of option from config.mk with these config.h.tmp
# 7. Add sentinels
config.h: mk/config.mk
	$(Q)cp mk/config.mk $@.tmp
	$(Q)for cfg in $(filter CONFIG_%,$(MAKEFLAGS)); do \
		sed -i.bak -r "s@$${cfg%%=*}[?]?=.*@$$cfg@g" $@.tmp; \
	done

	$(Q)sed -i.bak -r '/^\s*(#.*)?$$/d' $@.tmp
	$(Q)sed -i.bak -r 's/(CONFIG_[[:alnum:]_]+)[?]?=(n)?\s*$$/\#undef SPDK_\1/g' $@.tmp
	$(Q)sed -i.bak -r 's/(CONFIG_[[:alnum:]_]+)[?]?=y/\#define SPDK_\1 1/g' $@.tmp
	$(Q)sed -i.bak -r 's/(CONFIG_[[:alnum:]_]+)[?]?=(.+)/\#define SPDK_\1 \2/g' $@.tmp

	$(Q)config_mk_cnt=`egrep -c '^\\s*CONFIG_[[:alnum:]_]+\\?=' mk/config.mk`; \
	config_h_cnt=`egrep -c '^#(define|undef) SPDK_CONFIG' $@.tmp`; \
	if [ $$config_mk_cnt -ne $$config_h_cnt ]; then \
		echo ''; \
		echo 'BUG: Number of options from CONFIG not equal to $@.tmp'; \
		echo ''; \
		exit 1; \
	fi;

	$(Q)echo '#ifndef SPDK_CONFIG_H' > $@.tmp.bak
	$(Q)echo '#define SPDK_CONFIG_H' >> $@.tmp.bak
	$(Q)echo '' >> $@.tmp.bak
	$(Q)cat $@.tmp >> $@.tmp.bak
	$(Q)echo '' >> $@.tmp.bak
	$(Q)echo '#endif /* SPDK_CONFIG_H */' >> $@.tmp.bak

	$(Q)mv $@.tmp.bak $@.tmp
	$(Q)cmp -s $@.tmp $@ || cp $@.tmp $@
	$(Q)rm -f $@.tmp

cc_version: mk/cc.mk
	$(Q)echo "SPDK using CC=$(CC)"; $(CC) -v

cxx_version: mk/cc.mk
	$(Q)echo "SPDK using CXX=$(CXX)"; $(CXX) -v

include $(SPDK_ROOT_DIR)/mk/spdk.subdirs.mk
