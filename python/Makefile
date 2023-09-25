#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2023 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

setup_cmd = python3 setup.py install
ifneq ($(DESTDIR),)
setup_cmd += --root $(DESTDIR)
endif

ifneq ($(CONFIG_PYDIR),)
setup_cmd += --install-purelib $(CONFIG_PYDIR)
purelibdir := $(CONFIG_PYDIR)
else
purelibdir := $(shell python3 -c "import sysconfig; print(sysconfig.get_paths()['purelib'])")
endif

all:

clean:

install:
	$(Q)$(setup_cmd)
	$(Q)rm -r spdk.egg-info

uninstall:
	$(Q)rm -r $(DESTDIR)/$(purelibdir)/spdk*

.PHONY: all clean install uninstall
