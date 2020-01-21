# Shows how to compile both an external bdev and an external application against the SPDK combined shared object and dpdk shared object.
bdev_shared_combo:
	gcc $(COMMON_CFLAGS) -L../passthru -o hello_bdev ./hello_bdev.c -lpassthru_external -lspdk -lspdk_env_dpdk -ldpdk
# Shows how to compile both an external bdev and an external application against the SPDK individual shared objects and dpdk shared object.
bdev_shared_iso:
	gcc $(COMMON_CFLAGS) -L../passthru -Wl,-rpath=$(SPDK_LIB_DIR),--no-as-needed -o hello_bdev ./hello_bdev.c \
	-lpassthru_external -lspdk_event_bdev -lspdk_bdev -lspdk_bdev_malloc -lspdk_log -lspdk_thread -lspdk_util -lspdk_event -lspdk_env_dpdk -ldpdk
# Shows how to compile an external application against the SPDK combined shared object and dpdk shared object.
alone_shared_combo:
	gcc $(COMMON_CFLAGS) -o hello_bdev ./hello_bdev.c -lspdk -lspdk_env_dpdk -ldpdk
# Shows how to compile an external application against the SPDK individual shared objects and dpdk shared object.
alone_shared_iso:
	gcc $(COMMON_CFLAGS) -Wl,-rpath=$(SPDK_LIB_DIR),--no-as-needed -o hello_bdev ./hello_bdev.c -lspdk_event_bdev \
	-lspdk_bdev -lspdk_bdev_malloc -lspdk_log -lspdk_thread -lspdk_util -lspdk_event -lspdk_env_dpdk -ldpdk
# Shows how to compile an external application against the SPDK archives.
alone_static:
	gcc $(COMMON_CFLAGS) -o hello_bdev ./hello_bdev.c -Wl,--whole-archive -lspdk_bdev_malloc -lspdk_event_bdev -lspdk_event_copy -lspdk_event_vmd \
	-lspdk_bdev -lspdk_copy -lspdk_event -lspdk_thread -lspdk_util -lspdk_conf -lspdk_trace -lspdk_log -lspdk_json \
	-lspdk_jsonrpc -lspdk_rpc -lspdk_sock -lspdk_notify -lspdk_vmd -lspdk_env_dpdk -lrte_eal -lrte_mempool -lrte_ring \
	-lrte_mbuf -lrte_mempool_ring -lrte_pci -lrte_bus_pci -lrte_kvargs -lrte_vhost -lrte_net -lrte_hash \
	-lrte_cryptodev -Wl,--no-whole-archive -lnuma -luuid -lpthread -ldl -lrt
# Shows how to compile and external bdev and application sgainst the SPDK archives.
bdev_static:
	gcc $(COMMON_CFLAGS) -L../passthru -o hello_bdev ./hello_bdev.c -Wl,--whole-archive -lpassthru_external -lspdk_bdev_malloc -lspdk_event_bdev \
	-lspdk_event_copy -lspdk_event_vmd -lspdk_bdev -lspdk_copy -lspdk_event -lspdk_thread -lspdk_util -lspdk_conf -lspdk_trace \
	-lspdk_log -lspdk_json -lspdk_jsonrpc -lspdk_rpc -lspdk_sock -lspdk_notify -lspdk_vmd -lspdk_env_dpdk -lrte_eal -lrte_mempool \
	-lrte_ring -lrte_mbuf -lrte_mempool_ring -lrte_pci -lrte_bus_pci -lrte_kvargs -lrte_vhost -lrte_net -lrte_hash -lrte_cryptodev \
	-Wl,--no-whole-archive -lnuma -luuid -lpthread -ldl -lrt