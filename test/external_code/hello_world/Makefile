#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2020 Intel Corporation.
#  Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES
#  All rights reserved.
#

PKG_CONFIG_PATH = $(SPDK_LIB_DIR)/pkgconfig

DPDK_LIB := $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --libs spdk_env_dpdk)
SPDK_EVENT_LIB := $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --libs spdk_event spdk_event_bdev)
SPDK_DPDK_LIB := $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --libs spdk_event spdk_event_bdev spdk_env_dpdk)
SYS_LIB := $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --libs --static spdk_syslibs)

# Shows how to compile both an external bdev and an external application against the SPDK combined shared object and dpdk shared objects.
bdev_shared_combo:
	$(CC) $(COMMON_CFLAGS) -L../passthru -Wl,-rpath=$(SPDK_LIB_DIR),--no-as-needed -o hello_bdev ./hello_bdev.c -lpassthru_external \
	-lspdk $(DPDK_LIB) -Wl,--no-whole-archive $(SYS_LIB)

# Shows how to compile both an external bdev and an external application against the SPDK individual shared objects and dpdk shared objects.
bdev_shared_iso:
	$(CC) $(COMMON_CFLAGS) -L../passthru -Wl,--no-as-needed -o hello_bdev ./hello_bdev.c \
	-lpassthru_external $(SPDK_EVENT_LIB) \
	$(DPDK_LIB) $(SYS_LIB)

# Shows how to compile an external application against the SPDK combined shared object and dpdk shared objects.
alone_shared_combo:
	$(CC) $(COMMON_CFLAGS) -Wl,-rpath=$(SPDK_LIB_DIR),--no-as-needed -o hello_bdev ./hello_bdev.c -lspdk $(DPDK_LIB) $(SYS_LIB)

# Shows how to compile an external application against the SPDK individual shared objects and dpdk shared objects.
alone_shared_iso:
	$(CC) $(COMMON_CFLAGS) -Wl,-rpath=$(SPDK_LIB_DIR),--no-as-needed -o hello_bdev ./hello_bdev.c \
	$(SPDK_EVENT_LIB) $(DPDK_LIB) $(SYS_LIB)

# Shows how to compile an external application against the SPDK archives.
alone_static:
	$(CC) $(COMMON_CFLAGS) -o hello_bdev ./hello_bdev.c -pthread -Wl,--whole-archive,-Bstatic \
	$(SPDK_DPDK_LIB) -Wl,--no-whole-archive,-Bdynamic $(SYS_LIB)

# Shows how to compile and external bdev and application against the SPDK archives.
bdev_static:
	$(CC) $(COMMON_CFLAGS) -L../passthru -o hello_bdev ./hello_bdev.c -pthread -Wl,--whole-archive,-Bstatic -lpassthru_external $(SPDK_DPDK_LIB) \
	-Wl,--no-whole-archive,-Bdynamic $(SYS_LIB)
