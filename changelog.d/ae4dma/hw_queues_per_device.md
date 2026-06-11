`spdk_ae4dma_probe()` gained a leading `uint8_t hw_queues_per_device` parameter.
Pass `0` to keep the previous "use the device maximum" behaviour; out-of-range
values are clamped with a warning. `spdk_ae4dma_detach()` now tears down only
the queues that were actually started, fixing a spurious destruct loop on
partial attaches.
