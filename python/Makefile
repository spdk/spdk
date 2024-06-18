#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2023 Intel Corporation.
#  Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

setup_cmd = pip install --prefix=$(CONFIG_PREFIX)
ifneq ($(DESTDIR),)
setup_cmd += --root $(DESTDIR)
endif

pylibdir := $(abspath $(shell python3 -c "import sysconfig; print(sysconfig.get_paths()['purelib'])"))
ifneq ($(CONFIG_PREFIX),)
pylibdir := $(pylibdir:/usr%=%)
pylibdir := $(pylibdir:/local%=%)
pylibdir := $(CONFIG_PREFIX)$(pylibdir)
endif

all:

clean:

install: rpc spdkcli
	$(Q)$(setup_cmd) $(SPDK_ROOT_DIR)/python
	rm -rf $(SPDK_ROOT_DIR)/python/spdk.egg-info

$(SPDK_ROOT_DIR)/build/bin/spdk_rpc:
	cp $(SPDK_ROOT_DIR)/scripts/rpc.py $(SPDK_ROOT_DIR)/build/bin/spdk_rpc
	chmod +x $(SPDK_ROOT_DIR)/build/bin/spdk_rpc

$(SPDK_ROOT_DIR)/build/bin/spdk_cli:
	cp $(SPDK_ROOT_DIR)/scripts/spdkcli.py $(SPDK_ROOT_DIR)/build/bin/spdk_cli
	chmod +x $(SPDK_ROOT_DIR)/build/bin/spdk_cli

rpc: $(SPDK_ROOT_DIR)/build/bin/spdk_rpc
	$(INSTALL_APP)

spdkcli: $(SPDK_ROOT_DIR)/build/bin/spdk_cli
	$(INSTALL_APP)

uninstall:
	$(Q)rm -rf $(DESTDIR)/$(pylibdir)/spdk*
	$(Q)rm -f $(DESTDIR)/$(bindir)/spdk_rpc
	$(Q)rm -f $(DESTDIR)/$(bindir)/spdk_cli

.PHONY: all clean install uninstall
