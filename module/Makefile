#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

DIRS-y = bdev blob blobfs accel event sock scheduler
ifeq ($(SPDK_ROOT_DIR)/lib/env_dpdk,$(CONFIG_ENV))
DIRS-y += env_dpdk
endif

ifeq ($(CONFIG_VFIO_USER), y)
DIRS-y += vfu_device
endif

DEPDIRS-blob :=
DEPDIRS-accel :=
DEPDIRS-env_dpdk :=
DEPDIRS-sock :=
DEPDIRS-scheduler :=
DEPDIRS-bdev := blob
DEPDIRS-blobfs := blob
DEPDIRS-event := bdev blob
DEPDIRS-vfu_device :=

.PHONY: all clean $(DIRS-y)

BDEV_MODULES_PKGCONFIG = $(call pkgconfig_filename,spdk_bdev_modules)
ACCEL_MODULES_PKGCONFIG = $(call pkgconfig_filename,spdk_accel_modules)
SOCK_MODULES_PKGCONFIG = $(call pkgconfig_filename,spdk_sock_modules)
SCHEDULER_MODULES_PKGCONFIG = $(call pkgconfig_filename,spdk_scheduler_modules)
SYSLIBS_PKGCONFIG = $(call pkgconfig_filename,spdk_syslibs)

PRIVATE_SYSLIBS = $(SYS_LIBS)
ifeq ($(SPDK_ROOT_DIR)/lib/env_dpdk,$(CONFIG_ENV))
PRIVATE_SYSLIBS += $(DPDK_PRIVATE_LINKER_ARGS)
endif

MODULES_PKGCONFIG = $(BDEV_MODULES_PKGCONFIG) $(ACCEL_MODULES_PKGCONFIG)
MODULES_PKGCONFIG += $(SOCK_MODULES_PKGCONFIG) $(SYSLIBS_PKGCONFIG)
MODULES_PKGCONFIG += $(SCHEDULER_MODULES_PKGCONFIG)

$(BDEV_MODULES_PKGCONFIG):
	$(Q)$(SPDK_ROOT_DIR)/scripts/pc_modules.sh bdev "$(BLOCKDEV_MODULES_LIST:%=spdk_%)" > $@

$(ACCEL_MODULES_PKGCONFIG):
	$(Q)$(SPDK_ROOT_DIR)/scripts/pc_modules.sh accel "$(ACCEL_MODULES_LIST:%=spdk_%)" > $@

$(SOCK_MODULES_PKGCONFIG):
	$(Q)$(SPDK_ROOT_DIR)/scripts/pc_modules.sh sock "$(SOCK_MODULES_LIST:%=spdk_%)" > $@

$(SCHEDULER_MODULES_PKGCONFIG):
	$(Q)$(SPDK_ROOT_DIR)/scripts/pc_modules.sh scheduler "$(SCHEDULER_MODULES_LIST:%=spdk_%)" > $@

$(SYSLIBS_PKGCONFIG):
	$(Q)$(SPDK_ROOT_DIR)/scripts/pc_libs.sh "" "$(PRIVATE_SYSLIBS)" System spdk_syslibs > $@

all: $(DIRS-y) $(MODULES_PKGCONFIG)

install: all
	@$(foreach mod, $(MODULES_PKGCONFIG), $(call pkgconfig_install,$(mod)))

uninstall: $(DIRS-y)
	@$(foreach mod, $(MODULES_PKGCONFIG), $(call pkgconfig_uninstall,$(mod)))

clean: $(DIRS-y)

include $(SPDK_ROOT_DIR)/mk/spdk.subdirs.mk
