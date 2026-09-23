Removed `spdk_nvme_rdma_init_hooks()` and `struct spdk_nvme_rdma_hooks`. The RDMA transport
always allocates its own protection domains and registers memory itself. The hooks passed
verbs objects through the public API.
