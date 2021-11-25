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

PKG_CONFIG_PATH = $(SPDK_LIB_DIR)/pkgconfig

DPDK_LIB := $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --libs spdk_env_dpdk)
SPDK_EVENT_LIB := $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --libs spdk_event spdk_event_bdev)
SPDK_DPDK_LIB := $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --libs spdk_event spdk_event_bdev spdk_env_dpdk)
SYS_LIB := $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --libs --static spdk_syslibs)

# Shows how to compile both an external bdev and an external application against the SPDK combined shared object and dpdk shared objects.
bdev_shared_combo:
	$(CC) $(COMMON_CFLAGS) -L../passthru -Wl,-rpath=$(SPDK_LIB_DIR),--no-as-needed -o hello_bdev ./hello_bdev.c -lpassthru_external \
	-lspdk $(DPDK_LIB) -Wl,--no-whole-archive

# Shows how to compile both an external bdev and an external application against the SPDK individual shared objects and dpdk shared objects.
bdev_shared_iso:
	$(CC) $(COMMON_CFLAGS) -L../passthru -Wl,--no-as-needed -o hello_bdev ./hello_bdev.c \
	-lpassthru_external $(SPDK_EVENT_LIB) \
	$(DPDK_LIB)

# Shows how to compile an external application against the SPDK combined shared object and dpdk shared objects.
alone_shared_combo:
	$(CC) $(COMMON_CFLAGS) -Wl,-rpath=$(SPDK_LIB_DIR),--no-as-needed -o hello_bdev ./hello_bdev.c -lspdk $(DPDK_LIB)

# Shows how to compile an external application against the SPDK individual shared objects and dpdk shared objects.
alone_shared_iso:
	$(CC) $(COMMON_CFLAGS) -Wl,-rpath=$(SPDK_LIB_DIR),--no-as-needed -o hello_bdev ./hello_bdev.c \
	$(SPDK_EVENT_LIB) $(DPDK_LIB)

# Shows how to compile an external application against the SPDK archives.
alone_static:
	$(CC) $(COMMON_CFLAGS) -o hello_bdev ./hello_bdev.c -pthread -Wl,--whole-archive,-Bstatic \
	$(SPDK_DPDK_LIB) -Wl,--no-whole-archive,-Bdynamic $(SYS_LIB)

# Shows how to compile and external bdev and application against the SPDK archives.
bdev_static:
	$(CC) $(COMMON_CFLAGS) -L../passthru -o hello_bdev ./hello_bdev.c -pthread -Wl,--whole-archive,-Bstatic -lpassthru_external $(SPDK_DPDK_LIB) \
	-Wl,--no-whole-archive,-Bdynamic $(SYS_LIB)
