#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) Samsung Electronics Co., Ltd.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

.PHONY: all clean install uninstall

all:
	(cd $(SPDK_ROOT_DIR)/xnvme && \
		export PKG_CONFIG_PATH=$$PKG_CONFIG_PATH:/usr/lib/pkgconfig:/usr/lib64/pkgconfig && \
		meson setup builddir \
		-Dwith-libaio=enabled \
		-Dwith-liburing=enabled \
		-Dwith-libvfn=disabled \
		-Dwith-spdk=disabled \
		-Dexamples=false \
		-Dtests=false \
		-Dtools=false && \
		meson compile -C builddir && \
		cd -)

install: all

uninstall:
	@:

clean:
	(cd $(SPDK_ROOT_DIR)/xnvme && rm -fr builddir || true && cd -)
