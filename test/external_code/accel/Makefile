#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2023 Intel Corporation.
#  All rights reserved.
#

PKG_CONFIG_PATH = $(SPDK_LIB_DIR)/pkgconfig

SYS_LIB := $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --libs --static spdk_syslibs)
SPDK_LIB := $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config \
		--libs spdk_event spdk_event_accel spdk_accel spdk_env_dpdk)

shared:
	$(CC) $(COMMON_CFLAGS) -c -fPIC accel_module.c -o accel_module.o
	$(CC) $(COMMON_CFLAGS) -shared accel_module.o -o libaccel_external.so
	$(CC) $(COMMON_CFLAGS) -o accel_module accel_run.c -L. -laccel_external \
		-lspdk $(SPDK_LIB)

static:
	$(CC) $(COMMON_CFLAGS) -o accel_module accel_module.c accel_run.c -pthread \
		-Wl,--whole-archive,-Bstatic $(SPDK_LIB) \
		-Wl,--no-whole-archive,-Bdynamic $(SYS_LIB)
