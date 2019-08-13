# SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)

# APP = try_blobfs

# include $(SPDK_ROOT_DIR)/lib/rocksdb/spdk.rocksdb.mk


SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

APP = testspeed_blobfs

C_SRCS := testspeed_blobfs.c

# Don't link bdev_lvol in blobcli - otherwise this utility cannot operate on an lvolstore
SPDK_LIB_LIST = $(filter-out bdev_lvol,$(ALL_MODULES_LIST))
SPDK_LIB_LIST += event_bdev event_copy event_vmd
SPDK_LIB_LIST += bdev copy event thread util conf trace \
		log jsonrpc json rpc sock notify

#include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
include $(SPDK_ROOT_DIR)/lib/rocksdb/spdk.rocksdb.mk
