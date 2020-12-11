#
#  BSD LICENSE
#
#  Copyright (c) Intel Corporation.
#  All rights reserved.
#
#  Redistribution and use in source and binary forms, with or without
#  modification, are permitted provided that the following conditions
#  are met:
#
#    * Redistributions of source code must retain the above copyright
#      notice, this list of conditions and the following disclaimer.
#    * Redistributions in binary form must reproduce the above copyright
#      notice, this list of conditions and the following disclaimer in
#      the documentation and/or other materials provided with the
#      distribution.
#    * Neither the name of Intel Corporation nor the names of its
#      contributors may be used to endorse or promote products derived
#      from this software without specific prior written permission.
#
#  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
#  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
#  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
#  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
#  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
#  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
#  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
#  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
#  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
#  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
#  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#

include $(SPDK_ROOT_DIR)/mk/spdk.lib_deps.mk

# _uniq returns the unique elements from the list specified. It does
# not change the order of the elements. If the same element occurs
# multiple times in the list, the last instance is kept and the others
# removed.
# Example: _uniq(conf log json log util util log util) = conf json log util
define _uniq
$(if $1,$(call _uniq,$(filter-out $(lastword $1),$1)) $(lastword $1))
endef

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
