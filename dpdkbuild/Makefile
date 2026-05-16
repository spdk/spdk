#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2017 Intel Corporation.
#  All rights reserved.
#  Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

.PHONY: all clean install uninstall

DPDK_OPTS = -Denable_docs=false
DPDK_OPTS += -Dtests=false

ifeq ($(shell uname -m),aarch64)
ifeq ($(shell uname -p),unknown)
# Unknown ARM platform. ARM doesn't do platform=native which is DPDK's default. So change it.
DPDK_OPTS += -Dplatform=generic
else
ifeq ($(shell $(SPDK_ROOT_DIR)/dpdk/config/arm/armv8_machine.py | cut -d' ' -f4),0x0)
DPDK_OPTS += -Dplatform=generic
endif
endif
endif

ifeq ($(CONFIG_DEBUG),y)
DPDK_OPTS += --buildtype=debug
endif

ifeq ($(CONFIG_SHARED),y)
DPDK_OPTS += --default-library=shared
else
DPDK_OPTS += --default-library=static
endif

ifeq ($(CONFIG_LTO),y)
# dpdk complains about compiler not supporting fat LTO objects when clang is in use so don't enable it in such a instance
ifneq ($(CC_TYPE),clang)
DPDK_OPTS += -Db_lto=true
endif
endif

ifeq ($(CONFIG_PGO_CAPTURE),y)
DPDK_OPTS += -Db_pgo=generate
endif

ifeq ($(CONFIG_PGO_USE),y)
DPDK_OPTS += -Db_pgo=use
DPDK_CFLAGS += -Wno-missing-profile
endif

ifneq ($(CONFIG_PGO_DIR),)
DPDK_CFLAGS += -fprofile-dir=$(CONFIG_PGO_DIR)
DPDK_LDFLAGS += -fprofile-dir=$(CONFIG_PGO_DIR)
endif

# the drivers we use
DPDK_DRIVERS = bus bus/pci bus/vdev mempool/ring

# Core DPDK libs - used by SPDK directly
DPDK_LIBS = eal hash log mbuf mempool pci ring
# Depdencies of core DPDK libs
DPDK_LIBS += argparse cmdline ethdev kvargs meter net rcu stack telemetry

# Governor required libs
ifeq ($(OS),Linux)
DPDK_LIBS += power timer
# rte_power drivers
DPDK_DRIVERS += power/acpi power/amd_pstate power/cppc power/intel_pstate power/intel_uncore \
		power/kvm_vm
endif

# Both crypto and compress QAT drivers are bunched up in common/qat. Crypto and compress drivers can only be disabled,
# but not explicitly enabled. Rather than trying to separate them out, enable both and their dependencies.
ifeq ($(or $(filter y,$(CONFIG_CRYPTO)),$(filter y,$(CONFIG_DPDK_COMPRESSDEV))),y)
DPDK_DRIVERS += crypto/qat compress/qat common/qat
DPDK_LIBS += compressdev cryptodev security
endif

# uadk drivers
ifeq ($(CONFIG_DPDK_UADK),y)
DPDK_DRIVERS += crypto/uadk
DPDK_DRIVERS += compress/uadk
endif
ifeq ($(CONFIG_CRYPTO),y)
DPDK_DRIVERS += crypto
DPDK_LIBS += cryptodev
ifeq ($(CONFIG_CRYPTO_MLX5),y)
DPDK_DRIVERS += common/mlx5 bus/auxiliary crypto/mlx5
endif
ifeq ($(CONFIG_IPSEC_MB),y)
DPDK_DRIVERS += crypto/ipsec_mb
DPDK_LIBS += security
DPDK_CFLAGS += -I$(IPSEC_MB_DIR)
DPDK_LDFLAGS += -L$(IPSEC_MB_DIR)
endif
endif

ifeq ($(CONFIG_DPDK_COMPRESSDEV),y)
DPDK_DRIVERS += compress compress/isal
DPDK_LIBS += compressdev
ifeq ($(CONFIG_ISAL_PATH),)
DPDK_CFLAGS += -I$(ISAL_DIR) -I$(ISAL_BUILD_DIR)
DPDK_LDFLAGS += -L$(ISAL_DIR)/.libs -lisal
else
DPDK_CFLAGS += -I$(ISAL_DIR)/include
DPDK_LDFLAGS += -L$(ISAL_DIR)/lib64 -lisal
endif
endif

DPDK_ENABLED_DRIVERS = $(shell echo $(DPDK_DRIVERS) | sed -E "s/ +/,/g")

# vhost and deps
ifeq ($(CONFIG_VHOST),y)
DPDK_LIBS += vhost cryptodev dmadev
endif

DPDK_OPTS += -Dcpu_instruction_set=$(TARGET_ARCHITECTURE)

ifneq ($(CONFIG_CROSS_PREFIX),)
ifeq ($(findstring mingw,$(CONFIG_CROSS_PREFIX)),mingw)
DPDK_OPTS += --cross-file $(SPDK_ROOT_DIR)/dpdk/config/x86/cross-mingw
else
$(error Automatic DPDK cross build is not supported. Please compile DPDK manually \
with e.g. `meson build --cross-file config/arm/arm64_armv8_linux_gcc`)
endif
endif

# DPDK supports ASAN since version 21.11
ifeq ($(CONFIG_ASAN),y)
ifneq ($(OS),Windows)
DPDK_OPTS += -Db_sanitize=address
ifeq ($(CC_TYPE),clang)
DPDK_OPTS += -Db_lundef=false
endif
endif
endif

DPDK_CFLAGS += -fPIC

ifneq ($(CONFIG_MAX_LCORES),)
DPDK_OPTS += -Dmax_lcores=$(CONFIG_MAX_LCORES)
endif

ifeq ($(CONFIG_WERROR),y)
DPDK_CFLAGS += -Werror
else
DPDK_CFLAGS += -Wno-error
endif

ifeq ($(CONFIG_CET),y)
DPDK_CFLAGS += -fcf-protection
DPDK_LDFLAGS += -fcf-protection
endif

ifdef EXTRA_DPDK_CFLAGS
$(warning EXTRA_DPDK_CFLAGS defined, possibly to work around an unsupported compiler version)
$(shell sleep 1)
endif

# Allow users to specify EXTRA_DPDK_CFLAGS if they want to build DPDK using unsupported compiler versions
DPDK_CFLAGS += $(EXTRA_DPDK_CFLAGS)

# Force-disable scan-build
SUB_CC = $(patsubst %ccc-analyzer,$(DEFAULT_CC),$(CC))

DPDK_ENABLED_LIBS = $(shell echo $(DPDK_LIBS) | sed -E "s/ +/,/g")

ifneq ($(OS),FreeBSD)
SED_INPLACE_FLAG = "-i"
MESON_PREFIX = $(SPDK_ROOT_DIR)/dpdk/build
else
SED_INPLACE_FLAG = "-i ''"
MESON_PREFIX = "/"
endif

# Some ninja versions come with a (broken?) jobserver which defaults to use
# only 1 thread for the build. We workaround this by specifying -j to ninja
# with the same value as top-makefile. This is OK as long as DPDK is not built
# in parallel with anything else, which is the case for now.
ifeq ($(MAKE_PID),)
MAKE_PID := $(shell echo $$PPID)
endif

MAKE_NUMJOBS := $(shell ps T | sed -nE 's/[[:space:]]*$(MAKE_PID)[[:space:]].* (-j|--jobs=)( *[0-9]+).*/\1\2/p')

ifeq ($(OS),Linux)
# FIXME: set path to allow meson build in Linux environment
virtdir=$(PIP_VIRTDIR)
ifeq ($(virtdir),)
virtdir=/var/spdk/dependencies/pip
endif
export PATH := $(virtdir)/bin:$(PATH)
endif

all: $(SPDK_ROOT_DIR)/dpdk/build-tmp
	$(Q)# DPDK doesn't handle nested make calls, so unset MAKEFLAGS
	$(Q)env -u MAKEFLAGS meson compile -C $(SPDK_ROOT_DIR)/dpdk/build-tmp $(MAKE_NUMJOBS)
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
	$(Q)cd "$(SPDK_ROOT_DIR)/dpdk"; CC="$(SUB_CC)" meson setup --prefix="$(MESON_PREFIX)" --libdir lib -Dc_args="$(DPDK_CFLAGS)" -Dc_link_args="$(DPDK_LDFLAGS)" $(DPDK_OPTS) -Denable_drivers="$(DPDK_ENABLED_DRIVERS)" -Denable_libs="$(DPDK_ENABLED_LIBS)" -Ddisable_apps="*" $(DPDKBUILD_FLAGS) build-tmp
	$(Q)sed $(SED_INPLACE_FLAG) 's/#define RTE_EAL_PMD_PATH .*/#define RTE_EAL_PMD_PATH ""/g' $(SPDK_ROOT_DIR)/dpdk/build-tmp/rte_build_config.h

clean:
	$(Q)rm -rf $(SPDK_ROOT_DIR)/dpdk/build $(SPDK_ROOT_DIR)/dpdk/build-tmp

install:
	$(Q)cd "$(SPDK_ROOT_DIR)/dpdk"; CC="$(SUB_CC)" meson setup --prefix="$(CONFIG_PREFIX)" --reconfigure build-tmp
	$(Q)env -u MAKEFLAGS meson install -C $(SPDK_ROOT_DIR)/dpdk/build-tmp --no-rebuild
	$(Q)cd "$(SPDK_ROOT_DIR)/dpdk"; CC="$(SUB_CC)" meson setup --prefix="$(MESON_PREFIX)" --reconfigure build-tmp


# meson has no uninstall target. We'll just do our best.
uninstall:
	$(Q)rm -rf $(DESTDIR)$(CONFIG_PREFIX)/share/dpdk
	$(Q)rm -f $(DESTDIR)$(CONFIG_PREFIX)/bin/dpdk-*.py
	$(Q)rm -f $(DESTDIR)$(CONFIG_PREFIX)/include/rte_*.h
	$(Q)rm -f $(DESTDIR)$(CONFIG_PREFIX)/include/generic/rte_*.h
	$(Q)rm -f $(DESTDIR)$(CONFIG_PREFIX)/lib/librte_*
	$(Q)rm -rf $(DESTDIR)$(CONFIG_PREFIX)/lib/dpdk
	$(Q)rm -f $(DESTDIR)$(CONFIG_PREFIX)/lib/pkgconfig/libdpdk*
	$(Q)rm -f $(DESTDIR)$(CONFIG_PREFIX)/include/cmdline_parse_portlist.h
	$(Q)rm -f $(DESTDIR)$(CONFIG_PREFIX)/include/cmdline_cirbuf.h
	$(Q)rm -f $(DESTDIR)$(CONFIG_PREFIX)/include/cmdline_socket.h
	$(Q)rm -f $(DESTDIR)$(CONFIG_PREFIX)/include/cmdline_vt100.h
	$(Q)rm -f $(DESTDIR)$(CONFIG_PREFIX)/include/cmdline_rdline.h
	$(Q)rm -f $(DESTDIR)$(CONFIG_PREFIX)/include/cmdline_parse_bool.h
	$(Q)rm -f $(DESTDIR)$(CONFIG_PREFIX)/include/cmdline_parse_string.h
	$(Q)rm -f $(DESTDIR)$(CONFIG_PREFIX)/include/cmdline_parse_etheraddr.h
	$(Q)rm -f $(DESTDIR)$(CONFIG_PREFIX)/include/cmdline_parse_ipaddr.h
	$(Q)rm -f $(DESTDIR)$(CONFIG_PREFIX)/include/cmdline_parse_num.h
	$(Q)rm -f $(DESTDIR)$(CONFIG_PREFIX)/include/cmdline_parse.h
	$(Q)rm -f $(DESTDIR)$(CONFIG_PREFIX)/include/cmdline.h
	$(Q)rm -f $(DESTDIR)$(CONFIG_PREFIX)/include/power_cpufreq.h
	$(Q)rm -f $(DESTDIR)$(CONFIG_PREFIX)/include/power_uncore_ops.h
