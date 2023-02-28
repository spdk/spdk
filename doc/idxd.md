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

## Kernel vs User {#idxd_configs}

The low level library can be initialized either directly via `spdk_idxd_set_config`,
passing in a value of `true` indicates that the IDXD kernel driver is loaded and
that SPDK will use work queue(s) surfaced by the driver.  Passing in `false` means
that the SPDK user space driver will be used to initialize the hardware.
