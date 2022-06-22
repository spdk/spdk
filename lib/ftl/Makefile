#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 5
SO_MINOR := 0

CFLAGS += -I.

FTL_SUBDIRS := mngt utils

C_SRCS = ftl_core.c
C_SRCS += mngt/ftl_mngt.c

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_ftl.map)

LIBNAME = ftl

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk

# TODO: This should be handled by spdk.subdirs.mk
CLEAN_FTL_SUBDIRS = $(addprefix clean_ftl_, $(FTL_SUBDIRS))

clean: $(CLEAN_FTL_SUBDIRS)

.PHONY: $(CLEAN_FTL_SUBDIRS)
$(CLEAN_FTL_SUBDIRS): _Q := $(Q)
$(CLEAN_FTL_SUBDIRS): Q :=
$(CLEAN_FTL_SUBDIRS): clean_ftl_%:%
	$(_Q)cd $< && $(CLEAN_C)
