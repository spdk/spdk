#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/..)

all: doc
	@:

.PHONY: all doc jsonrpc clean

doc: output

deprecation.md: ../deprecation.md
	$(Q)sed -e 's/^# Deprecation/# Deprecation {#deprecation}/' \
	    < $< > $@

changelog.md: ../CHANGELOG.md
	$(Q)sed -e 's/^# Changelog/# Changelog {#changelog}/' \
	    -e 's/^##/#/' \
	    -e 's/^# \(\(v..\...\):.*\)/# \1 {#changelog-\2}/' \
	    -e '/# v..\...:/s/\./-/2' \
	    < $< > $@

jsonrpc: $(SPDK_ROOT_DIR)/doc/jsonrpc.md

$(SPDK_ROOT_DIR)/doc/jsonrpc.md: $(SPDK_ROOT_DIR)/scripts/genrpc.py $(SPDK_ROOT_DIR)/schema/schema.json $(SPDK_ROOT_DIR)/doc/jsonrpc.md.jinja2
	$(Q)$(SPDK_ROOT_DIR)/scripts/genrpc.py --schema $(SPDK_ROOT_DIR)/schema/schema.json --doc > $@

output: jsonrpc Doxyfile changelog.md deprecation.md $(wildcard *.md) $(wildcard ../include/spdk/*.h)
	$(Q)rm -rf $@
	$(Q)doxygen Doxyfile

clean:
	$(Q)rm -rf output changelog.md deprecation.md jsonrpc.md
