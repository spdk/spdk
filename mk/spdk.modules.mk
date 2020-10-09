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
BLOCKDEV_MODULES_LIST += blobfs blobfs_bdev blob_bdev blob lvol vmd nvme

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
BLOCKDEV_MODULES_LIST += rdma
SYS_LIBS += -libverbs -lrdmacm
ifeq ($(CONFIG_RDMA_PROV),mlx5_dv)
SYS_LIBS += -lmlx5
endif
endif

ifeq ($(OS),Linux)
BLOCKDEV_MODULES_LIST += bdev_ftl ftl
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
SYS_LIBS += -lpmemblk -lpmem
endif

SOCK_MODULES_LIST = sock_posix

ifeq ($(OS), Linux)
ifeq ($(CONFIG_URING),y)
SOCK_MODULES_LIST += sock_uring
endif
endif

ACCEL_MODULES_LIST = accel_ioat ioat
ifeq ($(CONFIG_IDXD),y)
ACCEL_MODULES_LIST += accel_idxd idxd
endif

EVENT_BDEV_SUBSYSTEM = event_bdev event_accel event_vmd event_sock

ALL_MODULES_LIST = $(BLOCKDEV_MODULES_LIST) $(ACCEL_MODULES_LIST) $(SOCK_MODULES_LIST)
