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

DPDK_OPTS = -Denable_docs=false
DPDK_CFLAGS =

DPDK_KMODS = false
ifeq ($(CONFIG_IGB_UIO_DRIVER),y)
DPDK_KMODS = true
endif
ifeq ($(OS),FreeBSD)
DPDK_KMODS = true
endif
DPDK_OPTS += -Denable_kmods=$(DPDK_KMODS)

# the drivers we use
DPDK_DRIVERS = bus bus/pci bus/vdev mempool/ring

# common crypto/reduce drivers
ifeq ($(findstring y,$(CONFIG_CRYPTO)$(CONFIG_REDUCE)),y)
DPDK_DRIVERS += crypto/qat compress/qat common/qat
endif

ifeq ($(CONFIG_CRYPTO),y)
# crypto/qat is just a stub, the compress/qat pmd is used instead
DPDK_DRIVERS += crypto crypto/aesni_mb
DPDK_CFLAGS += -I$(IPSEC_MB_DIR)
DPDK_LDFLAGS += -L$(IPSEC_MB_DIR)
endif

ifeq ($(CONFIG_REDUCE),y)
DPDK_DRIVERS += compress compress/isal
DPDK_CFLAGS += -I$(ISAL_DIR)
DPDK_LDFLAGS += -L$(ISAL_DIR)/.libs -lisal
endif

DPDK_OPTS += -Dmachine=$(TARGET_ARCHITECTURE)

ifneq ($(CONFIG_CROSS_PREFIX),)
$(error Automatic DPDK cross build is not supported. Please compile DPDK manually \
with e.g. `meson build --cross-file config/arm/arm64_armv8_linux_gcc`)
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

ifeq ($(CC_TYPE),gcc)
GCC_MAJOR = $(shell echo __GNUC__ | $(CC) -E -x c - | tail -n 1)
ifeq ($(shell test $(GCC_MAJOR) -ge 10 && echo 1), 1)
#1. gcc 10 complains on operations with zero size arrays in rte_cryptodev.c, so
#disable this warning
#2. gcc 10 disables fcommon by default and complains on multiple definition of
#aesni_mb_logtype_driver symbol which is defined in header file and presented in sevral
#translation units
DPDK_CFLAGS += -Wno-stringop-overflow -fcommon
endif
endif

# Force-disable scan-build
SUB_CC = $(patsubst %ccc-analyzer,$(DEFAULT_CC),$(CC))

DPDK_ALL_DRIVER_DIRS = $(shell find $(SPDK_ROOT_DIR)/dpdk/drivers -mindepth 1 -type d)
DPDK_ALL_DRIVERS = $(DPDK_ALL_DRIVER_DIRS:$(SPDK_ROOT_DIR)/dpdk/drivers/%=%)
DPDK_DISABLED_DRVERS = $(filter-out $(DPDK_DRIVERS),$(DPDK_ALL_DRIVERS))

ifeq ($(OS),Linux)
SED_INPLACE_FLAG = "-i"
MESON_PREFIX = $(SPDK_ROOT_DIR)/dpdk/build
else
SED_INPLACE_FLAG = "-i ''"
MESON_PREFIX = "/"
endif

# Some ninja versions come with a (broken?) jobserver which defaults to use
# only 1 thread for the build. We workaround this by specifying -j to ninja
# with the same value as top-makefile. This is OK as long as DPDK is not built
# in parralel with anything else, which is the case for now.
ifeq ($(MAKE_PID),)
MAKE_PID := $(shell echo $$PPID)
endif

MAKE_NUMJOBS := $(shell ps T | sed -nE 's/\s*$(MAKE_PID)\s.* (-j|--jobs=)( *[0-9]+).*/\1\2/p')

all: $(SPDK_ROOT_DIR)/dpdk/build-tmp
	$(Q)# DPDK doesn't handle nested make calls, so unset MAKEFLAGS
	$(Q)env -u MAKEFLAGS ninja -C $(SPDK_ROOT_DIR)/dpdk/build-tmp $(MAKE_NUMJOBS)
	$(Q) \
	# Meson on FreeBSD sometimes appends --prefix value to the default DESTDIR (which is e.g. \
	# /usr/local) instead of replacing it. --prefix needs to be an absolute path, so we set \
	# it to / and then set DESTDIR directly, so libs and headers are copied to "DESTDIR//". \
	# DPDK kernel modules are set to install in $DESTDIR/boot/modules, but we move them \
	# to DESTDIR/kmod to be consistent with the makefile build. \
	# \
	# Also use meson install --only-changed instead of ninja install so that the shared \
	# libraries don't get reinstalled when they haven't been rebuilt - this avoids all of \
	# our applications getting relinked even when nothing has changed.
	$(Q)if [ "$(OS)" = "FreeBSD" ]; then \
		env -u MAKEFLAGS DESTDIR=$(SPDK_ROOT_DIR)/dpdk/build ninja -C $(SPDK_ROOT_DIR)/dpdk/build-tmp $(MAKE_NUMJOBS) install > /dev/null && \
		mv $(SPDK_ROOT_DIR)/dpdk/build/boot/modules $(SPDK_ROOT_DIR)/dpdk/build/kmod; \
	else \
		env -u MAKEFLAGS meson install -C $(SPDK_ROOT_DIR)/dpdk/build-tmp --only-changed > /dev/null; \
	fi

$(SPDK_ROOT_DIR)/dpdk/build-tmp: $(SPDK_ROOT_DIR)/mk/cc.mk $(SPDK_ROOT_DIR)/include/spdk/config.h
	$(Q)rm -rf $(SPDK_ROOT_DIR)/dpdk/build $(SPDK_ROOT_DIR)/dpdk/build-tmp
	$(Q)cd "$(SPDK_ROOT_DIR)/dpdk"; CC="$(SUB_CC)" meson --prefix="$(MESON_PREFIX)" --libdir lib -Dc_args="$(DPDK_CFLAGS)" -Dc_link_args="$(DPDK_LDFLAGS)" $(DPDK_OPTS) -Ddisable_drivers="$(shell echo $(DPDK_DISABLED_DRVERS) | sed -E "s/ +/,/g")" build-tmp
	$(Q)sed $(SED_INPLACE_FLAG) 's/#define RTE_EAL_PMD_PATH .*/#define RTE_EAL_PMD_PATH ""/g' $(SPDK_ROOT_DIR)/dpdk/build-tmp/rte_build_config.h
	$(Q) \
	# TODO Meson build adds libbsd dependency when it's available. This means any app will be \
	# forced to link with -lbsd, but only if it's available on the system. The clean way to \
	# handle this would be to rely on DPDK's pkg-config file which will contain the -lbsd when \
	# required. For now just remove the libbsd dependency. DPDK will fallback to its internal \
	# functions.
	$(Q)sed $(SED_INPLACE_FLAG) 's/#define RTE_USE_LIBBSD .*//g' $(SPDK_ROOT_DIR)/dpdk/build-tmp/rte_build_config.h

clean:
	$(Q)rm -rf $(SPDK_ROOT_DIR)/dpdk/build $(SPDK_ROOT_DIR)/dpdk/build-tmp

install:
	@:

uninstall:
	@:
