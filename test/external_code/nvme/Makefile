#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2021 Intel Corporation.
#  All rights reserved.
#

PKG_CONFIG_PATH = $(SPDK_LIB_DIR)/pkgconfig

DEPLIBS := $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --libs spdk_env_dpdk spdk_util)
SYS_LIB := $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --libs --static spdk_syslibs)

shared:
	$(CC) $(COMMON_CFLAGS) -c -fPIC nvme.c -o nvme.o
	$(CC) $(COMMON_CFLAGS) -shared nvme.o -o libnvme_external.so
	$(CC) $(COMMON_CFLAGS) identify.c -o identify -L. -lnvme_external \
		-Wl,-rpath=$(SPDK_LIB_DIR) $(DEPLIBS)

static:
	$(CC) $(COMMON_CFLAGS) nvme.c identify.c -o identify -Wl,--whole-archive \
		$(DEPLIBS) -Wl,--no-whole-archive $(SYS_LIB)
