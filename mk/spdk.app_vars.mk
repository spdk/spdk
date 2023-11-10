#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation.
#  All rights reserved.
#

include $(SPDK_ROOT_DIR)/mk/spdk.lib_deps.mk

define _deplibs
$(if $1,$(foreach d,$1,$(d) $(call _deplibs,$(DEPDIRS-$(d)))))
endef

define deplibs
$(call _uniq,$(call _deplibs,$1))
endef

SPDK_DEPLIB_LIST += $(call deplibs,$(SPDK_LIB_LIST))

SPDK_LIB_FILES = $(call spdk_lib_list_to_static_libs,$(SPDK_DEPLIB_LIST))
SPDK_LIB_LINKER_ARGS = \
	-L$(SPDK_ROOT_DIR)/build/lib \
	-Wl,--whole-archive \
	-Wl,--no-as-needed \
	$(SPDK_DEPLIB_LIST:%=-lspdk_%) \
	-Wl,--no-whole-archive

# This is primarily used for unit tests to ensure they link when shared library
# build is enabled.  Shared libraries can't get their mock implementation from
# the unit test file.  Note that even for unittests, we must include the mock
# library with whole-archive, to keep its functions from getting stripped out
# when LTO is enabled.
SPDK_STATIC_LIB_LINKER_ARGS = \
	$(SPDK_LIB_LIST:%=$(SPDK_ROOT_DIR)/build/lib/libspdk_%.a) \
	-Wl,--whole-archive \
	$(SPDK_ROOT_DIR)/build/lib/libspdk_ut_mock.a \
	-Wl,--no-whole-archive
