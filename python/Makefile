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
# Consider different scenarios where purelib can be located:
#
# - /usr/lib/python*/site-packages
# - /usr/local/lib/python*/site-packages
# - /foo/bar/lib/python*/site-packages
#
# The latter here can be a result of using interpreter from within a venv
# located at custom location - see how we handle PEP668 under ubuntu in
# pkgdep/ubuntu.sh for instance.
#
# So the common part here is:
#   *lib/python*/site-packages
#
# So we cut the path to get the prefix (whatever it may be) and then we nuke
# it from the main path. So the corner case would look like so:
#
# pylibdir := /foo/bar/lib/python*/site-packages
# pylibprefix := /foo/bar
# pylibdir := $(CONFIG_PREFIX)/lib/python*/site-packages
#
# FIXME: I couldn't get patsubst to do the heavy lifting here, hence fallback
# to shell.
pylibprefix := $(shell a=$(pylibdir); echo $${a%/lib*})
pylibdir := $(CONFIG_PREFIX)$(pylibdir:$(pylibprefix)%=%)
endif

all: spdk/version.py

clean:
	$(Q)rm -f spdk/version.py

spdk/version.py: $(SPDK_ROOT_DIR)/VERSION
	$(Q)printf "__version__ = '%d.%d%s%s'\n" \
		$(version_major) $(version_minor) \
		$(shell [ $(version_patch) != 0 ] && echo -n ".$(version_patch)") \
		$(shell echo -n \"$(version_suffix)\" | sed 's/-pre/rc0/g' | tr -d -) > spdk/version.py

install:
	$(Q)$(setup_cmd) $(SPDK_ROOT_DIR)/python
	rm -rf $(SPDK_ROOT_DIR)/python/spdk.egg-info

uninstall:
	$(Q)rm -rf $(DESTDIR)/$(pylibdir)/spdk*
	$(Q)rm -f $(DESTDIR)/$(bindir)/spdk-rpc
	$(Q)rm -f $(DESTDIR)/$(bindir)/spdk-sma
	$(Q)rm -f $(DESTDIR)/$(bindir)/spdk-cli
	$(Q)rm -f $(DESTDIR)/$(bindir)/spdk-mcp

.PHONY: all clean install uninstall
