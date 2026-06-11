Added a user-space driver for the AMD EPYC 4th-Generation Non-Transparent Bridge
(NTB). The new `lib/ntb_amd` library handles device probe/attach, BAR mapping,
memory windows, scratchpads, doorbells, link state and the peer-side handshake.
The public API lives in `include/spdk/ntb_amd.h` (spec definitions in
`include/spdk/ntb_amd_spec.h`).

A peer memory window is described to callers via `spdk_ntb_amd_get_peer_mw()`,
which returns a `struct iovec` (device bus address + size) together with an
`spdk_memory_domain` and an optional CPU-mapped virtual address. The pair can be
handed directly to `spdk_ae4dma_build_copy()` as a DMA endpoint. The remaining
introspection keys are `SPDK_NTB_*`-prefixed.

A reference application exercising the remote memory window with either CPU
`memcpy` or AE4DMA-offloaded copies ships as `examples/ntb/perf`; see
`doc/ntb_amd.md` for the topology model and two-host bring-up procedure.
