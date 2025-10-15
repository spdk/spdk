#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2023 Intel Corporation.
#  Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

all: spdk/version.py

clean:
	$(Q)rm -f spdk/version.py
	$(Q)rm -rf $(SPDK_ROOT_DIR)/python/dist

spdk/version.py: $(SPDK_ROOT_DIR)/VERSION
	$(Q)printf "__version__ = '%d.%d%s%s'\n" \
		$(version_major) $(version_minor) \
		$(shell [ $(version_patch) != 0 ] && echo -n ".$(version_patch)") \
		$(shell echo -n \"$(version_suffix)\" | sed 's/-pre/rc0/g' | tr -d -) > spdk/version.py

build: spdk/version.py
	$(Q)python -m build $(SPDK_ROOT_DIR)/python/

check: build
	$(Q)python -m twine check $(SPDK_ROOT_DIR)/python/dist/*

install:
	$(Q)uv pip install --prefix=$(CONFIG_PREFIX) $(SPDK_ROOT_DIR)/python
	rm -rf $(SPDK_ROOT_DIR)/python/spdk.egg-info

uninstall:
	$(Q)uv pip uninstall --prefix=$(CONFIG_PREFIX) spdk

.PHONY: all clean install uninstall build check
