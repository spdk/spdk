# SPDK Porting Guide {#porting}

SPDK is ported to new environments by implementing the *env*
library interface.  The *env* interface provides APIs for drivers
to allocate physically contiguous and pinned memory, perform PCI
operations (config cycles and mapping BARs), virtual to physical
address translation and managing memory pools.  The *env* API is
defined in include/spdk/env.h.

SPDK includes a default implementation of the *env* library based
on the Data Plane Development Kit ([DPDK](http://dpdk.org/)).
This DPDK implementation can be found in `lib/env_dpdk`.

DPDK is currently supported on Linux and FreeBSD only.
Users who want to use SPDK on other operating systems, or in
userspace driver frameworks other than DPDK, will need to implement
a new version of the *env* library.  The new implementation can be
integrated into the SPDK build by updating the following line
in CONFIG:

    CONFIG_ENV?=$(SPDK_ROOT_DIR)/lib/env_dpdk
