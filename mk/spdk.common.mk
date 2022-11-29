#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  Copyright (c) 2017, IBM Corporation.
#  Copyright (c) 2019, 2021 Mellanox Corporation.
#  Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES
#  All rights reserved.
#  Copyright (c) 2022 Dell Inc, or its subsidiaries.
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
EXEEXT?=

ifneq ($(prefix),)
CONFIG_PREFIX=$(prefix)
endif

bindir?=$(CONFIG_PREFIX)/bin
ifeq ($(CONFIG_LIBDIR),)
libdir?=$(CONFIG_PREFIX)/lib
else
libdir?=$(CONFIG_LIBDIR)
endif
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
ifneq ($(filter mingw% windows%,$(TARGET_TRIPLET_WORDS)),)
OS = Windows
endif

TARGET_ARCHITECTURE ?= $(CONFIG_ARCH)
TARGET_MACHINE := $(firstword $(TARGET_TRIPLET_WORDS))

ifeq ($(OS),Windows)
EXEEXT = .exe
endif

COMMON_CFLAGS = -g $(C_OPT) -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wmissing-declarations -fno-strict-aliasing -I$(SPDK_ROOT_DIR)/include

ifneq ($(filter powerpc% ppc%,$(TARGET_MACHINE)),)
COMMON_CFLAGS += -mcpu=$(TARGET_ARCHITECTURE)
else ifeq ($(TARGET_MACHINE),aarch64)
COMMON_CFLAGS += -march=$(TARGET_ARCHITECTURE)
COMMON_CFLAGS += -DPAGE_SIZE=$(shell getconf PAGESIZE)
else ifeq ('$(TARGET_MACHINE)|$(TARGET_ARCHITECTURE)','riscv64|native')
# -march=native is not yet supported by GCC on RISC-V. Falling back to default.
else ifneq ($(filter loongarch%,$(TARGET_MACHINE)),)
COMMON_CFLAGS += -march=$(TARGET_ARCHITECTURE)
COMMON_CFLAGS += -DPAGE_SIZE=$(shell getconf PAGESIZE)
else
COMMON_CFLAGS += -march=$(TARGET_ARCHITECTURE)
endif

ifeq ($(TARGET_MACHINE),x86_64)
ifeq ($(CC_TYPE),gcc)
ifneq (,$(shell $(CC) --target-help 2>/dev/null | grep -e -mavx512f >/dev/null && echo 1))
# Don't use AVX-512 instructions in SPDK code - it breaks Valgrind for
# some cases where compiler decides to hyper-optimize a relatively
# simple operation (like int-to-float conversion) using AVX-512
COMMON_CFLAGS += -mno-avx512f
endif
endif
ifeq ($(CC_TYPE),clang)
LLC=llc$(shell echo $(CC) | grep -o -E  "\-[0-9]{2}")
ifneq (,$(shell $(LLC) -march=x86-64 -mattr=help 2>&1 | grep -e avx512f >/dev/null && echo 1))
COMMON_CFLAGS += -mno-avx512f
endif
endif
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

ifeq ($(CONFIG_CET),y)
COMMON_CFLAGS += -fcf-protection
LDFLAGS += -fcf-protection
endif

COMMON_CFLAGS += -Wformat -Wformat-security

COMMON_CFLAGS += -D_GNU_SOURCE

# Always build PIC code so that objects can be used in shared libs and position-independent executables
COMMON_CFLAGS += -fPIC

# Enable stack buffer overflow checking
COMMON_CFLAGS += -fstack-protector

ifeq ($(OS).$(CC_TYPE),Windows.gcc)
# Workaround for gcc bug 86832 - invalid TLS usage
COMMON_CFLAGS += -mstack-protector-guard=global
endif

# Prevent accidental multiple definitions of global variables
COMMON_CFLAGS += -fno-common

# Enable full RELRO - no lazy relocation (resolve everything at load time).
# This allows the GOT to be made read-only early in the loading process.
ifneq ($(OS),Windows)
LDFLAGS += -Wl,-z,relro,-z,now
endif

# Make the stack non-executable.
# This is the default in most environments, but it doesn't hurt to set it explicitly.
ifneq ($(OS),Windows)
LDFLAGS += -Wl,-z,noexecstack
endif

# Specify the linker to use
ifneq ($(LD_TYPE),)
LDFLAGS += -fuse-ld=$(LD_TYPE)
endif

SYS_LIBS =

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

ifeq ($(CONFIG_AVAHI),y)
SYS_LIBS += -lavahi-common -lavahi-client
endif

IPSEC_MB_DIR=$(CONFIG_IPSEC_MB_DIR)

ISAL_DIR=$(SPDK_ROOT_DIR)/isa-l
ISAL_CRYPTO_DIR=$(SPDK_ROOT_DIR)/isa-l-crypto
ifeq ($(CONFIG_ISAL), y)
SYS_LIBS += -L$(ISAL_DIR)/.libs -lisal
COMMON_CFLAGS += -I$(ISAL_DIR)/..
ifeq ($(CONFIG_ISAL_CRYPTO), y)
SYS_LIBS += -L$(ISAL_CRYPTO_DIR)/.libs -lisal_crypto
COMMON_CFLAGS += -I$(ISAL_CRYPTO_DIR)/..
endif
endif

ifeq ($(CONFIG_VFIO_USER), y)
ifneq ($(CONFIG_VFIO_USER_DIR),)
VFIO_USER_SRC_DIR=$(CONFIG_VFIO_USER_DIR)
else
VFIO_USER_SRC_DIR=$(SPDK_ROOT_DIR)/libvfio-user
endif
ifeq ($(CONFIG_DEBUG), y)
VFIO_USER_BUILD_TYPE=debug
else
VFIO_USER_BUILD_TYPE=release
endif
ifeq ($(CONFIG_SHARED), y)
VFIO_USER_BUILD_SHARED=shared
else
VFIO_USER_BUILD_SHARED=static
endif
VFIO_USER_LIB_PREFIX=/usr/local/lib
VFIO_USER_BUILD_DIR=$(SPDK_ROOT_DIR)/build/libvfio-user/build-$(VFIO_USER_BUILD_TYPE)
VFIO_USER_INSTALL_DIR=$(SPDK_ROOT_DIR)/build/libvfio-user
VFIO_USER_INCLUDE_DIR=$(VFIO_USER_INSTALL_DIR)/usr/local/include
VFIO_USER_LIBRARY_DIR=$(VFIO_USER_INSTALL_DIR)$(VFIO_USER_LIB_PREFIX)

CFLAGS += -I$(VFIO_USER_INCLUDE_DIR)
LDFLAGS += -L$(VFIO_USER_LIBRARY_DIR)
SYS_LIBS += -lvfio-user -ljson-c
endif

ifeq ($(CONFIG_XNVME), y)
XNVME_DIR=$(SPDK_ROOT_DIR)/xnvme
XNVME_INSTALL_DIR=$(XNVME_DIR)/builddir/lib
XNVME_INCLUDE_DIR=$(XNVME_DIR)/include

CFLAGS += -I$(XNVME_INCLUDE_DIR)
LDFLAGS += -L$(XNVME_INSTALL_DIR)
SYS_LIBS += -lxnvme
ifneq ($(CONFIG_URING), y)
SYS_LIBS += -luring
endif
endif

ifeq ($(CONFIG_DAOS),y)
ifneq ($(CONFIG_DAOS_DIR),)
CFLAGS += -I$(CONFIG_DAOS_DIR)/include
LDFLAGS += -L$(CONFIG_DAOS_DIR)/lib64
endif
endif

ifeq ($(CONFIG_UBLK),y)
SYS_LIBS += -luring
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

ifeq ($(CONFIG_FC),y)
ifneq ($(strip $(CONFIG_FC_PATH)),)
SYS_LIBS += -L$(CONFIG_FC_PATH)
endif
SYS_LIBS += -lufc
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

ifeq ($(OS),Windows)
WPDK_DIR = $(abspath $(CONFIG_WPDK_DIR))
COMMON_CFLAGS += -I$(WPDK_DIR)/include/wpdk -I$(WPDK_DIR)/include
LDFLAGS += -L$(WPDK_DIR)/lib
ifeq ($(CONFIG_SHARED),y)
SYS_LIBS += -lwpdk
else
SYS_LIBS += $(WPDK_DIR)/lib/libwpdk.a
endif
SYS_LIBS += -ldbghelp -lkernel32 -lsetupapi -lws2_32 -lrpcrt4 -liphlpapi
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

ifeq ($(CONFIG_FUZZER),y)
COMMON_CFLAGS += -fsanitize=fuzzer-no-link
LDFLAGS += -fsanitize=fuzzer-no-link
SYS_LIBS += $(CONFIG_FUZZER_LIB)
endif

SPDK_GIT_COMMIT := $(shell git rev-parse --short HEAD 2>/dev/null)
ifneq (, $(SPDK_GIT_COMMIT))
COMMON_CFLAGS += -DSPDK_GIT_COMMIT=$(SPDK_GIT_COMMIT)
endif

ifneq ($(OS),Windows)
COMMON_CFLAGS += -pthread
SYS_LIBS += -pthread
endif

ifeq ($(CONFIG_IDXD_KERNEL),y)
SYS_LIBS += -laccel-config
endif

CFLAGS   += $(COMMON_CFLAGS) -Wno-pointer-sign -Wstrict-prototypes -Wold-style-definition -std=gnu99
CXXFLAGS += $(COMMON_CFLAGS) -std=c++11

SYS_LIBS += -lrt
SYS_LIBS += -luuid
SYS_LIBS += -lssl
SYS_LIBS += -lcrypto
SYS_LIBS += -lm

PKGCONF ?= pkg-config
ifneq ($(strip $(CONFIG_OPENSSL_PATH)),)
CFLAGS += -I$(CONFIG_OPENSSL_PATH)/include
LDFLAGS += -L$(CONFIG_OPENSSL_PATH)
else
# `libssl11` name is unique to Centos7 via EPEL
# So it's safe to add it here without additional check for Centos7
ifeq ($(shell $(PKGCONF) --exists libssl11 && echo 1),1)
CFLAGS  += $(shell $(PKGCONF) --cflags libssl11)
LDFLAGS += $(shell $(PKGCONF) --libs libssl11)
endif
endif

ifneq ($(CONFIG_NVME_CUSE)$(CONFIG_FUSE),nn)
SYS_LIBS += -lfuse3
endif

ifeq ($(OS).$(CC_TYPE),Windows.gcc)
# Include libssp.a for stack-protector and _FORTIFY_SOURCE
SYS_LIBS += -l:libssp.a
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
	$(1) -o $@ -shared $(CPPFLAGS) $(LDFLAGS) \
	    -Wl,-rpath=$(DESTDIR)/$(libdir) \
	    -Wl,--soname,$(notdir $@) \
	    -Wl,--whole-archive $(2) -Wl,--no-whole-archive \
	    -Wl,--version-script=$(3) \
	    $(4) -Wl,--no-as-needed $(5) -Wl,--as-needed
endef

BUILD_LINKERNAME_LIB=\
	ln -sf $(notdir $<) $@

# Archive $(OBJS) into $@ (.a)
LIB_C=\
	$(Q)echo "  LIB $(notdir $@)"; \
	rm -f $@; \
	mkdir -p $(dir $@); \
	$(CCAR) crDs $@ $(OBJS)

# Clean up generated files listed as arguments plus a default list
CLEAN_C=\
	$(Q)rm -f *.a *.lib *.o *.obj *.d *.d.tmp *.pdb *.gcno *.gcda

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

define pkgconfig_install
	echo "  INSTALL $(DESTDIR)$(libdir)/pkgconfig/$(notdir $(1))";
	install -d -m 755 "$(DESTDIR)$(libdir)/pkgconfig";
	install -m 644 "$(1)" "$(DESTDIR)$(libdir)/pkgconfig";
endef

define pkgconfig_uninstall
	echo "  UNINSTALL $(DESTDIR)$(libdir)/pkgconfig/$(notdir $(1))";
	rm -f "$(DESTDIR)$(libdir)/pkgconfig/$(notdir $(1))";
	if [ -d "$(DESTDIR)$(libdir)/pkgconfig" ] && [ $$(ls -A "$(DESTDIR)$(libdir)/pkgconfig" | wc -l) -eq 0 ]; then rm -rf "$(DESTDIR)$(libdir)/pkgconfig"; fi;
endef

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
	if file $(SHARED_REALNAME_LIB) | grep -q 'LSB shared object'; then \
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

define pkgconfig_filename
$(SPDK_ROOT_DIR)/build/lib/pkgconfig/$(1).pc
endef
