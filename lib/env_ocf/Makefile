#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2019 Intel Corporation.
#  All rights reserved.
#

# OCF requires users to build with their sources
# If SPDK is configured with OCF source directory,
# we export its files and then compile SPDK LIB with them
# Else if SPDK is configured with OCF precompiled library
# we just use it as SPDK lib by copying it to /build/lib/

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
OCFDIR=$(CONFIG_OCF_DIR)

include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

LIBNAME := ocfenv

CFLAGS +=  $(ENV_CFLAGS) -I$(CURDIR) -I$(CURDIR)/include -w
C_SRCS = $(shell find -name \*.c)

LIB = $(call spdk_lib_list_to_static_libs,$(LIBNAME))


ifeq ($(CONFIG_CUSTOMOCF),y)

.PHONY: all clean install

all:
	$(Q)$(MAKE) $(LIB)

clean:
	$(Q)rm -f $(LIB)

$(LIB):
	cp $(CONFIG_OCF_PATH) $(LIB)

install:

uninstall:
	$(UNINSTALL_LIB)

else

.PHONY: all clean install ocf_inc ocf_src ocf_distclean all exportlib

all: ocf_inc ocf_src
	$(Q)$(MAKE) $(LIB)

ocf_inc:
	$(Q)$(MAKE) -C "$(CONFIG_OCF_PATH)" inc O="$(SPDK_ROOT_DIR)/lib/env_ocf/" ENV= --quiet

ocf_src: ocf_inc
	$(Q)$(MAKE) -C "$(CONFIG_OCF_PATH)" src O="$(SPDK_ROOT_DIR)/lib/env_ocf/" CMD=cp ENV= --quiet

ocf_distclean:
	$(Q)$(MAKE) -C "$(CONFIG_OCF_PATH)" distclean O="$(SPDK_ROOT_DIR)/lib/env_ocf/" ENV= --quiet

clean: ocf_distclean
	$(Q)rm -rf	"$(SPDK_ROOT_DIR)/lib/env_ocf/include" \
				"$(SPDK_ROOT_DIR)/lib/env_ocf/src" \
				$(LIB) $(OBJS);

$(LIB): $(OBJS)
	$(LIB_C)

install:

uninstall:
	$(UNINSTALL_LIB)

endif

exportlib: all
	@ if [ -z $(O) ]; then echo "No output specified"; exit 1; fi
	cp $(LIB) $(O)

help:
	@ echo "all                     Default"
	@ echo "exportlib O=<outpath>   Default build to specified outpath"
