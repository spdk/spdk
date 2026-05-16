#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2017 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

HEADERS := $(shell find $(SPDK_ROOT_DIR)/include/spdk/ -name '*.h')
INSTALLED_HEADERS := $(patsubst $(SPDK_ROOT_DIR)/include%,$(DESTDIR)$(includedir)%,$(HEADERS))

$(INSTALLED_HEADERS):
ifeq ($(MAKECMDGOALS),install)
	$(INSTALL_HEADER)
else
	$(UNINSTALL_HEADER)
endif

.PHONY: $(INSTALLED_HEADERS) jsonrpc

all: jsonrpc
	$(Q)cp -r $(SPDK_ROOT_DIR)/include/spdk/* $(SPDK_ROOT_DIR)/build/include/spdk/

clean:
	$(Q)rm -f $(SPDK_ROOT_DIR)/include/spdk_internal/rpc_autogen.h
	$(Q)rm -f $(SPDK_ROOT_DIR)/include/spdk_internal/rpc_autogen.h.tmp

jsonrpc: $(SPDK_ROOT_DIR)/include/spdk_internal/rpc_autogen.h

$(SPDK_ROOT_DIR)/include/spdk_internal/rpc_autogen.h: $(SPDK_ROOT_DIR)/scripts/genrpc.py $(SPDK_ROOT_DIR)/schema/schema.json $(SPDK_ROOT_DIR)/include/spdk_internal/rpc_autogen.h.jinja2
	$(Q)$(SPDK_ROOT_DIR)/scripts/genrpc.py --schema $(SPDK_ROOT_DIR)/schema/schema.json --rpcs > $@.tmp && \
		{ cmp -s $@.tmp $@ && rm -f $@.tmp || mv $@.tmp $@; } || \
		{ rm -f $@.tmp; echo "ERROR: scripts/genrpc.py failed to generate $@. Run scripts/pkgdep.sh to install dependencies."; exit 1; }

install: $(INSTALLED_HEADERS)

uninstall: $(INSTALLED_HEADERS)

include $(SPDK_ROOT_DIR)/mk/spdk.deps.mk
