#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2020 Intel Corporation.
#  All rights reserved.
#

ALL_DEPDIRS+= hello_world passthru nvme

ifneq ($(SPDK_HEADER_DIR),)
COMMON_CFLAGS+=-I$(SPDK_HEADER_DIR)
endif

ifneq ($(SPDK_LIB_DIR),)
COMMON_CFLAGS+=-L$(SPDK_LIB_DIR)
endif

ifneq ($(DPDK_LIB_DIR),)
COMMON_CFLAGS+=-L$(DPDK_LIB_DIR)
endif

ifneq ($(VFIO_LIB_DIR),)
COMMON_CFLAGS+=-L$(VFIO_LIB_DIR)
endif
export
.PHONY: all

all: hello_world_bdev_shared_combo nvme_shared

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
