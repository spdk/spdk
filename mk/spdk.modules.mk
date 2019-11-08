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

BLOCKDEV_MODULES_LIST = bdev_malloc bdev_null bdev_nvme bdev_passthru bdev_lvol
BLOCKDEV_MODULES_LIST += bdev_raid bdev_error bdev_gpt bdev_split bdev_delay
BLOCKDEV_MODULES_LIST += bdev_zone_block
BLOCKDEV_MODULES_LIST += blobfs blob_bdev blob lvol vmd nvme

ifeq ($(CONFIG_CRYPTO),y)
BLOCKDEV_MODULES_LIST += bdev_crypto
endif

ifeq ($(CONFIG_OCF),y)
BLOCKDEV_MODULES_LIST += bdev_ocf
BLOCKDEV_MODULES_LIST += ocfenv
endif

ifeq ($(CONFIG_REDUCE),y)
BLOCKDEV_MODULES_LIST += bdev_compress reduce
SYS_LIBS += -lpmem
endif

ifeq ($(CONFIG_RDMA),y)
SYS_LIBS += -libverbs -lrdmacm
endif

ifeq ($(OS),Linux)
BLOCKDEV_MODULES_LIST += ftl
BLOCKDEV_MODULES_LIST += bdev_aio
SYS_LIBS += -laio
ifeq ($(CONFIG_VIRTIO),y)
BLOCKDEV_MODULES_LIST += bdev_virtio virtio
endif
ifeq ($(CONFIG_ISCSI_INITIATOR),y)
BLOCKDEV_MODULES_LIST += bdev_iscsi
# Fedora installs libiscsi to /usr/lib64/iscsi for some reason.
SYS_LIBS += -L/usr/lib64/iscsi -liscsi
endif
endif

ifeq ($(CONFIG_URING),y)
BLOCKDEV_MODULES_LIST += bdev_uring
SYS_LIBS += -luring
ifneq ($(strip $(CONFIG_URING_PATH)),)
CFLAGS += -I$(CONFIG_URING_PATH)
LDFLAGS += -L$(CONFIG_URING_PATH)
endif
endif

ifeq ($(CONFIG_RBD),y)
BLOCKDEV_MODULES_LIST += bdev_rbd
SYS_LIBS += -lrados -lrbd
endif

ifeq ($(CONFIG_PMDK),y)
BLOCKDEV_MODULES_LIST += bdev_pmem
SYS_LIBS += -lpmemblk
endif

SOCK_MODULES_LIST = sock_posix

ifeq ($(CONFIG_VPP),y)
SYS_LIBS += -Wl,--whole-archive
ifneq ($(CONFIG_VPP_DIR),)
SYS_LIBS += -L$(CONFIG_VPP_DIR)/lib
endif
SYS_LIBS += -lvppinfra -lsvm -lvlibmemoryclient
SYS_LIBS += -Wl,--no-whole-archive
SOCK_MODULES_LIST += sock_vpp
endif

COPY_MODULES_LIST = copy_ioat ioat

ALL_MODULES_LIST = $(BLOCKDEV_MODULES_LIST) $(COPY_MODULES_LIST) $(SOCK_MODULES_LIST)
