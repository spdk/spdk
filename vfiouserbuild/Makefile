#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2020 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/..)
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

all: build

build:
	$(Q)test -d $(VFIO_USER_BUILD_DIR) || \
	     CC=$(SUB_CC) meson setup $(VFIO_USER_CFG_OPTS) $(VFIO_USER_BUILD_DIR) $(VFIO_USER_SRC_DIR)
	$(Q)meson compile $(VFIO_USER_MESON_COMPILE_OPTS) -C $(VFIO_USER_BUILD_DIR) vfio-user
	$(Q)DESTDIR=$(VFIO_USER_INSTALL_DIR) meson install $(VFIO_USER_MESON_INSTALL_OPTS) -C $(VFIO_USER_BUILD_DIR)

install: all

uninstall:
	@:

clean:
	$(Q)rm -rf $(VFIO_USER_BUILD_DIR)
