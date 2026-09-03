#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2017 Intel Corporation
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

APP = iofilespdk

C_SRCS := iofilespdk.c

CUDA_PATH ?= /usr/local/cuda
NVCC := $(CUDA_PATH)/bin/nvcc

.DEFAULT_GOAL := all

all: gpu_fill.o

gpu_fill.o: gpu_fill.cu gpu_fill.h
	$(NVCC) -c gpu_fill.cu -o gpu_fill.o

CLEAN_FILES += gpu_fill.o

LIBS += gpu_fill.o -L$(CUDA_PATH)/lib64 -lcudart -lstdc++

SPDK_LIB_LIST = $(ALL_MODULES_LIST) event event_bdev

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk


