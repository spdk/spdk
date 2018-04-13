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

LVOL_MODULES_LIST = vbdev_lvol
# Modules below are added as dependency for vbdev_lvol
LVOL_MODULES_LIST += blob blob_bdev lvol

BLOCKDEV_MODULES_LIST = $(LVOL_MODULES_LIST)
BLOCKDEV_MODULES_LIST += bdev_malloc bdev_null bdev_nvme nvme vbdev_passthru vbdev_error vbdev_gpt vbdev_split

ifeq ($(CONFIG_RDMA),y)
BLOCKDEV_MODULES_DEPS += -libverbs -lrdmacm
endif

ifeq ($(OS),Linux)
BLOCKDEV_MODULES_LIST += bdev_aio bdev_virtio virtio
BLOCKDEV_MODULES_DEPS += -laio
ifeq ($(CONFIG_ISCSI_INITIATOR),y)
BLOCKDEV_MODULES_LIST += bdev_iscsi
# Fedora installs libiscsi to /usr/lib64/iscsi for some reason.
BLOCKDEV_MODULES_DEPS += -L/usr/lib64/iscsi -liscsi
endif
endif

ifeq ($(CONFIG_RBD),y)
BLOCKDEV_MODULES_LIST += bdev_rbd
BLOCKDEV_MODULES_DEPS += -lrados -lrbd
endif

ifeq ($(CONFIG_PMDK),y)
BLOCKDEV_MODULES_LIST += bdev_pmem
BLOCKDEV_MODULES_DEPS += -lpmemblk
endif

NET_MODULES_LIST = net
NET_MODULES_LIST += net_posix

ifeq ($(CONFIG_VPP),y)
ifneq ($(CONFIG_VPP_DIR),)
NET_MODULES_DEPS = -l:libvppinfra.a -l:libsvm.a -l:libvapiclient.a
NET_MODULES_DEPS += -l:libvppcom.a -l:libvlibmemoryclient.a
else
NET_MODULES_DEPS = -lvppcom
endif
NET_MODULES_LIST += net_vpp
endif

COPY_MODULES_LIST = copy_ioat ioat

BLOCKDEV_MODULES_LINKER_ARGS = -Wl,--whole-archive \
			       $(BLOCKDEV_MODULES_LIST:%=-lspdk_%) \
			       -Wl,--no-whole-archive \
			       $(BLOCKDEV_MODULES_DEPS)

BLOCKDEV_MODULES_FILES = $(call spdk_lib_list_to_files,$(BLOCKDEV_MODULES_LIST))

BLOCKDEV_NO_LVOL_MODULES_LIST = $(filter-out $(LVOL_MODULES_LIST),$(BLOCKDEV_MODULES_LIST))
BLOCKDEV_NO_LVOL_MODULES_LINKER_ARGS = -Wl,--whole-archive \
			       $(BLOCKDEV_NO_LVOL_MODULES_LIST:%=-lspdk_%) \
			       -Wl,--no-whole-archive \
			       $(BLOCKDEV_MODULES_DEPS)

BLOCKDEV_NO_LVOL_MODULES_FILES = $(call spdk_lib_list_to_files,$(BLOCKDEV_NO_LVOL_MODULES_LIST))

COPY_MODULES_LINKER_ARGS = -Wl,--whole-archive \
			   $(COPY_MODULES_LIST:%=-lspdk_%) \
			   -Wl,--no-whole-archive \
			   $(COPY_MODULES_DEPS)

COPY_MODULES_FILES = $(call spdk_lib_list_to_files,$(COPY_MODULES_LIST))

NET_MODULES_LINKER_ARGS = -Wl,--whole-archive \
			   $(NET_MODULES_LIST:%=-lspdk_%) \
			   $(NET_MODULES_DEPS) \
			   -Wl,--no-whole-archive

NET_MODULES_FILES = $(call spdk_lib_list_to_files,$(NET_MODULES_LIST))
