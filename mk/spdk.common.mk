#
#  BSD LICENSE
#
#  Copyright (c) Intel Corporation.
#  Copyright (c) 2017, IBM Corporation.
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

ifneq ($(MAKECMDGOALS),clean)
ifeq ($(wildcard $(SPDK_ROOT_DIR)/mk/config.mk),)
$(error mk/config.mk: file not found. Please run configure before 'make $(filter-out clean,$(MAKECMDGOALS))')
endif
endif

include $(SPDK_ROOT_DIR)/mk/config.mk

-include $(SPDK_ROOT_DIR)/mk/cc.mk

ifneq ($(V),1)
Q ?= @
endif
S ?= $(notdir $(CURDIR))

DESTDIR?=

ifneq ($(prefix),)
CONFIG_PREFIX=$(prefix)
endif

bindir?=$(CONFIG_PREFIX)/bin
libdir?=$(CONFIG_PREFIX)/lib
includedir?=$(CONFIG_PREFIX)/include

ifeq ($(MAKECMDGOALS),)
MAKECMDGOALS=$(.DEFAULT_GOAL)
endif

TARGET_TRIPLET := $(shell $(CC) -dumpmachine)
TARGET_TRIPLET_WORDS := $(subst -, ,$(TARGET_TRIPLET))

ifneq ($(filter linux%,$(TARGET_TRIPLET_WORDS)),)
OS = Linux
endif
ifneq ($(filter freebsd%,$(TARGET_TRIPLET_WORDS)),)
OS = FreeBSD
endif

TARGET_MACHINE := $(firstword $(TARGET_TRIPLET_WORDS))

COMMON_CFLAGS = -g $(C_OPT) -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wmissing-declarations -fno-strict-aliasing -I$(SPDK_ROOT_DIR)/include

ifneq ($(filter powerpc%,$(TARGET_MACHINE)),)
COMMON_CFLAGS += -mcpu=native
endif
ifeq ($(TARGET_MACHINE),x86_64)
COMMON_CFLAGS += -march=native
endif

ifeq ($(CONFIG_WERROR), y)
COMMON_CFLAGS += -Werror
endif

ifeq ($(CONFIG_LTO),y)
COMMON_CFLAGS += -flto
LDFLAGS += -flto
endif

COMMON_CFLAGS += -Wformat -Wformat-security

COMMON_CFLAGS += -D_GNU_SOURCE

# Always build PIC code so that objects can be used in shared libs and position-independent executables
COMMON_CFLAGS += -fPIC

# Enable stack buffer overflow checking
COMMON_CFLAGS += -fstack-protector

# Prevent accidental multiple definitions of global variables
COMMON_CFLAGS += -fno-common

# Enable full RELRO - no lazy relocation (resolve everything at load time).
# This allows the GOT to be made read-only early in the loading process.
LDFLAGS += -Wl,-z,relro,-z,now

# Make the stack non-executable.
# This is the default in most environments, but it doesn't hurt to set it explicitly.
LDFLAGS += -Wl,-z,noexecstack

ifeq ($(OS),FreeBSD)
SYS_LIBS += -L/usr/local/lib
COMMON_CFLAGS += -I/usr/local/include
endif

# Attach only if PMDK lib specified with configure
ifneq ($(CONFIG_PMDK_DIR),)
LIBS += -L$(CONFIG_PMDK_DIR)/src/nondebug
COMMON_CFLAGS += -I$(CONFIG_PMDK_DIR)/src/include
endif

ifneq ($(CONFIG_VPP_DIR),)
LIBS += -L$(CONFIG_VPP_DIR)/lib64
COMMON_CFLAGS += -I$(CONFIG_VPP_DIR)/include
endif

ifeq ($(CONFIG_RDMA),y)
SYS_LIBS += -libverbs -lrdmacm
endif

#Attach only if FreeBSD and RDMA is specified with configure
ifeq ($(OS),FreeBSD)
ifeq ($(CONFIG_RDMA),y)
# Mellanox - MLX4 HBA Userspace Library
ifneq ("$(wildcard /usr/lib/libmlx4.*)","")
SYS_LIBS += -lmlx4
endif
# Mellanox - MLX5 HBA Userspace Library
ifneq ("$(wildcard /usr/lib/libmlx5.*)","")
SYS_LIBS += -lmlx5
endif
# Chelsio HBA Userspace Library
ifneq ("$(wildcard /usr/lib/libcxgb4.*)","")
SYS_LIBS += -lcxgb4
endif
endif
endif

ifeq ($(CONFIG_DEBUG), y)
COMMON_CFLAGS += -DDEBUG -O0 -fno-omit-frame-pointer
else
COMMON_CFLAGS += -DNDEBUG -O2
# Enable _FORTIFY_SOURCE checks - these only work when optimizations are enabled.
COMMON_CFLAGS += -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2
endif

ifeq ($(CONFIG_COVERAGE), y)
COMMON_CFLAGS += -fprofile-arcs -ftest-coverage
LDFLAGS += -fprofile-arcs -ftest-coverage
ifeq ($(OS),FreeBSD)
LDFLAGS += --coverage
endif
endif

include $(CONFIG_ENV)/env.mk

ifeq ($(CONFIG_ASAN),y)
COMMON_CFLAGS += -fsanitize=address
LDFLAGS += -fsanitize=address
endif

ifeq ($(CONFIG_UBSAN),y)
COMMON_CFLAGS += -fsanitize=undefined
LDFLAGS += -fsanitize=undefined
endif

ifeq ($(CONFIG_TSAN),y)
COMMON_CFLAGS += -fsanitize=thread
LDFLAGS += -fsanitize=thread
endif

COMMON_CFLAGS += -pthread
LDFLAGS += -pthread

CFLAGS   += $(COMMON_CFLAGS) -Wno-pointer-sign -Wstrict-prototypes -Wold-style-definition -std=gnu99
CXXFLAGS += $(COMMON_CFLAGS) -std=c++0x

SYS_LIBS += -lrt
SYS_LIBS += -luuid
SYS_LIBS += -lcrypto
ifeq ($(CONFIG_LOG_BACKTRACE),y)
SYS_LIBS += -lunwind
endif

MAKEFLAGS += --no-print-directory

C_SRCS += $(C_SRCS-y)
CXX_SRCS += $(CXX_SRCS-y)

OBJS = $(C_SRCS:.c=.o) $(CXX_SRCS:.cpp=.o)

DEPFLAGS = -MMD -MP -MF $*.d.tmp

# Compile first input $< (.c) into $@ (.o)
COMPILE_C=\
	$(Q)echo "  CC $S/$@"; \
	$(CC) -o $@ $(DEPFLAGS) $(CFLAGS) -c $< && \
	mv -f $*.d.tmp $*.d && touch -c $@

COMPILE_CXX=\
	$(Q)echo "  CXX $S/$@"; \
	$(CXX) -o $@ $(DEPFLAGS) $(CXXFLAGS) -c $< && \
	mv -f $*.d.tmp $*.d && touch -c $@

# Link $(OBJS) and $(LIBS) into $@ (app)
LINK_C=\
	$(Q)echo "  LINK $S/$@"; \
	$(CC) -o $@ $(CPPFLAGS) $(LDFLAGS) $(OBJS) $(LIBS) $(SYS_LIBS)

LINK_CXX=\
	$(Q)echo "  LINK $S/$@"; \
	$(CXX) -o $@ $(CPPFLAGS) $(LDFLAGS) $(OBJS) $(LIBS) $(SYS_LIBS)

#
# Variables to use for versioning shared libs
#
SO_VER := 1
SO_MINOR := 0
SO_SUFFIX_ALL := $(SO_VER).$(SO_MINOR)

# Provide function to ease build of a shared lib
define spdk_build_realname_shared_lib
	$(CC) -o $@ -shared $(CPPFLAGS) $(LDFLAGS) \
	    -Wl,--soname,$(patsubst %.so.$(SO_SUFFIX_ALL),%.so.$(SO_VER),$(notdir $@)) \
	    -Wl,--whole-archive $(1) -Wl,--no-whole-archive \
	    -Wl,--version-script=$(2) \
	    $(3)
endef

BUILD_LINKERNAME_LIB=\
	ln -sf $(notdir $<) $@

# Archive $(OBJS) into $@ (.a)
LIB_C=\
	$(Q)echo "  LIB $(notdir $@)"; \
	rm -f $@; \
	$(CCAR) crDs $@ $(OBJS)

# Clean up generated files listed as arguments plus a default list
CLEAN_C=\
	$(Q)rm -f *.a *.o *.d *.d.tmp *.gcno *.gcda

# Install a library
INSTALL_LIB=\
	$(Q)echo "  INSTALL $(DESTDIR)$(libdir)/$(notdir $(LIB))"; \
	install -d -m 755 "$(DESTDIR)$(libdir)"; \
	install -m 644 "$(LIB)" "$(DESTDIR)$(libdir)/"

ifeq ($(OS),FreeBSD)
INSTALL_REL_SYMLINK := install -l rs
else
INSTALL_REL_SYMLINK := ln -sf -r
endif

define spdk_install_lib_symlink
	$(INSTALL_REL_SYMLINK) $(DESTDIR)$(libdir)/$(1) $(DESTDIR)$(libdir)/$(2)
endef

INSTALL_SHARED_LIB=\
	$(Q)echo "  INSTALL $(DESTDIR)$(libdir)/$(notdir $(SHARED_LINKED_LIB))"; \
	install -d -m 755 "$(DESTDIR)$(libdir)"; \
	install -m 755 "$(SHARED_REALNAME_LIB)" "$(DESTDIR)$(libdir)/"; \
	$(call spdk_install_lib_symlink,$(notdir $(SHARED_REALNAME_LIB)),$(notdir $(SHARED_LINKED_LIB)));

# Install an app binary
INSTALL_APP=\
	$(Q)echo "  INSTALL $(DESTDIR)$(bindir)/$(APP)"; \
	install -d -m 755 "$(DESTDIR)$(bindir)"; \
	install -m 755 "$(APP)" "$(DESTDIR)$(bindir)/"

# Install a header
INSTALL_HEADER=\
	$(Q)echo "  INSTALL $@"; \
	install -d -m 755 "$(DESTDIR)$(includedir)/$(dir $(patsubst $(DESTDIR)$(includedir)/%,%,$@))"; \
	install -m 644 "$(patsubst $(DESTDIR)$(includedir)/%,%,$@)" "$(DESTDIR)$(includedir)/$(dir $(patsubst $(DESTDIR)$(includedir)/%,%,$@))/"

%.o: %.c %.d $(MAKEFILE_LIST)
	$(COMPILE_C)

%.o: %.cpp %.d $(MAKEFILE_LIST)
	$(COMPILE_CXX)

%.d: ;

define spdk_lib_list_to_static_libs
$(1:%=$(SPDK_ROOT_DIR)/build/lib/libspdk_%.a)
endef

define spdk_lib_list_to_shared_libs
$(1:%=$(SPDK_ROOT_DIR)/build/lib/libspdk_%.so)
endef
