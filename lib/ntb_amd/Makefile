#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 Advanced Micro Devices, Inc.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 1
SO_MINOR := 0

C_SRCS = ntb_amd.c

# Keep the directory name (ntb_amd), the LIBNAME, and the DEPDIRS-* key in
# mk/spdk.lib_deps.mk in lockstep. mk/spdk.subdirs.mk looks up DEPDIRS by
# directory name; mismatches require workarounds in lib/Makefile, so just
# don't introduce them.
#
# Naming convention follows the NTB-framework precedent (Linux's
# drivers/ntb/hw/amd/, DPDK's dpdk/drivers/raw/ntb/ntb_hw_*.c) and the
# SPDK <category>_<implementation> pattern (bdev_nvme, accel_ioat, etc.):
# this is "an NTB driver from AMD", so subsystem-first, vendor-last.
LIBNAME = ntb_amd

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_ntb_amd.map)

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
