SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 1
SO_MINOR := 0

CFLAGS += -I$(SPDK_ROOT_DIR)/module/bdev/wal/
CFLAGS += -I$(SPDK_ROOT_DIR)/lib/bdev/

C_SRCS = vbdev_wal.c bdev_wal_rpc.c
LIBNAME = bdev_wal

SPDK_MAP_FILE = $(SPDK_ROOT_DIR)/mk/spdk_blank.map

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
