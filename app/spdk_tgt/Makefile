#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2018 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

APP = spdk_tgt

C_SRCS := spdk_tgt.c

SPDK_LIB_LIST = $(ALL_MODULES_LIST)

SPDK_LIB_LIST += event event_iscsi event_nvmf

ifeq ($(SPDK_ROOT_DIR)/lib/env_dpdk,$(CONFIG_ENV))
SPDK_LIB_LIST += env_dpdk_rpc
endif

ifeq ($(OS),Linux)
SPDK_LIB_LIST += event_nbd
ifeq ($(CONFIG_UBLK),y)
SPDK_LIB_LIST += event_ublk
endif
ifeq ($(CONFIG_VHOST),y)
SPDK_LIB_LIST += event_vhost_blk event_vhost_scsi
endif
ifeq ($(CONFIG_VFIO_USER),y)
SPDK_LIB_LIST += event_vfu_tgt
endif
endif

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk

install: $(APP)
	$(INSTALL_APP)

uninstall:
	$(UNINSTALL_APP)
