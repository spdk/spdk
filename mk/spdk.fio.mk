#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2020 Intel Corporation.
#  Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES
#  All rights reserved.
#

include $(SPDK_ROOT_DIR)/mk/spdk.app_vars.mk

# Plugins go into build/fio/
FIO_PLUGIN := $(SPDK_ROOT_DIR)/build/fio/$(notdir $(FIO_PLUGIN))

LIBS += $(SPDK_LIB_LINKER_ARGS)

CFLAGS += -I$(CONFIG_FIO_SOURCE_DIR)
# Compiling against fio 3.19 on latest FreeBSD generates warnings so we
# cannot use -Werror
ifeq ($(OS),FreeBSD)
CFLAGS += -Wno-error
else ifeq ($(CC_TYPE),clang)
CFLAGS += -Wno-error
endif
LDFLAGS += -shared -rdynamic -Wl,-z,nodelete

# By default, clang uses static sanitizer libraries, which means that the executable needs to have
# them linked in.  Since we don't control how the fio binary is compiled, we need to use the shared
# libraries.
ifeq ($(CC_TYPE),clang)
ifneq ($(filter y,$(CONFIG_ASAN) $(CONFIG_UBSAN)),)
LDFLAGS += -shared-libsan
# clang's sanitizers aren't in ld's search path by default, so we need to add it manually
LDFLAGS += -Wl,-rpath=$(shell $(CC) -print-resource-dir)/lib
endif
endif

CLEAN_FILES = $(FIO_PLUGIN)

all : $(FIO_PLUGIN)
	@:

install: all
	$(Q)echo "  INSTALL $(DESTDIR)$(libdir)/fio/$(notdir $(FIO_PLUGIN))";
	$(Q)install -d -m 755 "$(DESTDIR)$(libdir)/fio";
	$(Q)install -m 755 "$(FIO_PLUGIN)" "$(DESTDIR)$(libdir)/fio"

uninstall:
	$(Q)echo "  UNINSTALL $(DESTDIR)$(libdir)/fio/$(notdir $(FIO_PLUGIN))";
	$(Q)rm -f "$(DESTDIR)$(libdir)/fio/$(notdir $(FIO_PLUGIN))";
	$(Q)if [ -d "$(DESTDIR)$(libdir)/fio" ] && [ $$(ls -A "$(DESTDIR)$(libdir)/fio" | wc -l) -eq 0 ]; then rm -rf "$(DESTDIR)$(libdir)/fio"; fi

$(FIO_PLUGIN) : $(OBJS) $(SPDK_LIB_FILES) $(ENV_LIBS)
	$(LINK_C)

clean :
	$(CLEAN_C) $(CLEAN_FILES)

include $(SPDK_ROOT_DIR)/mk/spdk.deps.mk
