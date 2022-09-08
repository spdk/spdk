#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 11
SO_MINOR := 0

CFLAGS += $(ENV_CFLAGS)
C_SRCS = env.c memory.c pci.c init.c threads.c
C_SRCS += pci_ioat.c pci_virtio.c pci_vmd.c pci_idxd.c
C_SRCS += pci_event.c sigbus_handler.c pci_dpdk.c
C_SRCS += pci_dpdk_2207.c pci_dpdk_2211.c
LIBNAME = env_dpdk

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_env_dpdk.map)

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk

LIBDPDK_PKGCONFIG = $(call pkgconfig_filename,spdk_dpdklibs)

$(LIBDPDK_PKGCONFIG): $(PKGCONFIG) $(PKGCONFIG_INST)
	$(Q)$(SPDK_ROOT_DIR)/scripts/pc_libs.sh \
		"-L$(DPDK_LIB_DIR) $(DPDK_LIB_LIST:%=-l%)" "" DPDK spdk_dpdklibs > $@
	$(Q)sed -i.bak '5s,.*,Requires: $(DEPDIRS-$(LIBNAME):%=spdk_%) spdk_dpdklibs,' $(PKGCONFIG) ; rm $(PKGCONFIG).bak
	$(Q)sed -i.bak '5s,.*,Requires: $(DEPDIRS-$(LIBNAME):%=spdk_%) spdk_dpdklibs,' $(PKGCONFIG_INST) ; rm $(PKGCONFIG_INST).bak

_install_dpdklibs: $(LIBDPDK_PKGCONFIG)
	@$(call pkgconfig_install,$(LIBDPDK_PKGCONFIG))

_uninstall_dpdklibs: $(LIBDPDK_PKGCONFIG)
	@$(call pkgconfig_uninstall,$(LIBDPDK_PKGCONFIG))

all: $(LIBDPDK_PKGCONFIG)
install: _install_dpdklibs
uninstall: _uninstall_dpdklibs
