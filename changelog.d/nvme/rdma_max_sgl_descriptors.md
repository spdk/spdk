`NVME_RDMA_MAX_SGL_DESCRIPTORS`, the compile-time upper bound for the NVMe/RDMA initiator's SGL
descriptor arrays, now follows `SPDK_CONFIG_NVMF_MAX_SGL_ENTRIES` (build-time configurable via
`--max-nvmf-sgl-entries`, default 16). The effective per-controller value is still limited at
runtime by `nvme_rdma_ctrlr_get_max_sges()` to the target's advertised MSDBD and the HCA's
`max_sge`, so behavior is unchanged unless the build option is raised.
