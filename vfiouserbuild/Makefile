#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2020 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/..)
# Save configured flags before the common makefile adds SPDK-only arguments.
-include $(SPDK_ROOT_DIR)/mk/cc.flags.mk
VFIO_USER_CFLAGS := $(CFLAGS)
VFIO_USER_LDFLAGS := $(LDFLAGS)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

.PHONY: all build clean install uninstall

# Do not run scan-build on the libvfio-user sources.
SUB_CC = $(patsubst %ccc-analyzer,$(DEFAULT_CC),$(CC))

VFIO_USER_CFG_OPTS = --buildtype $(VFIO_USER_BUILD_TYPE) -Ddefault_library=$(VFIO_USER_BUILD_SHARED) -Dlibdir=$(VFIO_USER_LIB_PREFIX)

ifeq ($(V),1)
VFIO_USER_MESON_COMPILE_OPTS = --verbose
VFIO_USER_MESON_INSTALL_OPTS =
else
VFIO_USER_MESON_COMPILE_OPTS =
VFIO_USER_MESON_INSTALL_OPTS = --quiet
endif

VFIO_USER_MESON_INSTALL_OPTS += --no-rebuild --only-changed

ifneq ($(strip $(VFIO_USER_CFLAGS)),)
VFIO_USER_CFG_OPTS += -Dc_args="$(VFIO_USER_CFLAGS)"
endif
ifneq ($(strip $(VFIO_USER_LDFLAGS)),)
VFIO_USER_CFG_OPTS += -Dc_link_args="$(VFIO_USER_LDFLAGS)"
endif

# Meson caches the compiler, flags and dependency discovery from the first
# setup. Start over whenever the SPDK configuration that selected them changes.
VFIO_USER_CONFIG_FILES = $(SPDK_ROOT_DIR)/mk/cc.mk $(SPDK_ROOT_DIR)/mk/config.mk \
			 $(wildcard $(SPDK_ROOT_DIR)/mk/cc.flags.mk) $(CURDIR)/Makefile

all: build

$(VFIO_USER_BUILD_DIR)/build.ninja: $(VFIO_USER_CONFIG_FILES)
	$(Q)rm -rf $(VFIO_USER_INSTALL_DIR)
	$(Q)env -u CFLAGS -u CPPFLAGS -u LDFLAGS CC=$(SUB_CC) \
		meson setup $(VFIO_USER_CFG_OPTS) $(VFIO_USER_BUILD_DIR) $(VFIO_USER_SRC_DIR)

build: $(VFIO_USER_BUILD_DIR)/build.ninja
	$(Q)meson compile $(VFIO_USER_MESON_COMPILE_OPTS) -C $(VFIO_USER_BUILD_DIR) vfio-user
	$(Q)DESTDIR=$(VFIO_USER_INSTALL_DIR) meson install $(VFIO_USER_MESON_INSTALL_OPTS) -C $(VFIO_USER_BUILD_DIR)

install: all

uninstall:
	@:

# The Meson build directory is inside the install directory.
clean:
	$(Q)rm -rf $(VFIO_USER_INSTALL_DIR)
