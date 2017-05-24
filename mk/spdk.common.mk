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

-include $(SPDK_ROOT_DIR)/CONFIG.local
include $(SPDK_ROOT_DIR)/CONFIG

-include $(SPDK_ROOT_DIR)/mk/cc.mk

C_OPT ?= -fno-omit-frame-pointer
ifneq ($(V),1)
Q ?= @
endif
S ?= $(notdir $(CURDIR))

ifeq ($(MAKECMDGOALS),)
MAKECMDGOALS=$(.DEFAULT_GOAL)
endif

OS := $(shell uname)

COMMON_CFLAGS = -g $(C_OPT) -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wmissing-declarations -fno-strict-aliasing -march=native -m64 -I$(SPDK_ROOT_DIR)/include

COMMON_CFLAGS += -include $(SPDK_ROOT_DIR)/config.h

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
LIBS += -L/usr/local/lib
COMMON_CFLAGS += -I/usr/local/include
endif

ifeq ($(CONFIG_DEBUG), y)
COMMON_CFLAGS += -DDEBUG -O0
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

MAKEFLAGS += --no-print-directory

C_SRCS += $(C_SRCS-y)
CXX_SRCS += $(CXX_SRCS-y)

OBJS = $(C_SRCS:.c=.o) $(CXX_SRCS:.cpp=.o)

DEPFLAGS = -MMD -MP -MF $*.d.tmp

# Compile first input $< (.c) into $@ (.o)
COMPILE_C=\
	$(Q)echo "  CC $S/$@"; \
	$(CC) -o $@ $(DEPFLAGS) $(CFLAGS) -c $< && \
	mv -f $*.d.tmp $*.d

COMPILE_CXX=\
	$(Q)echo "  CXX $S/$@"; \
	$(CXX) -o $@ $(DEPFLAGS) $(CXXFLAGS) -c $< && \
	mv -f $*.d.tmp $*.d

# Link $(OBJS) and $(LIBS) into $@ (app)
LINK_C=\
	$(Q)echo "  LINK $S/$@"; \
	$(CC) -o $@ $(CPPFLAGS) $(LDFLAGS) $(OBJS) $(LIBS) $(SYS_LIBS)

LINK_CXX=\
	$(Q)echo "  LINK $S/$@"; \
	$(CXX) -o $@ $(CPPFLAGS) $(LDFLAGS) $(OBJS) $(LIBS) $(SYS_LIBS)

# Archive $(OBJS) into $@ (.a)
LIB_C=\
	$(Q)echo "  LIB $(notdir $@)"; \
	rm -f $@; \
	$(CCAR) crDs $@ $(OBJS)

# Clean up generated files listed as arguments plus a default list
CLEAN_C=\
	$(Q)rm -f *.a *.o *.d *.d.tmp *.gcno *.gcda

%.o: %.c %.d $(MAKEFILE_LIST)
	$(COMPILE_C)

%.o: %.cpp %.d $(MAKEFILE_LIST)
	$(COMPILE_CXX)

%.d: ;

define spdk_lib_list_to_files
	$(1:%=$(SPDK_ROOT_DIR)/build/lib/libspdk_%.a)
endef
