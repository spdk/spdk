#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) Intel Corporation.
#  All rights reserved.
#

include $(SPDK_ROOT_DIR)/mk/spdk.app_vars.mk

# Applications in app/ go into build/bin/.
# Applications in examples/ go into build/examples/.
# Use findstring to identify if the current directory is in the app
# or examples directory. If it is, change the APP location.
APP_NAME := $(notdir $(APP))
ifneq (,$(findstring $(SPDK_ROOT_DIR)/app,$(CURDIR)))
	APP := $(APP_NAME:%=$(SPDK_ROOT_DIR)/build/bin/%)
else
ifneq (,$(findstring $(SPDK_ROOT_DIR)/examples,$(CURDIR)))
	APP := $(APP_NAME:%=$(SPDK_ROOT_DIR)/build/examples/%)
endif
endif

APP := $(APP)$(EXEEXT)

LIBS += $(SPDK_LIB_LINKER_ARGS)

CLEAN_FILES = $(APP)

ifeq ($(findstring vfio_user,$(SPDK_LIB_FILES)),vfio_user)
VFIO_USER_LIB_FILE=$(VFIO_USER_LIBRARY_DIR)/libvfio-user.a
endif

all : $(APP)
	@:

install: empty_rule

uninstall: empty_rule

# To avoid overwriting warning
empty_rule:
	@:

$(APP) : $(OBJS) $(SPDK_LIB_FILES) $(ENV_LIBS) $(VFIO_USER_LIB_FILE)
	$(LINK_C)

clean :
	$(CLEAN_C) $(CLEAN_FILES)

include $(SPDK_ROOT_DIR)/mk/spdk.deps.mk
