SPDK_ROOT_DIR := $(abspath $(CURDIR)/..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

all: doc
	@:

.PHONY: all doc clean

doc: output

changelog.md: ../CHANGELOG.md
	$(Q)sed -e 's/^# Changelog/# Changelog {#changelog}/' \
	    -e 's/^##/#/' \
	    -e 's/^# \(\(v..\...\):.*\)/# \1 {#changelog-\2}/' \
	    -e '/# v..\...:/s/\./-/2' \
	    < $< > $@

output: Doxyfile changelog.md $(wildcard *.md) $(wildcard ../include/spdk/*.h)
	$(Q)rm -rf $@
	$(Q)doxygen Doxyfile

clean:
	$(Q)rm -rf output changelog.md
