#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2020 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

.PHONY: all clean install uninstall

# Force-disable scan-build: we support it below, but meson has its own special
# target instead of allowing CC=scan-build like everyone else.
SUB_CC = $(patsubst %ccc-analyzer,$(DEFAULT_CC),$(CC))

VFIO_USER_CFG_OPTS = --buildtype $(VFIO_USER_BUILD_TYPE) -Ddefault_library=$(VFIO_USER_BUILD_SHARED) -Dlibdir=$(VFIO_USER_LIB_PREFIX)

all: scan-build

build:
	$(Q)test -d $(VFIO_USER_BUILD_DIR) || \
	     CC=$(SUB_CC) meson setup $(VFIO_USER_BUILD_DIR) $(VFIO_USER_SRC_DIR)
	# Work around pre-v1.9 versions of ninja ignoring new configuration.
	$(Q)[ $$( (ninja --version; echo 1.9) | sort --version-sort | head -n 1) \
	     = 1.9 ] || sleep 0.5
	$(Q)meson configure $(VFIO_USER_BUILD_DIR) $(VFIO_USER_CFG_OPTS)
	$(Q)meson compile $(MESON_COMPILE_OPTS) -C $(VFIO_USER_BUILD_DIR)
	DESTDIR=$(VFIO_USER_INSTALL_DIR) meson install --quiet -C $(VFIO_USER_BUILD_DIR)

scan-build: build
	$(Q)if [ $(CC) = "scan-build" ]; then \
		ninja -C $(VFIO_USER_BUILD_DIR) scan-build; \
	fi

install: all

uninstall:
	@:

clean:
	$(Q)rm -rf $(VFIO_USER_BUILD_DIR)
