Renamed `lib/rdma_provider` to `lib/rdma`. Public symbols `spdk_rdma_provider_*` are now
`spdk_rdma_*` and the internal header `spdk_internal/rdma_provider.h` is now
`spdk_internal/rdma.h`. The library is now `libspdk_rdma` and its pkg-config file is
`spdk_rdma.pc`, so applications that link `spdk_rdma_provider` by name must be updated.
