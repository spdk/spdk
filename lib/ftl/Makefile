#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 6
SO_MINOR := 0

ifdef SPDK_FTL_VSS_EMU
CFLAGS += -DSPDK_FTL_VSS_EMU
endif

ifdef SPDK_FTL_RETRY_ON_ERROR
CFLAGS += -DSPDK_FTL_RETRY_ON_ERROR
endif

ifdef SPDK_FTL_L2P_FLAT
CFLAGS += -DSPDK_FTL_L2P_FLAT
endif

ifneq ($(strip $(SPDK_FTL_ZONE_EMU_BLOCKS)),)
CFLAGS += -DSPDK_FTL_ZONE_EMU_BLOCKS=$(SPDK_FTL_ZONE_EMU_BLOCKS)
endif

CFLAGS += -I.

FTL_SUBDIRS := mngt utils upgrade

C_SRCS = ftl_core.c ftl_init.c ftl_layout.c ftl_debug.c ftl_io.c ftl_sb.c ftl_l2p.c ftl_l2p_flat.c
C_SRCS += ftl_nv_cache.c ftl_band.c ftl_band_ops.c ftl_writer.c ftl_rq.c ftl_reloc.c ftl_l2p_cache.c
C_SRCS += ftl_p2l.c ftl_trace.c
C_SRCS += mngt/ftl_mngt.c mngt/ftl_mngt_bdev.c mngt/ftl_mngt_shutdown.c mngt/ftl_mngt_startup.c
C_SRCS += mngt/ftl_mngt_md.c mngt/ftl_mngt_misc.c mngt/ftl_mngt_ioch.c mngt/ftl_mngt_l2p.c
C_SRCS += mngt/ftl_mngt_band.c mngt/ftl_mngt_self_test.c mngt/ftl_mngt_p2l.c
C_SRCS += mngt/ftl_mngt_recovery.c mngt/ftl_mngt_upgrade.c
C_SRCS += utils/ftl_conf.c utils/ftl_md.c utils/ftl_mempool.c utils/ftl_bitmap.c
C_SRCS += upgrade/ftl_layout_upgrade.c upgrade/ftl_sb_upgrade.c upgrade/ftl_p2l_upgrade.c
C_SRCS += upgrade/ftl_band_upgrade.c upgrade/ftl_chunk_upgrade.c

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
