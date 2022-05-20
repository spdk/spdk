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

## Pre-defined configuration {#idxd_configs}

The low level library can be initialized either directly via `spdk_idxd_set_config` or
through an RPC via one of the accelerator framework modules that rely on the low level
IDXD library.  Either way, the underlying hardware will be set to the pre-defined
hardware configuration below:

Config: 1 group, 1 work queue, 4 engines in the group.
