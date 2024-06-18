#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2020 Intel Corporation.
#  All rights reserved.
#

rootdir = $(shell dirname $(shell realpath $(word 1,$(MAKEFILE_LIST))))/../..

SPDK_HEADER_DIR	?= $(rootdir)/include
SPDK_LIB_DIR	?= $(rootdir)/build/lib
DPDK_LIB_DIR	?= $(rootdir)/dpdk/build/lib
ISAL_LIB_DIR	?= $(rootdir)/isa-l/.libs
ISAL_CRYPTO_LIB_DIR ?= $(rootdir)/isa-l-crypto/.libs
VFIO_LIB_DIR	?= $(rootdir)/build/libvfio-user/usr/local/lib
ISAL_LIB_DIR	?= $(rootdir)/isa-l/.libs
ISAL_CRYPTO_LIB_DIR ?= $(rootdir)/isa-l-crypto/.libs

ifneq ($(SPDK_HEADER_DIR),)
COMMON_CFLAGS+=-I$(SPDK_HEADER_DIR)
endif

ifneq ($(SPDK_LIB_DIR),)
COMMON_CFLAGS+=-L$(SPDK_LIB_DIR)
endif

ifneq ($(DPDK_LIB_DIR),)
COMMON_CFLAGS+=-L$(DPDK_LIB_DIR)
endif

ifneq ($(ISAL_LIB_DIR),)
COMMON_CFLAGS+=-L$(ISAL_LIB_DIR)
endif

ifneq ($(ISAL_CRYPTO_LIB_DIR),)
COMMON_CFLAGS+=-L$(ISAL_CRYPTO_LIB_DIR)
endif

ifneq ($(VFIO_LIB_DIR),)
COMMON_CFLAGS+=-L$(VFIO_LIB_DIR)
endif

ifneq ($(ISAL_LIB_DIR),)
COMMON_CFLAGS+=-L$(ISAL_LIB_DIR)
endif

ifneq ($(ISAL_CRYPTO_LIB_DIR),)
COMMON_CFLAGS+=-L$(ISAL_CRYPTO_LIB_DIR)
endif

export
.PHONY: all

all: hello_world_bdev_shared_combo nvme_shared accel_shared

static: hello_world_bdev_static nvme_static

hello_world_bdev_shared_combo: passthru_shared
	$(MAKE) --directory=hello_world bdev_shared_combo

hello_world_bdev_shared_iso: passthru_shared
	$(MAKE) --directory=hello_world bdev_shared_iso

hello_world_no_bdev_shared_combo:
	$(MAKE) --directory=hello_world alone_shared_combo

hello_world_no_bdev_shared_iso:
	$(MAKE) --directory=hello_world alone_shared_iso

hello_world_bdev_static: passthru_static
	$(MAKE) --directory=hello_world bdev_static

hello_world_no_bdev_static:
	$(MAKE) --directory=hello_world alone_static

accel_module_shared:
	$(MAKE) --directory=accel shared_module

accel_driver_shared:
	$(MAKE) --directory=accel shared_driver

accel_module_static:
	$(MAKE) --directory=accel static_module

accel_driver_static:
	$(MAKE) --directory=accel static_driver

passthru_shared:
	$(MAKE) --directory=passthru shared

passthru_static:
	$(MAKE) --directory=passthru static

nvme_shared:
	$(MAKE) --directory=nvme shared

nvme_static:
	$(MAKE) --directory=nvme static

clean:
	rm -f ./hello_world/hello_bdev
	rm -f ./passthru/libpassthru_external.*
	rm -f ./nvme/*.{so,o} ./nvme/identify
	rm -f ./accel/*.{so,o} ./accel/module ./accel/driver
