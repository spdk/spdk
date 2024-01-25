#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2023 Intel Corporation.
#  All rights reserved.
#

PKG_CONFIG_PATH = $(SPDK_LIB_DIR)/pkgconfig

SYS_LIB := $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --libs --static spdk_syslibs)
SPDK_LIB := $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config \
		--libs spdk_event spdk_event_accel spdk_accel spdk_env_dpdk)

shared_module:
	$(CC) $(COMMON_CFLAGS) -c -fPIC module.c -o module.o
	$(CC) $(COMMON_CFLAGS) -shared module.o -o libaccel_external_module.so
	$(CC) $(COMMON_CFLAGS) -o module app_module.c -g -L. -laccel_external_module \
		-lspdk -lisal -lisal_crypto $(SPDK_LIB)

shared_driver:
	$(CC) $(COMMON_CFLAGS) -c -fPIC module.c -o module.o
	$(CC) $(COMMON_CFLAGS) -c -fPIC driver.c -o driver.o
	$(CC) $(COMMON_CFLAGS) -shared module.o driver.o -o libaccel_external_driver.so
	$(CC) $(COMMON_CFLAGS) -o driver app_driver.c -g -L. -laccel_external_driver \
		-lspdk -lisal -lisal_crypto $(SPDK_LIB)

static_module:
	$(CC) $(COMMON_CFLAGS) -o module module.c app_module.c -pthread \
		-Wl,--whole-archive,-Bstatic $(SPDK_LIB) \
		-Wl,--no-whole-archive,-Bdynamic $(SYS_LIB)

static_driver:
	$(CC) $(COMMON_CFLAGS) -o driver module.c driver.c app_driver.c \
		-Wl,--whole-archive,-Bstatic $(SPDK_LIB) \
		-Wl,--no-whole-archive,-Bdynamic $(SYS_LIB) \
		-pthread
