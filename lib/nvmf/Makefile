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

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 7
SO_MINOR := 0

C_SRCS = ctrlr.c ctrlr_discovery.c ctrlr_bdev.c \
	 subsystem.c nvmf.c nvmf_rpc.c transport.c tcp.c

C_SRCS-$(CONFIG_RDMA) += rdma.c
LIBNAME = nvmf
LOCAL_SYS_LIBS =
ifeq ($(CONFIG_RDMA),y)
LOCAL_SYS_LIBS += -libverbs -lrdmacm
#Attach only if FreeBSD and RDMA is specified with configure
ifeq ($(OS),FreeBSD)
# Mellanox - MLX4 HBA Userspace Library
ifneq ("$(wildcard /usr/lib/libmlx4.*)","")
LOCAL_SYS_LIBS += -lmlx4
endif
# Mellanox - MLX5 HBA Userspace Library
ifneq ("$(wildcard /usr/lib/libmlx5.*)","")
LOCAL_SYS_LIBS += -lmlx5
endif
# Chelsio HBA Userspace Library
ifneq ("$(wildcard /usr/lib/libcxgb4.*)","")
LOCAL_SYS_LIBS += -lcxgb4
endif
endif
endif

ifeq ($(CONFIG_FC),y)
C_SRCS += fc.c fc_ls.c
CFLAGS += -I$(CURDIR)
ifneq ($(strip $(CONFIG_FC_PATH)),)
CFLAGS += -I$(CONFIG_FC_PATH)
endif
endif

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_nvmf.map)

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
