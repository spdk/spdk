# IDXD Driver {#idxd}

## Public Interface {#idxd_interface}

- spdk/idxd.h

## Key Functions {#idxd_key_functions}

Function                                | Description
--------------------------------------- | -----------
spdk_idxd_probe()                       | @copybrief spdk_idxd_probe()
spdk_idxd_submit_copy()                 | @copybrief spdk_idxd_submit_copy()
spdk_idxd_submit_compare()              | @copybrief spdk_idxd_submit_compare()
spdk_idxd_submit_crc32c()               | @copybrief spdk_idxd_submit_crc32c()
spdk_idxd_submit_dualcast               | @copybrief spdk_idxd_submit_dualcast()
spdk_idxd_submit_fill()                 | @copybrief spdk_idxd_submit_fill()

## Pre-defined configurations {#idxd_configs}

The RPC `idxd_scan_accel_engine` is used to both enable IDXD and set it's
configuration to one of two pre-defined configs:

Config #0: 4 groups, 1 work queue per group, 1 engine per group.
Config #1: 2 groups, 2 work queues per group, 2 engines per group.
