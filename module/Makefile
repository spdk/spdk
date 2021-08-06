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

SPDK_ROOT_DIR := $(abspath $(CURDIR)/..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

DIRS-y = bdev blob blobfs accel event sock scheduler
ifeq ($(SPDK_ROOT_DIR)/lib/env_dpdk,$(CONFIG_ENV))
DIRS-y += env_dpdk
endif

DEPDIRS-blob :=
DEPDIRS-accel :=
DEPDIRS-env_dpdk :=
DEPDIRS-sock :=
DEPDIRS-scheduler :=
DEPDIRS-bdev := blob
DEPDIRS-blobfs := blob
DEPDIRS-event := bdev blob

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
