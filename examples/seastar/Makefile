FLAGS := $(shell pkg-config --libs --cflags --static ../../../seastar/build/debug/seastar.pc)

SPDK_ROOT_DIR = ../..
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.app_vars.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

FLAGS += -g -O0 -I$(SPDK_ROOT_DIR)/include -Iinclude/

SPDK_LIB_LIST = $(ALL_MODULES_LIST)
SPDK_LIB_LIST += event_bdev event_copy event_nvmf event_vmd event_iscsi event_net event_scsi
SPDK_LIB_LIST += nvmf bdev_rpc bdev iscsi scsi net copy event util conf trace log
SPDK_LIB_LIST += jsonrpc json rpc sock thread notify

FLAGS += $(SPDK_LIB_LINKER_ARGS) $(ENV_LINKER_ARGS)
FLAGS += $(SYS_LIBS)

.PHONY : nvme spdk_tgt sock

all: nvme spdk_tgt sock

nvme:
	g++ nvme.cc $(FLAGS) -o nvme

spdk_tgt:
	g++ spdk_tgt.cc $(FLAGS) -o spdk_tgt

sock:
	g++ sock.cc $(FLAGS) -o sock
