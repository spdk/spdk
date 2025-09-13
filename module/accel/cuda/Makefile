#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  Copyright (c) 2025 StarWind Software, Inc.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 6
SO_MINOR := 0

LIBNAME = accel_cuda
C_SRCS = accel_cuda.c accel_cuda_rpc.c cuda_utils.c
CU_SRCS = accel_cuda_kern.cu

# pass options to nvcc
CUDA_ARCH = 70
#CU_CFLAGS = -keep

SPDK_MAP_FILE = $(SPDK_ROOT_DIR)/mk/spdk_blank.map

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
