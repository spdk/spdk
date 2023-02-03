#ARCH
ARCH := $(shell uname -p)


SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

dpa_devdir = device
CROSS_CC = /opt/mellanox/doca/tools/dpa-clang
dpa_dev_CFLAGS = -target riscv64-unknown-elf -ffreestanding -mabi=lp64 \
				  -mcmodel=medany -march=rv64imac_zbb_zbp_zbr_zbs_xrpfxp_xnvcc \
				  -mno-relax -nostdlib \
				  -Wdouble-promotion -fPIE -O2 \
				  -Wall -Werror

ifeq ($(ARCH), aarch64)
	SYSLIBINC = /usr/include/aarch64-linux-gnu
	dpa_dev_LDADD = -L /usr/lib/aarch64-linux-gnu
else
	SYSLIBINC = /usr/include/x86_64-linux-gnu
	dpa_dev_LDADD = -L /usr/lib/x86_64-linux-gnu
endif

INCDIR = -I /usr/include -I $(SYSLIBINC) -I . #-I $(top_builddir)
ADD_HEADER = -include ../vrdma_dpa_common.h 
DEFINES = -DE_MODE_LE

dpa_dev_LDFLAGS = -L/usr/include//lib -lflexio_dev -lflexio-libc -lflexio_os -Wl,--gc-sections \
				  -Wl,-uflexio_hw_rpc -Wl,-urpc_print_cfg \
				  -lclang_rt.builtins-riscv64 -z nognustack -z norelro \
				  -Wl,--no-rosegment
dpa_dev_SOURCES = vrdma_dpa_rpc.c vrdma_dpa_db.c vrdma_dpa_msix.c vrdma_dpa_dev.c
DPA_LINKER_SCRIPT = vrdma_dpa_linker.ld

.PHONY: all clean

all: $(dpa_dev_SOURCES)
	$(CROSS_CC) $(dpa_dev_CFLAGS) -o ../dpa_dev.elf $(dpa_dev_SOURCES) \
	 -T$(DPA_LINKER_SCRIPT) $(INCDIR) $(ADD_HEADER) $(DEFINES) \
	 $(dpa_dev_LDADD) $(dpa_dev_LDFLAGS)

clean:
	rm -f  dpa_dev.elf
#include $(SPDK_ROOT_DIR)/mk/spdk.app.mk


