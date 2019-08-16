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

# Module directory dependencies. These take a slightly different form than the
# library ones. For the top level directories, we only specify other directories
# in the module directory. Then, for each individual library, we specify all of
# the libs on which they depend.
DEPDIRS-blob :=
DEPDIRS-blob_bdev := bdev log thread
DEPDIRS-copy :=
DEPDIRS-copy_ioat := conf copy ioat json jsonrpc log rpc thread
DEPDIRS-sock :=
DEPDIRS-sock_posix := log sock
DEPDIRS-sock_vpp := log sock thread util

DEPDIRS-bdev := blob
DEPDIRS-bdev_aio := bdev conf json jsonrpc log rpc thread util
DEPDIRS-bdev_compress := bdev json jsonrpc log reduce rpc thread util
DEPDIRS-bdev_crypto := bdev conf json jsonrpc log rpc thread util
DEPDIRS-bdev_delay := bdev json jsonrpc log rpc thread util
DEPDIRS-bdev_error := bdev conf json jsonrpc log rpc util
DEPDIRS-bdev_gpt := bdev conf json log rpc thread util
DEPDIRS-bdev_iscsi := bdev conf json jsonrpc log rpc thread util
DEPDIRS-bdev_lvol := bdev blob_bdev blob json jsonrpc log lvol rpc util
DEPDIRS-bdev_malloc := bdev conf copy json jsonrpc log rpc util
DEPDIRS-bdev_null := bdev conf json jsonrpc log rpc thread util
DEPDIRS-bdev_nvme = bdev conf json jsonrpc log nvme rpc thread util
ifeq ($(OS),Linux)
DEPDIRS-bdev_nvme += ftl
endif
DEPDIRS-bdev_passthru = bdev conf json jsonrpc log rpc thread util
DEPDIRS-bdev_pmem = bdev conf json jsonrpc log rpc thread util
DEPDIRS-bdev_raid = bdev conf json jsonrpc log rpc thread util
DEPDIRS-bdev_rbd = bdev conf json jsonrpc log rpc thread util
DEPDIRS-bdev_rpc = bdev json jsonrpc log rpc util
DEPDIRS-bdev_split = bdev conf json jsonrpc log rpc util
DEPDIRS-bdev_virtio = bdev conf json jsonrpc log rpc thread util virtio

DEPDIRS-event := bdev blob
DEPDIRS-app_rpc := event json jsonrpc rpc thread util
DEPDIRS-event_bdev := bdev event
DEPDIRS-event_copy := copy event
DEPDIRS-event_iscsi := event iscsi
DEPDIRS-event_nbd := event nbd
DEPDIRS-event_net := event net sock
DEPDIRS-event_nvmf = bdev conf event json jsonrpc log nvme nvmf rpc thread util
DEPDIRS-event_scsi = event scsi
DEPDIRS-event_vhost = event vhost
DEPDIRS-event_vmd = conf event vmd
