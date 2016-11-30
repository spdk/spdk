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

# RPC libraries only export constructor functions, so these need to be treated
#  separately and wrapped in whole-archive linker args
SPDK_RPC_LIB_LIST = $(filter %_rpc,$(SPDK_LIB_LIST))

# Currently the iscsi, net, and scsi libraries contain their respective RPC methods
#  rather than breaking them out into separate libraries.  So we must also include
#  these directories in the RPC library list.
SPDK_RPC_LIB_LIST += $(filter iscsi,$(SPDK_LIB_LIST))
SPDK_RPC_LIB_LIST += $(filter net,$(SPDK_LIB_LIST))
SPDK_RPC_LIB_LIST += $(filter scsi,$(SPDK_LIB_LIST))

SPDK_REMAINING_LIB_LIST = $(filter-out $(SPDK_RPC_LIB_LIST),$(SPDK_LIB_LIST))

SPDK_LIB_FILES = $(call spdk_lib_list_to_files,$(SPDK_LIB_LIST))
SPDK_LIB_LINKER_ARGS = \
	-L$(SPDK_ROOT_DIR)/build/lib \
	-Wl,--whole-archive \
	$(SPDK_RPC_LIB_LIST:%=-lspdk_%) \
	-Wl,--no-whole-archive \
	$(SPDK_REMAINING_LIB_LIST:%=-lspdk_%)
