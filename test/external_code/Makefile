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

ALL_DEPDIRS+= hello_world passthru

ifneq ($(SPDK_HEADER_DIR),)
COMMON_CFLAGS+=-I$(SPDK_HEADER_DIR)
endif

ifneq ($(SPDK_LIB_DIR),)
COMMON_CFLAGS+=-L$(SPDK_LIB_DIR)
endif

ifneq ($(DPDK_LIB_DIR),)
COMMON_CFLAGS+=-L$(DPDK_LIB_DIR)
endif
export
.PHONY: all

all: hello_world_bdev_shared_combo

static: hello_world_bdev_static

hello_world_bdev_shared_combo: passthru_shared
	$(MAKE) --directory=hello_world bdev_shared_combo

hello_world_bdev_shared_iso: passthru_shared
	$(MAKE) --directory=hello_world bdev_shared_iso

hello_world_no_bdev_shared_combo:
	$(MAKE) --directory=hello_world alone_shared_combo

hello_world_no_bdev_shared_iso:
	$(MAKE) --directory=hello_world alone_shared_iso

hello_world_bdev_static: passthru_static
	$(MAKE) --directory=hello_world bdev_static

hello_world_no_bdev_static:
	$(MAKE) --directory=hello_world alone_static

passthru_shared:
	$(MAKE) --directory=passthru shared

passthru_static:
	$(MAKE) --directory=passthru static

clean:
	rm -f ./hello_world/hello_bdev
	rm -f ./passthru/libpassthru_external.*
