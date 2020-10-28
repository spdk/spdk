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

DPDK_LIB = -lrte_eal -lrte_mempool -lrte_ring -lrte_mbuf -lrte_mempool_ring -lrte_pci
DPDK_LIB += -lrte_bus_pci -lrte_kvargs -lrte_vhost -lrte_net -lrte_hash -lrte_telemetry
DPDK_LIB += -lrte_cryptodev -lrte_power -lrte_rcu

# Shows how to compile both an external bdev and an external application against the SPDK combined shared object and dpdk shared objects.
bdev_shared_combo:
	$(CC) $(COMMON_CFLAGS) -L../passthru -Wl,-rpath=$(SPDK_LIB_DIR),--no-as-needed -o hello_bdev ./hello_bdev.c -lpassthru_external \
	-lspdk -lspdk_env_dpdk $(DPDK_LIB) -Wl,--no-whole-archive

# Shows how to compile both an external bdev and an external application against the SPDK individual shared objects and dpdk shared objects.
bdev_shared_iso:
	$(CC) $(COMMON_CFLAGS) -L../passthru -Wl,-rpath=$(SPDK_LIB_DIR),--no-as-needed -o hello_bdev ./hello_bdev.c \
	-lpassthru_external -lspdk_event_bdev -lspdk_event_accel -lspdk_event_vmd -lspdk_bdev -lspdk_bdev_malloc -lspdk_log -lspdk_thread -lspdk_util -lspdk_event \
	-lspdk_env_dpdk $(DPDK_LIB) -Wl,--no-whole-archive -lnuma

# Shows how to compile an external application against the SPDK combined shared object and dpdk shared objects.
alone_shared_combo:
	$(CC) $(COMMON_CFLAGS) -Wl,-rpath=$(SPDK_LIB_DIR),--no-as-needed -o hello_bdev ./hello_bdev.c -lspdk -lspdk_env_dpdk $(DPDK_LIB)

# Shows how to compile an external application against the SPDK individual shared objects and dpdk shared objects.
alone_shared_iso:
	$(CC) $(COMMON_CFLAGS) -Wl,-rpath=$(SPDK_LIB_DIR),--no-as-needed -o hello_bdev ./hello_bdev.c -lspdk_event_bdev \
	-lspdk_event_accel -lspdk_event_vmd -lspdk_bdev -lspdk_bdev_malloc -lspdk_log -lspdk_thread -lspdk_util -lspdk_event -lspdk_env_dpdk $(DPDK_LIB)

# Shows how to compile an external application against the SPDK archives.
alone_static:
	$(CC) $(COMMON_CFLAGS) -o hello_bdev ./hello_bdev.c -Wl,--whole-archive,-Bstatic -lspdk_bdev_malloc -lspdk_event_bdev -lspdk_event_accel -lspdk_event_vmd \
	-lspdk_event_sock -lspdk_bdev -lspdk_accel -lspdk_event -lspdk_thread -lspdk_util -lspdk_conf -lspdk_trace -lspdk_log -lspdk_json \
	-lspdk_jsonrpc -lspdk_rpc -lspdk_sock -lspdk_notify -lspdk_vmd -lspdk_env_dpdk \
	$(DPDK_LIB) -Wl,--no-whole-archive,-Bdynamic -lnuma -luuid -lpthread -ldl -lrt

# Shows how to compile and external bdev and application sgainst the SPDK archives.
bdev_static:
	$(CC) $(COMMON_CFLAGS) -L../passthru -o hello_bdev ./hello_bdev.c -Wl,--whole-archive,-Bstatic -lpassthru_external -lspdk_bdev_malloc -lspdk_event_bdev \
	-lspdk_event_accel -lspdk_event_vmd -lspdk_event_sock -lspdk_bdev -lspdk_accel -lspdk_event -lspdk_thread -lspdk_util -lspdk_conf -lspdk_trace \
	-lspdk_log -lspdk_json -lspdk_jsonrpc -lspdk_rpc -lspdk_sock -lspdk_notify -lspdk_vmd -lspdk_env_dpdk $(DPDK_LIB) \
	-Wl,--no-whole-archive,-Bdynamic -lnuma -luuid -lpthread -ldl -lrt
