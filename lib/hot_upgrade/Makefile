#
#  BSD LICENSE
#
#  Copyright (C) 2024 SPDK Hot Upgrade Contributors.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

C_SRCS = hot_upgrade.c hot_upgrade_rpc.c

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_hot_upgrade.map)
SO_VER := 1
SO_MINOR := 0

LIBNAME = hot_upgrade

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk