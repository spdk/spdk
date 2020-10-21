#
#  BSD LICENSE
#
#  Copyright (c) Intel Corporation.
#  Copyright (c) 2017, IBM Corporation.
#  Copyright (c) 2019, Mellanox Corporation.
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

ifeq ($(wildcard $(SPDK_ROOT_DIR)/mk/config.mk),)
$(error mk/config.mk: file not found. Please run configure before make)
endif

include $(SPDK_ROOT_DIR)/mk/config.mk
-include $(SPDK_ROOT_DIR)/mk/cc.flags.mk
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

TARGET_ARCHITECTURE ?= $(CONFIG_ARCH)
TARGET_MACHINE := $(firstword $(TARGET_TRIPLET_WORDS))

COMMON_CFLAGS = -g $(C_OPT) -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wmissing-declarations -fno-strict-aliasing -I$(SPDK_ROOT_DIR)/include

ifneq ($(filter powerpc%,$(TARGET_MACHINE)),)
COMMON_CFLAGS += -mcpu=$(TARGET_ARCHITECTURE)
else ifeq ($(TARGET_MACHINE),aarch64)
ifeq ($(TARGET_ARCHITECTURE),native)
COMMON_CFLAGS += -march=armv8-a+crc
else
COMMON_CFLAGS += -march=$(TARGET_ARCHITECTURE)
endif
COMMON_CFLAGS += -DPAGE_SIZE=$(shell getconf PAGESIZE)
else
COMMON_CFLAGS += -march=$(TARGET_ARCHITECTURE)
endif

ifeq ($(CONFIG_WERROR), y)
COMMON_CFLAGS += -Werror
endif

ifeq ($(CONFIG_LTO),y)
COMMON_CFLAGS += -flto
LDFLAGS += -flto
endif

ifeq ($(CONFIG_PGO_CAPTURE),y)
COMMON_CFLAGS += -fprofile-generate=$(SPDK_ROOT_DIR)/build/pgo
LDFLAGS += -fprofile-generate=$(SPDK_ROOT_DIR)/build/pgo
endif

ifeq ($(CONFIG_PGO_USE),y)
COMMON_CFLAGS += -fprofile-use=$(SPDK_ROOT_DIR)/build/pgo
LDFLAGS += -fprofile-use=$(SPDK_ROOT_DIR)/build/pgo
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

# Specify the linker to use
ifneq ($(LD_TYPE),)
LDFLAGS += -fuse-ld=$(LD_TYPE)
endif

ifeq ($(OS),FreeBSD)
SYS_LIBS += -L/usr/local/lib
COMMON_CFLAGS += -I/usr/local/include
endif

# Attach only if PMDK lib specified with configure
ifneq ($(CONFIG_PMDK_DIR),)
LIBS += -L$(CONFIG_PMDK_DIR)/src/nondebug
COMMON_CFLAGS += -I$(CONFIG_PMDK_DIR)/src/include
endif

ifeq ($(CONFIG_RDMA),y)
SYS_LIBS += -libverbs -lrdmacm
endif

ifeq ($(CONFIG_URING),y)
SYS_LIBS += -luring
ifneq ($(strip $(CONFIG_URING_PATH)),)
CFLAGS += -I$(CONFIG_URING_PATH)
LDFLAGS += -L$(CONFIG_URING_PATH)
endif
endif

IPSEC_MB_DIR=$(SPDK_ROOT_DIR)/intel-ipsec-mb

ISAL_DIR=$(SPDK_ROOT_DIR)/isa-l
ifeq ($(CONFIG_ISAL), y)
SYS_LIBS += -L$(ISAL_DIR)/.libs -lisal
COMMON_CFLAGS += -I$(ISAL_DIR)/..
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

SPDK_GIT_COMMIT := $(shell git rev-parse --short HEAD 2>/dev/null)
ifneq (, $(SPDK_GIT_COMMIT))
COMMON_CFLAGS += -DSPDK_GIT_COMMIT=$(SPDK_GIT_COMMIT)
endif

COMMON_CFLAGS += -pthread
LDFLAGS += -pthread

CFLAGS   += $(COMMON_CFLAGS) -Wno-pointer-sign -Wstrict-prototypes -Wold-style-definition -std=gnu99
CXXFLAGS += $(COMMON_CFLAGS)

SYS_LIBS += -lrt
SYS_LIBS += -luuid
SYS_LIBS += -lcrypto

ifneq ($(CONFIG_NVME_CUSE)$(CONFIG_FUSE),nn)
SYS_LIBS += -lfuse3
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

ENV_LDFLAGS = $(if $(SPDK_NO_LINK_ENV),,$(ENV_LINKER_ARGS))

# Link $(OBJS) and $(LIBS) into $@ (app)
LINK_C=\
	$(Q)echo "  LINK $(notdir $@)"; \
	$(CC) -o $@ $(CPPFLAGS) $(LDFLAGS) $(OBJS) $(LIBS) $(ENV_LDFLAGS) $(SYS_LIBS)

LINK_CXX=\
	$(Q)echo "  LINK $(notdir $@)"; \
	$(CXX) -o $@ $(CPPFLAGS) $(LDFLAGS) $(OBJS) $(LIBS) $(ENV_LDFLAGS) $(SYS_LIBS)

# Provide function to ease build of a shared lib
define spdk_build_realname_shared_lib
	$(CC) -o $@ -shared $(CPPFLAGS) $(LDFLAGS) \
	    -Wl,-rpath=$(DESTDIR)/$(libdir) \
	    -Wl,--soname,$(notdir $@) \
	    -Wl,--whole-archive $(1) -Wl,--no-whole-archive \
	    -Wl,--version-script=$(2) \
	    $(3) -Wl,--no-as-needed $(4) -Wl,--as-needed
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

# Uninstall a library
UNINSTALL_LIB=\
	$(Q)echo "  UNINSTALL $(DESTDIR)$(libdir)/$(notdir $(LIB))";\
	rm -f "$(DESTDIR)$(libdir)/$(notdir $(LIB))"; \
	if [ -d "$(DESTDIR)$(libdir)" ] && [ $$(ls -A "$(DESTDIR)$(libdir)" | wc -l) -eq 0 ]; then rm -rf "$(DESTDIR)$(libdir)"; fi

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
	if file --mime-type $(SHARED_REALNAME_LIB) | grep -q 'application/x-sharedlib'; then \
		perm_mode=755; \
	else \
		perm_mode=644; \
	fi; \
	install -m $$perm_mode "$(SHARED_REALNAME_LIB)" "$(DESTDIR)$(libdir)/"; \
	$(call spdk_install_lib_symlink,$(notdir $(SHARED_REALNAME_LIB)),$(notdir $(SHARED_LINKED_LIB)));

# Uninstall an shared library
UNINSTALL_SHARED_LIB=\
	$(Q)echo "  UNINSTALL $(DESTDIR)$(libdir)/$(notdir $(SHARED_LINKED_LIB))"; \
	rm -f "$(DESTDIR)$(libdir)/$(notdir $(SHARED_LINKED_LIB))"; \
	rm -f "$(DESTDIR)$(libdir)/$(notdir $(SHARED_REALNAME_LIB))"; \
	if [ -d "$(DESTDIR)$(libdir)" ] && [ $$(ls -A "$(DESTDIR)$(libdir)" | wc -l) -eq 0 ]; then rm -rf "$(DESTDIR)$(libdir)"; fi


# Install an app binary
INSTALL_APP=\
	$(Q)echo "  INSTALL $(DESTDIR)$(bindir)/$(notdir $<)"; \
	install -d -m 755 "$(DESTDIR)$(bindir)"; \
	install -m 755 "$<" "$(DESTDIR)$(bindir)/"

# Uninstall an app binary
UNINSTALL_APP=\
        $(Q)echo "  UNINSTALL $(DESTDIR)$(bindir)/$(notdir $(APP))"; \
	rm -f "$(DESTDIR)$(bindir)/$(notdir $(APP))"; \
	if [ -d "$(DESTDIR)$(bindir)" ] && [ $$(ls -A "$(DESTDIR)$(bindir)" | wc -l) -eq 0 ]; then rm -rf "$(DESTDIR)$(bindir)"; fi

INSTALL_EXAMPLE=\
	$(Q)echo "  INSTALL $(DESTDIR)$(bindir)/spdk_$(strip $(subst /,_,$(subst $(SPDK_ROOT_DIR)/examples/, ,$(CURDIR))))"; \
	install -d -m 755 "$(DESTDIR)$(bindir)"; \
	install -m 755 "$<" "$(DESTDIR)$(bindir)/spdk_$(strip $(subst /,_,$(subst $(SPDK_ROOT_DIR)/examples/, ,$(CURDIR))))"

# Uninstall an example binary
UNINSTALL_EXAMPLE=\
	$(Q)echo "  UNINSTALL $(DESTDIR)$(bindir)/spdk_$(strip $(subst /,_,$(subst $(SPDK_ROOT_DIR)/examples/, ,$(CURDIR))))"; \
	rm -f "$(DESTDIR)$(bindir)/spdk_$(strip $(subst /,_,$(subst $(SPDK_ROOT_DIR)/examples/, ,$(CURDIR))))"; \
	if [ -d "$(DESTDIR)$(bindir)" ] && [ $$(ls -A "$(DESTDIR)$(bindir)" | wc -l) -eq 0 ]; then rm -rf "$(DESTDIR)$(bindir)"; fi

# Install a header
INSTALL_HEADER=\
	$(Q)echo "  INSTALL $@"; \
	install -d -m 755 "$(DESTDIR)$(includedir)/$(dir $(patsubst $(DESTDIR)$(includedir)/%,%,$@))"; \
	install -m 644 "$(patsubst $(DESTDIR)$(includedir)/%,%,$@)" "$(DESTDIR)$(includedir)/$(dir $(patsubst $(DESTDIR)$(includedir)/%,%,$@))";

# Uninstall a header
UNINSTALL_HEADER=\
	$(Q)echo "  UNINSTALL $@"; \
	rm -rf "$(DESTDIR)$(includedir)/$(dir $(patsubst $(DESTDIR)$(includedir)/%,%,$@))$(notdir $@)"; \
	if [ -d "$(DESTDIR)$(includedir)/$(dir $(patsubst $(DESTDIR)$(includedir)/%,%,$@))" ] \
	&& [ $$(ls -A "$(DESTDIR)$(includedir)/$(dir $(patsubst $(DESTDIR)$(includedir)/%,%,$@))" | wc -l) -eq 0 ]; \
	then rm -rf "$(DESTDIR)$(includedir)/$(dir $(patsubst $(DESTDIR)$(includedir)/%,%,$@))"; fi

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

define add_no_as_needed
-Wl,--no-as-needed $(1) -Wl,-as-needed
endef

define add_whole_archive
-Wl,--whole-archive $(1) -Wl,--no-whole-archive
endef
