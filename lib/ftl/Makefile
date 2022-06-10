#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 5
SO_MINOR := 0

ifdef SPDK_FTL_VSS_EMU
CFLAGS += -DSPDK_FTL_VSS_EMU
endif

ifneq ($(strip $(SPDK_FTL_ZONE_EMU_BLOCKS)),)
CFLAGS += -DSPDK_FTL_ZONE_EMU_BLOCKS=$(SPDK_FTL_ZONE_EMU_BLOCKS)
endif

CFLAGS += -I.

FTL_SUBDIRS := mngt utils

C_SRCS = ftl_core.c ftl_init.c ftl_layout.c ftl_debug.c ftl_io.c ftl_sb.c ftl_l2p.c ftl_l2p_flat.c
C_SRCS += ftl_nv_cache.c ftl_band.c
C_SRCS += mngt/ftl_mngt.c mngt/ftl_mngt_bdev.c mngt/ftl_mngt_shutdown.c mngt/ftl_mngt_startup.c
C_SRCS += mngt/ftl_mngt_md.c mngt/ftl_mngt_misc.c mngt/ftl_mngt_ioch.c mngt/ftl_mngt_l2p.c
C_SRCS += mngt/ftl_mngt_band.c
C_SRCS += utils/ftl_conf.c utils/ftl_md.c utils/ftl_mempool.c

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
