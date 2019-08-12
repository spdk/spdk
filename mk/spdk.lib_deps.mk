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

# A quick note on organization:
#
# Each grouping is independent from itself. it depends only on libraries
# in the grouping above it. All dependencies are listed alphabetically within
# groups. The only exception to this is the JSON_LIBS grouping which is a special
# case since they almost always occur together.

JSON_LIBS := json jsonrpc rpc

DEPDIRS-log :=

DEPDIRS-ioat := log
DEPDIRS-sock := log
DEPDIRS-util := log
DEPDIRS-vmd := log

DEPDIRS-conf := log util
DEPDIRS-json := log util
DEPDIRS-nvme := log sock util
DEPDIRS-reduce := log util
DEPDIRS-thread := log util

DEPDIRS-blob := log util thread
DEPDIRS-copy := thread
DEPDIRS-jsonrpc := log util json
DEPDIRS-virtio := log util json thread

DEPDIRS-lvol := log util blob
DEPDIRS-rpc := log util json jsonrpc

DEPDIRS-log_rpc := log $(JSON_LIBS)
DEPDIRS-net := log util $(JSON_LIBS)
DEPDIRS-notify := log util $(JSON_LIBS)
DEPDIRS-trace := log util $(JSON_LIBS)

DEPDIRS-bdev := log util conf thread $(JSON_LIBS) notify trace
DEPDIRS-blobfs := log conf thread blob trace
DEPDIRS-event := log util conf thread $(JSON_LIBS) trace

DEPDIRS-ftl := log util nvme thread trace bdev
DEPDIRS-nbd := log util thread $(JSON_LIBS) bdev
DEPDIRS-nvmf := log sock util nvme thread $(JSON_LIBS) trace bdev
DEPDIRS-scsi := log util thread $(JSON_LIBS) trace bdev

DEPDIRS-iscsi := log sock util conf thread $(JSON_LIBS) trace event scsi
DEPDIRS-vhost := log util conf thread $(JSON_LIBS) bdev event scsi