`spdk_ae4dma_build_copy()` now takes an optional `struct spdk_memory_domain` for
each endpoint (source and destination) in place of a per-call copy-type enum. A
NULL domain means the iovec holds host virtual addresses (translated via
`spdk_vtophys()`); a non-NULL domain means the addresses are already resolved in
that device memory domain (for example a peer NTB BAR window from
`spdk_ntb_amd_get_peer_mw()`) and are passed to the engine unchanged. Both the
address translation and the descriptor's memory-type bits are derived from the
same domains so they cannot disagree. SO_VER has been bumped for the signature
change.
