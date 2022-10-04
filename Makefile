#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  Copyright (c) 2020, Mellanox Corporation.
#  Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES
#  All rights reserved.
#

S :=

SPDK_ROOT_DIR := $(CURDIR)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

DIRS-y += lib
DIRS-y += module
DIRS-$(CONFIG_SHARED) += shared_lib
DIRS-y += include
DIRS-$(CONFIG_EXAMPLES) += examples
DIRS-$(CONFIG_APPS) += app
DIRS-y += test
DIRS-$(CONFIG_IPSEC_MB) += ipsecbuild
DIRS-$(CONFIG_ISAL) += isalbuild
DIRS-$(CONFIG_ISAL_CRYPTO) += isalcryptobuild
DIRS-$(CONFIG_VFIO_USER) += vfiouserbuild
DIRS-$(CONFIG_SMA) += proto
DIRS-$(CONFIG_XNVME) += xnvmebuild

.PHONY: all clean $(DIRS-y) include/spdk/config.h mk/config.mk \
	cc_version cxx_version .libs_only_other .ldflags ldflags install \
	uninstall

# Workaround for ninja. See dpdkbuild/Makefile
export MAKE_PID := $(shell echo $$PPID)

ifeq ($(SPDK_ROOT_DIR)/lib/env_dpdk,$(CONFIG_ENV))
ifeq ($(CURDIR)/dpdk/build,$(CONFIG_DPDK_DIR))
ifneq ($(SKIP_DPDK_BUILD),1)
ifneq ($(CONFIG_DPDK_PKG_CONFIG),y)
DPDKBUILD = dpdkbuild
DIRS-y += dpdkbuild
endif
endif
endif
endif

ifeq ($(OS),Windows)
ifeq ($(CURDIR)/wpdk/build,$(CONFIG_WPDK_DIR))
WPDK = wpdk
DIRS-y += wpdk
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
ISALBUILD = isalbuild
LIB += isalbuild
DPDK_DEPS += isalbuild
ifeq ($(CONFIG_ISAL_CRYPTO),y)
ISALCRYPTOBUILD = isalcryptobuild
LIB += isalcryptobuild
endif
endif

ifeq ($(CONFIG_VFIO_USER),y)
VFIOUSERBUILD = vfiouserbuild
LIB += vfiouserbuild
endif

ifeq ($(CONFIG_XNVME),y)
XNVMEBUILD = xnvmebuild
LIB += xnvmebuild
endif

all: mk/cc.mk $(DIRS-y)
clean: $(DIRS-y)
	$(Q)rm -f include/spdk/config.h
	$(Q)rm -rf build

install: all
	$(Q)echo "Installed to $(DESTDIR)$(CONFIG_PREFIX)"

uninstall: $(DIRS-y)
	$(Q)echo "Uninstalled spdk"

ifneq ($(SKIP_DPDK_BUILD),1)
dpdkdeps $(DPDK_DEPS): $(WPDK)
dpdkbuild: $(WPDK) $(DPDK_DEPS)
endif

lib: $(WPDK) $(DPDKBUILD) $(VFIOUSERBUILD) $(XNVMEBUILD) $(ISALBUILD) $(ISALCRYPTOBUILD)
module: lib
shared_lib: module
app: $(LIB)
test: $(LIB)
examples: $(LIB)
pkgdep:
	sh ./scripts/pkgdep.sh

$(DIRS-y): mk/cc.mk build_dir include/spdk/config.h

mk/cc.mk:
	$(Q)echo "Please run configure prior to make"
	false

build_dir: mk/cc.mk
	$(Q)mkdir -p build/lib/pkgconfig/tmp
	$(Q)mkdir -p build/bin
	$(Q)mkdir -p build/fio
	$(Q)mkdir -p build/examples
	$(Q)mkdir -p build/include/spdk

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
